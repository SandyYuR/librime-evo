#!/usr/bin/env python3
"""Build self-contained artifact directories with verifiable relative checksums."""
import hashlib
from pathlib import Path
import shutil


CHECKSUM_FILE = "SHA256SUMS"


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def separate_directories(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if (source == destination or source.is_relative_to(destination) or
            destination.is_relative_to(source)):
        raise ValueError("Artifact source and destination directories must not overlap")
    return source, destination


def replace_tree(source, destination):
    source, destination = separate_directories(source, destination)
    if not source.is_dir():
        raise FileNotFoundError(f"Artifact metadata directory is missing: {source}")
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination)
    return destination


def artifact_files(*paths):
    for value in paths:
        path = Path(value)
        if path.is_file():
            yield path
        elif path.is_dir():
            yield from (child for child in path.rglob("*") if child.is_file())
        else:
            raise FileNotFoundError(f"Artifact input is missing: {path}")


def write_checksums(root, files):
    root = Path(root).resolve()
    entries = {}
    for path in files:
        path = Path(path).resolve()
        if not path.is_file() or not path.is_relative_to(root):
            raise ValueError(f"Checksum input is outside the artifact: {path}")
        relative = path.relative_to(root).as_posix()
        if relative == CHECKSUM_FILE:
            raise ValueError("The checksum file cannot checksum itself")
        entries[relative] = sha256(path)
    if not entries:
        raise ValueError("Artifact checksum set is empty")
    output = root / CHECKSUM_FILE
    output.write_text("".join(f"{entries[name]}  {name}\n" for name in sorted(entries)),
                      encoding="utf-8", newline="\n")
    return output
