#!/usr/bin/env python3
"""Link the pinned Android archives into a self-contained, importable engine DSO."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from artifact_files import artifact_files, replace_tree, write_checksums
from build import LIBRARIES, LOCK, sha256
from local import ndk_tools


TARGETS = {
    "armeabi-v7a": "armv7a-linux-androideabi",
    "arm64-v8a": "aarch64-linux-android",
    "x86": "i686-linux-android",
    "x86_64": "x86_64-linux-android",
}
MAX_ENGINE_BYTES = 96 * 1024 * 1024


def run(*args, capture=False):
    command = [str(value) for value in args]
    if capture:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    subprocess.run(command, check=True)


def validate_elf_contract(headers, segments, abi, size):
    if size <= 0 or size > MAX_ENGINE_BYTES:
        raise ValueError(f"Engine size is outside the 96 MiB limit: {size}")
    if not re.search(r"^\s*Type:\s+DYN(?:\s|$)", headers, re.MULTILINE):
        raise ValueError("Engine ELF type is not ET_DYN")
    expected_machine = {
        "arm64-v8a": "AArch64", "armeabi-v7a": "ARM",
        "x86": "Intel 80386", "x86_64": "Advanced Micro Devices X86-64",
    }[abi]
    if not re.search(rf"^\s*Machine:\s+{re.escape(expected_machine)}\s*$", headers, re.MULTILINE):
        raise ValueError(f"Wrong ELF machine for {abi}")
    program_headers = []
    for line in segments.splitlines():
        fields = line.split()
        if not fields or fields[0] not in ("LOAD", "GNU_STACK"):
            continue
        if len(fields) < 8:
            raise ValueError(f"Malformed program header: {line}")
        try:
            alignment = int(fields[-1], 16)
        except ValueError as error:
            raise ValueError(f"Malformed program-header alignment: {line}") from error
        program_headers.append((fields[0], set("".join(fields[6:-1])), alignment))
    loads = [header for header in program_headers if header[0] == "LOAD"]
    if not loads:
        raise ValueError("Engine has no LOAD segments")
    if any("W" in flags and "E" in flags for _, flags, _ in loads):
        raise ValueError("Engine contains a writable and executable LOAD segment")
    load_alignments = [alignment for _, _, alignment in loads]
    if min(load_alignments) < 16384:
        raise ValueError(f"Engine is not 16 KiB page aligned: {load_alignments}")
    stacks = [header for header in program_headers if header[0] == "GNU_STACK"]
    if len(stacks) != 1 or "E" in stacks[0][1]:
        raise ValueError("Engine has a missing or executable GNU_STACK")
    return load_alignments


def validate_dynamic_contract(dynamic):
    needed = re.findall(r"Shared library: \[([^]]+)]", dynamic)
    allowed = {"libandroid.so", "libc.so", "libdl.so", "liblog.so", "libm.so", "libz.so"}
    if not set(needed) <= allowed or any(key in dynamic for key in ("RPATH", "RUNPATH", "TEXTREL")):
        raise ValueError(f"Unexpected dynamic contract: {needed}")
    sonames = re.findall(r"Library soname: \[([^]]+)]", dynamic)
    if sonames != ["librime-evo.so"]:
        raise ValueError(f"Unexpected SONAME: {sonames}")
    return needed, sonames[0]


def package(product, output, commit, abi, allow_development=False):
    product, output = product.resolve(), output.resolve()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Commit must be a full lowercase Git object ID")
    lock = json.loads(LOCK.read_text())
    if abi not in lock["abis"]:
        raise ValueError(f"Unsupported ABI: {abi}")
    ndk = Path(os.environ["ANDROID_NDK_ROOT"]).resolve()
    if f"Pkg.Revision = {lock['ndk']}" not in (ndk / "source.properties").read_text():
        raise ValueError("ANDROID_NDK_ROOT does not match sources.lock.json")
    toolchain_root, suffix = ndk_tools(ndk)
    tools = toolchain_root / "bin"
    output.mkdir(parents=True, exist_ok=True)
    short = commit[:8]
    library = output / f"librime-evo-{short}-{abi}.so"
    version_script = Path(__file__).with_name("engine-exports.map").resolve()
    archives = {
        name: product / name / abi / "lib" / filename
        for name, filename in LIBRARIES.items()
    }
    for name, archive in archives.items():
        if not archive.is_file():
            raise FileNotFoundError(f"Missing {name} archive: {archive}")
    source_manifest_path = product / "librime" / abi / "share/rime/source-manifest.json"
    source_manifest = json.loads(source_manifest_path.read_text())
    if source_manifest["librime_commit"] != commit or source_manifest["abi"] != abi:
        raise ValueError("Core commit or ABI does not match the build manifest")
    if source_manifest.get("link_smoke_test") != "passed":
        raise ValueError("The source build did not pass its final link smoke test")
    if source_manifest.get("development") and not allow_development:
        raise ValueError("Development snapshots require --allow-development")
    expected_archives = {"librime": source_manifest["archive_sha256"],
                         **source_manifest["dependency_archives_sha256"]}
    actual_archives = {name: sha256(path) for name, path in archives.items()}
    if actual_archives != expected_archives:
        raise ValueError("Input archives no longer match the verified source build")

    clang = tools / f"clang++{suffix}"
    strip = tools / f"llvm-strip{suffix}"
    run(
        clang,
        f"--target={TARGETS[abi]}{lock['platform']}",
        f"--sysroot={toolchain_root / 'sysroot'}",
        "-shared", "-fPIC", "-Wl,--no-undefined",
        "-Wl,--whole-archive", archives["librime"], "-Wl,--no-whole-archive",
        *(archives[name] for name in LIBRARIES if name != "librime"),
        "-static-libstdc++", f"-Wl,--version-script={version_script}",
        "-Wl,-soname,librime-evo.so", "-Wl,-z,max-page-size=16384",
        "-Wl,-z,relro,-z,now,-z,noexecstack", "-Wl,--build-id=sha1",
        "-llog", "-landroid", "-o", library,
    )
    run(strip, "--strip-unneeded", library)

    dynamic = run(tools / f"llvm-readelf{suffix}", "-dW", library, capture=True)
    needed, soname = validate_dynamic_contract(dynamic)
    headers = run(tools / f"llvm-readelf{suffix}", "-hW", library, capture=True)
    segments = run(tools / f"llvm-readelf{suffix}", "-lW", library, capture=True)
    load_alignments = validate_elf_contract(headers, segments, abi, library.stat().st_size)
    symbols = run(tools / f"llvm-nm{suffix}", "-D", "--defined-only", library, capture=True)
    exports = [line.split()[-1].split("@", 1)[0] for line in symbols.splitlines() if line.strip()]
    if exports != ["rime_get_api"]:
        raise ValueError(f"Unexpected exported symbols: {exports}")

    record = {
        "schema_version": 1,
        "repository": "https://github.com/SandyYuR/librime-evo",
        "librime_commit": commit,
        "librime_version": source_manifest["librime_version"],
        "source_tree": source_manifest["source_tree"],
        "abi": abi,
        "android_api": lock["platform"],
        "ndk": lock["ndk"],
        "cmake": lock["cmake"],
        "soname": soname,
        "entry_symbol": "rime_get_api",
        "needed_libraries": needed,
        "minimum_load_alignment": min(load_alignments),
        "sha256": sha256(library),
        "bytes": library.stat().st_size,
        "source_manifest": source_manifest,
        "input_archives_sha256": actual_archives,
    }
    manifest = output / "engine-manifest.json"
    manifest.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n")
    licenses = replace_tree(product / "licenses", output / "licenses")
    write_checksums(output, artifact_files(library, manifest, licenses))
    print(json.dumps({"engine": str(library), "sha256": record["sha256"], "bytes": record["bytes"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("product", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--allow-development", action="store_true")
    args = parser.parse_args()
    package(args.product, args.output, args.commit, args.abi, args.allow_development)
