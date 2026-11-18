"""Portable source materialization and explicit development snapshots."""
import os
import hashlib
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile


def source_revision(root, development=False):
    """Return a committed base and an exact tree; never stage into the user's index."""
    def git(*args, env=None):
        return subprocess.check_output(["git", "-C", str(root), *args], env=env)

    commit = git("rev-parse", "HEAD").decode().strip()
    if not development:
        git("diff", "--exit-code", "HEAD", "--")
        return commit, git("rev-parse", "HEAD^{tree}").decode().strip()

    with tempfile.TemporaryDirectory(prefix="rime-source-index-") as temporary:
        index = Path(temporary) / "index"
        environment = dict(os.environ, GIT_INDEX_FILE=str(index))
        git("read-tree", "HEAD", env=environment)
        # Keep gitlinks at their recorded pins; Android dependencies come from the lock.
        gitlinks = {entry.split(b"\t", 1)[1] for entry in git("ls-files", "--stage", "-z").split(b"\0")
                    if entry.startswith(b"160000 ")}
        changed = git("diff", "--no-renames", "--name-only", "-z", "HEAD", "--").split(b"\0")
        untracked = git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
        paths = sorted((set(changed) | set(untracked)) - gitlinks - {b""})
        if paths:
            pathspec = Path(temporary) / "paths"
            pathspec.write_bytes(b"\0".join(b":(literal)" + path for path in paths) + b"\0")
            git("add", "-A", f"--pathspec-from-file={pathspec}", "--pathspec-file-nul", env=environment)
        return commit, git("write-tree", env=environment).decode().strip()


def extract_sources(archive, destination, copy_links=None):
    """Preserve data-filter checks without requiring Windows symlink privileges."""
    destination = Path(destination).resolve()
    if copy_links is None:
        copy_links = os.name == "nt"
    if not copy_links:
        archive.extractall(destination, filter="data")
        return
    links = []
    for member in archive:
        checked = tarfile.data_filter(member, str(destination))
        if checked is None:
            continue
        if checked.issym() or checked.islnk():
            links.append(checked)
        else:
            archive.extract(checked, destination, filter="data")
    while links:
        unresolved = []
        for member in links:
            output = (destination / member.name).resolve()
            target = ((output.parent if member.issym() else destination) / member.linkname).resolve()
            if not output.is_relative_to(destination) or not target.is_relative_to(destination):
                raise ValueError("Source link escapes its extraction directory")
            # Do not copy an ancestor into itself or an incompletely materialized directory.
            if output == target or output.is_relative_to(target):
                raise ValueError("Cyclic source link")
            if not target.exists() or any((destination / other.name).is_relative_to(target)
                                          for other in links if other is not member):
                unresolved.append(member)
                continue
            output.parent.mkdir(parents=True, exist_ok=True)
            if target.is_dir():
                shutil.copytree(target, output)
            else:
                shutil.copy2(target, output)
        if len(unresolved) == len(links):
            raise ValueError("Unresolved or cyclic source links")
        links = unresolved


def ndk_tools(ndk):
    host = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system())
    if host is None:
        raise ValueError(f"Unsupported build host: {platform.system()}")
    suffix = ".exe" if os.name == "nt" else ""
    tools = Path(ndk) / "toolchains/llvm/prebuilt" / host
    return tools, suffix


def host_compiler_flags():
    cc, cxx = os.environ.get("RIME_HOST_CC"), os.environ.get("RIME_HOST_CXX")
    if bool(cc) != bool(cxx):
        raise ValueError("Set both RIME_HOST_CC and RIME_HOST_CXX")
    if not cc:
        return []
    if not Path(cc).is_file() or not Path(cxx).is_file():
        raise ValueError("The configured host C/C++ compiler is missing")
    flags = [f"-DCMAKE_C_COMPILER={Path(cc).as_posix()}", f"-DCMAKE_CXX_COMPILER={Path(cxx).as_posix()}"]
    if Path(cc).name.lower() == "cl.exe":
        flags += ["-DCMAKE_C_FLAGS=/utf-8", "-DCMAKE_CXX_FLAGS=/utf-8",
                  "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"]
        for variable, option in (("RIME_HOST_RC", "CMAKE_RC_COMPILER"), ("RIME_HOST_MT", "CMAKE_MT")):
            value = os.environ.get(variable)
            if not value or not Path(value).is_file():
                raise ValueError(f"Missing Windows SDK tool: {variable}")
            flags.append(f"-D{option}={Path(value).as_posix()}")
    return flags


def sync_source_tree(source, destination):
    """Refresh an owned source snapshot without invalidating unchanged C++ inputs."""
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if source == destination or source.is_relative_to(destination) or destination.is_relative_to(source):
        raise ValueError("Source snapshot directories must be separate")
    destination.mkdir(parents=True, exist_ok=True)
    for old in sorted(destination.rglob("*"), key=lambda path: len(path.parts), reverse=True):
        corresponding = source / old.relative_to(destination)
        if old.is_symlink() or not corresponding.exists() or old.is_dir() != corresponding.is_dir():
            if old.is_dir() and not old.is_symlink():
                old.rmdir()
            else:
                old.unlink()
    for incoming in source.rglob("*"):
        target = destination / incoming.relative_to(source)
        if incoming.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        identical = False
        if target.is_file() and incoming.stat().st_size == target.stat().st_size:
            with incoming.open("rb") as first, target.open("rb") as second:
                identical = hashlib.file_digest(first, "sha256").digest() == hashlib.file_digest(second, "sha256").digest()
        if not identical:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(incoming, target)
            shutil.copymode(incoming, target)
            os.utime(target, None)
