#!/usr/bin/env python3
"""Build the complete Android SDK from pinned sources, without prebuilt inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import urllib.request
import re
from local import extract_sources, host_compiler_flags, ndk_tools, source_revision, sync_source_tree

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
LOCK = HERE / "sources.lock.json"
LIBRARIES = {
    "librime": "librime.a", "glog": "libglog.a", "leveldb": "libleveldb.a",
    "marisa": "libmarisa.a", "opencc": "libopencc.a", "yaml-cpp": "libyaml-cpp.a",
}
EXTERNAL_PLUGINS = {
    "librime-octagram": "octagram",
    "librime-predict": "predict",
}
MERGED_PLUGINS = ("librime-lua", "octagram", "predict")
BUNDLED_LUA = Path("plugins/librime-lua")


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def source_lock_digest():
    # Git's canonical text bytes are LF even on a CRLF Windows checkout.
    return hashlib.sha256(LOCK.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def core_source_key(source_tree, locked_sources):
    inputs = {
        "source_tree": source_tree,
        "merged_plugins": MERGED_PLUGINS,
        "external_plugins": {
            name: locked_sources[name]
            for name in EXTERNAL_PLUGINS
        },
    }
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def apply_locked_patch(target, path, expected):
    content = path.read_bytes().replace(b"\r\n", b"\n")
    if hashlib.sha256(content).hexdigest() != expected:
        raise ValueError(f"Patch checksum mismatch: {path.name}")
    with tempfile.TemporaryDirectory(prefix="rime-patch-") as temporary:
        normalized = Path(temporary) / path.name
        normalized.write_bytes(content)
        apply_patch(target, normalized)


def apply_patch(target, patch):
    # An extracted source directory may be nested below this repository's
    # build/. Without a discovery ceiling, git apply treats it as a subdirectory
    # of the outer checkout and silently skips patches whose paths lack that
    # prefix. Apply against the extracted tree in both local and CI layouts.
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(target.parent))
    run("git", "-c", "core.autocrlf=false", "apply", "--check", patch, cwd=target, env=environment)
    run("git", "-c", "core.autocrlf=false", "apply", patch, cwd=target, env=environment)


def download(item, cache):
    target = cache / item["sha256"]
    if target.exists() and sha256(target) == item["sha256"]:
        return target
    for attempt in range(4):
        try:
            with urllib.request.urlopen(item["url"], timeout=120) as response:
                with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
                    temporary = Path(output.name)
                    shutil.copyfileobj(response, output)
            if sha256(temporary) != item["sha256"]:
                raise ValueError(f"Source checksum mismatch: {item['url']}")
            temporary.replace(target)
            return target
        except Exception:
            if "temporary" in locals():
                temporary.unlink(missing_ok=True)
            if attempt == 3:
                raise
            time.sleep(2 ** attempt)


def prepare_source(name, item, sources, cache, patches=True):
    target = sources / name
    key = hashlib.sha256(json.dumps([item, patches], sort_keys=True).encode()).hexdigest()
    if target.exists():
        stamp = target / ".android-source"
        if stamp.is_file() and stamp.read_text() == key:
            return target
        # A failed patch or interrupted extraction can leave a directory without
        # a completion stamp. Reusing it would nest the fresh archive below the
        # stale tree and compile inputs that do not match the source lock.
        shutil.rmtree(target)
    with tempfile.TemporaryDirectory(dir=sources) as temporary:
        with tarfile.open(download(item, cache)) as archive:
            extract_sources(archive, temporary)
        roots = list(Path(temporary).iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise ValueError(f"Expected one source root: {name}")
        shutil.move(roots[0], target)
    if patches:
        for patch in item["patches"]:
            # OpenCC uses separate source trees for host data and Android library.
            if patch["file"] == "opencc-host.patch":
                continue
            path = HERE / "patches" / patch["file"]
            apply_locked_patch(target, path, patch["sha256"])
    (target / ".android-source").write_text(key)
    return target


def cmake_build(cmake, source, build, prefix, jobs, flags=(), targets=(), install=True,
                environment_overrides=None):
    # Source archives have no Git metadata. Never let a dependency's version
    # detection discover the enclosing librime checkout in CI's build/ tree.
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(source.parent))
    if environment_overrides:
        environment.update(environment_overrides)
    run(cmake, "-S", source, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        f"-DCMAKE_INSTALL_PREFIX={prefix.as_posix()}", "-DCMAKE_INSTALL_LIBDIR=lib", *flags,
        env=environment)
    run(cmake, "--build", build, "--parallel", jobs,
        *(["--target", *targets] if targets else []), env=environment)
    if install:
        run(cmake, "--install", build, env=environment)


def verify_opencc_version(build, expected):
    versions = [line.split(":", 1)[1].strip() for line in (build / "opencc.pc").read_text().splitlines()
                if line.startswith("Version:")]
    if versions != [expected]:
        raise ValueError(f"OpenCC version does not match its locked source: {versions}, expected {expected}")


def prepare_boost_headers(source, include, source_sha256):
    stamp = include / ".android-source"
    if stamp.exists() and stamp.read_text() == source_sha256:
        return
    if include.exists():
        shutil.rmtree(include)
    include.mkdir(parents=True)
    for directory in sorted((source / "libs").rglob("include/boost")):
        shutil.copytree(directory, include / "boost", dirs_exist_ok=True)
    stamp.write_text(source_sha256)


def rime_version(source):
    match = re.search(r"^set\(rime_version\s+([^\s)]+)\s*\)$",
                      (Path(source) / "CMakeLists.txt").read_text(), re.MULTILINE)
    if not match or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}(?:[-+][0-9A-Za-z.-]+)?", match.group(1)):
        raise ValueError("Could not read a safe rime_version from CMakeLists.txt")
    return match.group(1)


def verify_core_build_graph(build):
    graph = (Path(build) / "build.ninja").read_text().replace("\\", "/")
    required = (
        "plugins/librime-lua/src/",
        "plugins/librime-lua/thirdparty/lua5.4/",
        "plugins/octagram/src/",
        "plugins/predict/src/",
    )
    missing = [path for path in required if path not in graph]
    if missing or "plugins/lua/" in graph:
        raise ValueError(
            f"Unexpected merged-plugin build graph; missing={missing}, "
            f"legacy_lua={'plugins/lua/' in graph}"
        )


def lua_version(source):
    header = (Path(source) / BUNDLED_LUA / "thirdparty/lua5.4/lua.h").read_text()
    parts = dict(re.findall(
        r'^#define LUA_VERSION_(MAJOR|MINOR|RELEASE)\s+"([0-9]+)"\s*$',
        header,
        re.MULTILINE,
    ))
    if set(parts) != {"MAJOR", "MINOR", "RELEASE"}:
        raise ValueError("Could not read the bundled Lua version")
    return ".".join(parts[name] for name in ("MAJOR", "MINOR", "RELEASE"))


def copy_licenses(sources, core, product, ndk):
    destination = product / "licenses"
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir()
    for name, source in sources.items():
        directory = destination / name
        for path in source.iterdir():
            if path.is_file() and path.name.lower().startswith(("license", "copying", "notice", "copyright")):
                directory.mkdir(exist_ok=True)
                shutil.copy2(path, directory / path.name)
    bundled = core / BUNDLED_LUA
    plugin_license = bundled / "LICENSE"
    lua_header = bundled / "thirdparty/lua5.4/lua.h"
    if not plugin_license.is_file() or not lua_header.is_file():
        raise FileNotFoundError("Bundled librime-lua or Lua license is missing")
    lua_notice = lua_header.read_text()
    if ("Copyright (C) 1994-2026 Lua.org, PUC-Rio." not in lua_notice or
            "Permission is hereby granted, free of charge" not in lua_notice):
        raise ValueError("Bundled Lua MIT notice is missing from lua.h")
    plugin_destination = destination / "librime-lua"
    plugin_destination.mkdir()
    shutil.copy2(plugin_license, plugin_destination / "LICENSE")
    lua_destination = destination / "lua"
    lua_destination.mkdir()
    # The vendored Lua distribution carries its MIT notice at the end of lua.h.
    shutil.copy2(lua_header, lua_destination / "Lua-license.h")
    shutil.copy2(core / "LICENSE", destination / "librime-LICENSE")
    ndk_destination = destination / "android-ndk"
    ndk_destination.mkdir(exist_ok=True)
    for name in ("NOTICE", "NOTICE.toolchain"):
        source = ndk / name
        if not source.is_file():
            raise FileNotFoundError(f"Android NDK license is missing: {source}")
        shutil.copy2(source, ndk_destination / name)


def build(args):
    lock = json.loads(LOCK.read_text())
    if args.abi not in lock["abis"]:
        raise ValueError("Unsupported ABI")
    # Release inputs stay committed. Development uses an explicit, isolated index snapshot.
    commit, source_tree = source_revision(ROOT, args.development)
    ndk = Path(os.environ["ANDROID_NDK_ROOT"]).resolve()
    if f"Pkg.Revision = {lock['ndk']}" not in (ndk / "source.properties").read_text():
        raise ValueError("ANDROID_NDK_ROOT does not match the locked NDK")
    cmake = os.environ.get("CMAKE_BIN", "cmake")
    version = subprocess.check_output([cmake, "--version"], text=True).splitlines()[0].split()[-1]
    if version != lock["cmake"]:
        raise ValueError(f"Expected CMake {lock['cmake']}, got {version}")
    work = args.work.resolve()
    cache = args.cache.resolve()
    sources = work / "sources"
    for directory in (cache, sources):
        directory.mkdir(parents=True, exist_ok=True)
    prepared = {
        name: prepare_source(name, item, sources, cache)
        for name, item in lock["sources"].items()
    }
    core = sources / "librime"
    materialization_key = core_source_key(source_tree, lock["sources"])
    if (not (core / ".android-source").is_file() or
            (core / ".android-source").read_text() != materialization_key):
        with tempfile.TemporaryDirectory(dir=sources) as temporary:
            staged = Path(temporary) / "librime"
            staged.mkdir()
            with tempfile.TemporaryFile() as output:
                run("git", "-C", ROOT, "archive", source_tree, stdout=output)
                output.seek(0)
                with tarfile.open(fileobj=output) as archive:
                    extract_sources(archive, staged, copy_links=True if args.development else None)
            bundled = staged / BUNDLED_LUA / "CMakeLists.txt"
            if not bundled.is_file():
                raise FileNotFoundError(f"Bundled librime-lua is missing: {bundled}")
            if (staged / "plugins/lua").exists():
                raise ValueError("Legacy external plugins/lua must not coexist with bundled librime-lua")
            for name, directory in EXTERNAL_PLUGINS.items():
                if os.name == "nt" or args.development:
                    shutil.copytree(prepared[name], staged / "plugins" / directory)
                else:
                    (staged / "plugins" / directory).symlink_to(prepared[name], target_is_directory=True)
            if args.development:
                sync_source_tree(staged, core)
            else:
                if core.exists():
                    shutil.rmtree(core)
                shutil.move(staged, core)
        (core / ".android-source").write_text(materialization_key)
    product = work / "product"
    product.mkdir(exist_ok=True)
    # Remove the archive produced by the previous external-Lua recipe. Bundled
    # Lua is now compiled into librime.a through rime-lua-objs.
    if (product / "lua").exists():
        shutil.rmtree(product / "lua")
    copy_licenses(prepared, core, product, ndk)
    # Boost is header-only on Android. Assemble its modular distribution into
    # the conventional include tree; no host Boost or Android Boost binary.
    boost = work / "boost/include"
    prepare_boost_headers(prepared["boost"], boost, lock["sources"]["boost"]["sha256"])
    android_cxx_flags = (
        f"-ffile-prefix-map={work.as_posix()}=. "
        "-DBOOST_ALL_NO_EMBEDDED_GDB_SCRIPTS"
    )
    common = [
        f"-DCMAKE_TOOLCHAIN_FILE={ndk.as_posix()}/build/cmake/android.toolchain.cmake",
        f"-DANDROID_ABI={args.abi}", f"-DANDROID_PLATFORM={lock['platform']}",
        "-DANDROID_STL=c++_shared", "-DBUILD_SHARED_LIBS=OFF",
        f"-DCMAKE_C_FLAGS=-ffile-prefix-map={work.as_posix()}=.",
        f"-DCMAKE_CXX_FLAGS={android_cxx_flags}",
    ]
    flags = {
        "glog": ["-DWITH_GFLAGS=OFF", "-DWITH_UNWIND=OFF", "-DBUILD_TESTING=OFF"],
        "yaml-cpp": ["-DYAML_CPP_BUILD_CONTRIB=OFF", "-DYAML_CPP_BUILD_TESTS=OFF", "-DYAML_CPP_BUILD_TOOLS=OFF"],
        "leveldb": ["-DLEVELDB_BUILD_TESTS=OFF", "-DLEVELDB_BUILD_BENCHMARKS=OFF", "-DLEVELDB_INSTALL=ON"],
        "marisa": [],
        "opencc": ["-DSHARE_INSTALL_PREFIX=share", "-DINCLUDE_INSTALL_DIR=include", "-DSYSCONF_INSTALL_DIR=etc", "-DLIB_INSTALL_DIR=lib", "-DBUILD_DOCUMENTATION=OFF", "-DBUILD_PYTHON=OFF", "-DBUILD_OPENCC_JIEBA_PLUGIN=OFF", "-DENABLE_GTEST=OFF", "-DENABLE_BENCHMARK=OFF", "-DUSE_SYSTEM_MARISA=ON", f"-DMarisa_DIR={product.as_posix()}/marisa/{args.abi}/lib/cmake/Marisa", "-DUSE_SYSTEM_PYBIND11=OFF", "-DUSE_SYSTEM_RAPIDJSON=OFF", "-DUSE_SYSTEM_TCLAP=OFF"],
    }
    for name in ("glog", "yaml-cpp", "leveldb", "marisa", "opencc"):
        cmake_build(cmake, prepared[name], work / "cmake" / args.abi / name,
                    product / name / args.abi, args.jobs, common + flags[name])
        if name == "opencc":
            verify_opencc_version(work / "cmake" / args.abi / name, lock["sources"]["opencc"]["version"])
    # Generate OpenCC dictionaries using native host tools, never Android
    # executables or files copied out of the retired artifact repository.
    host = work / "host"
    host_flags = host_compiler_flags()
    cmake_build(cmake, prepared["marisa"], work / "cmake/host/marisa", host,
                args.jobs, ["-DBUILD_SHARED_LIBS=OFF", *host_flags])
    host_opencc = prepare_source("opencc-host", lock["sources"]["opencc"], sources, cache, patches=False)
    patch = next(p for p in lock["sources"]["opencc"]["patches"] if p["file"] == "opencc-host.patch")
    patch_path = HERE / "patches" / patch["file"]
    if not (host_opencc / ".host-patched").exists():
        apply_locked_patch(host_opencc, patch_path, patch["sha256"])
        (host_opencc / ".host-patched").touch()
    cmake_build(cmake, host_opencc, work / "cmake/host/opencc", product / "opencc",
                args.jobs, [f"-DCMAKE_PREFIX_PATH={host.as_posix()}", "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_GTEST=OFF", "-DBUILD_OPENCC_JIEBA_PLUGIN=OFF", *host_flags],
                targets=["opencc_dict", "Dictionaries"], install=False)
    verify_opencc_version(work / "cmake/host/opencc", lock["sources"]["opencc"]["version"])
    run(cmake, "--install", work / "cmake/host/opencc", "--component", "data")
    roots = ";".join([(work / "boost").as_posix(), *((product / name / args.abi).as_posix() for name in flags)])
    cmake_build(cmake, core, work / "cmake" / args.abi / "librime", product / "librime" / args.abi,
                args.jobs, common + [f"-DCMAKE_CXX_FLAGS={android_cxx_flags} -I{(product / 'opencc' / args.abi / 'include').as_posix()}", "-DBUILD_STATIC=ON", "-DBUILD_MERGED_PLUGINS=ON", "-DBUILD_TEST=OFF", "-DBUILD_TOOLS=OFF", "-DALSO_LOG_TO_STDERR=ON", "-DLUA_VERSION=lua54", f"-DCMAKE_FIND_ROOT_PATH={roots}", f"-DBoost_INCLUDE_DIR={boost.as_posix()}", "-DBoost_NO_BOOST_CMAKE=ON"], targets=["rime-static"],
                environment_overrides={"RIME_PLUGINS": " ".join(MERGED_PLUGINS)})
    verify_core_build_graph(work / "cmake" / args.abi / "librime")
    toolchain_root, executable_suffix = ndk_tools(ndk)
    toolchain = toolchain_root / "bin"
    for name, library in LIBRARIES.items():
        run(toolchain / f"llvm-strip{executable_suffix}", "--strip-unneeded", product / name / args.abi / "lib" / library)
    # A real final link catches dependency omissions and incompatible objects.
    probe = work / f"link-{args.abi}.cc"
    probe.write_text('extern "C" void* rime_get_api();\nextern "C" void* sdk_api() { return rime_get_api(); }\n')
    target = {"armeabi-v7a": "armv7a-linux-androideabi", "arm64-v8a": "aarch64-linux-android", "x86": "i686-linux-android", "x86_64": "x86_64-linux-android"}[args.abi]
    run(toolchain / f"clang++{executable_suffix}", f"--target={target}{lock['platform']}",
        f"--sysroot={(toolchain_root / 'sysroot').as_posix()}", "-shared", "-fPIC", probe,
        "-Wl,--no-undefined", "-Wl,--whole-archive", product / "librime" / args.abi / "lib/librime.a", "-Wl,--no-whole-archive",
        *(product / name / args.abi / "lib" / lib for name, lib in LIBRARIES.items() if name != "librime"),
        "-llog", "-landroid", "-o", work / f"link-{args.abi}.so")
    downstream = json.loads((ROOT / "downstream/manifest.json").read_text())
    manifest = {
        "schema_version": 2, "librime_repository": downstream["repository"],
        "librime_commit": commit, "source_tree": source_tree,
        "librime_version": rime_version(core),
        "upstream_commit": downstream["upstream_commit"], "abi": args.abi,
        "ndk": lock["ndk"], "cmake": lock["cmake"], "platform": lock["platform"],
        "sources_lock_sha256": source_lock_digest(), "sources": lock["sources"],
        "merged_plugins": {
            "librime-lua": {
                "source": "librime-tree",
                "path": BUNDLED_LUA.as_posix(),
                "lua_version": lua_version(core),
            },
            "octagram": {"source_lock": "librime-octagram"},
            "predict": {"source_lock": "librime-predict"},
        },
        "archive_sha256": sha256(product / "librime" / args.abi / "lib/librime.a"),
        "rime_api_sha256": sha256(product / "librime" / args.abi / "include/rime_api.h"),
        "dependency_archives_sha256": {name: sha256(product / name / args.abi / "lib" / lib) for name, lib in LIBRARIES.items() if name != "librime"},
        "link_smoke_test": "passed", "core_patch_mode": "committed-source",
    }
    if args.development:
        manifest.update(development=True, core_patch_mode="working-tree-snapshot")
    output = product / "librime" / args.abi / "share/rime/source-manifest.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {args.abi} from {commit}, tree {source_tree}: {product}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi", required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/rime-android-sources")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--development", action="store_true", help="Snapshot local edits without staging or committing; not a release input")
    build(parser.parse_args())
