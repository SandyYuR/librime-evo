"""Local development must preserve the real index, source integrity and incremental builds."""
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

from local import extract_sources, source_revision, sync_source_tree
from build import apply_locked_patch
import hashlib


class DevelopmentSnapshotTest(unittest.TestCase):
    def test_snapshot_includes_local_edits_without_staging_or_committing_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)

            def git(*args):
                return subprocess.check_output(["git", "-C", str(root), *args])

            git("init", "-q")
            git("config", "user.name", "Source snapshot fixture")
            git("config", "user.email", "snapshot@example.invalid")
            (root / "src").mkdir()
            source = root / "src/main.cc"
            source.write_bytes(b"original\n")
            (root / "src/removed.cc").write_bytes(b"removed\n")
            git("add", "--", "src/main.cc", "src/removed.cc")
            git("commit", "-q", "-m", "fixture")
            original_head = git("rev-parse", "HEAD")
            source.write_bytes(b"staged\n")
            git("add", "--", "src/main.cc")
            original_index = git("diff", "--cached", "--binary", "HEAD")
            source.write_bytes(b"working\n")
            (root / "src/new.cc").write_bytes(b"new source\n")
            (root / "src/removed.cc").unlink()

            commit, tree = source_revision(root, development=True)

            self.assertEqual(original_head.decode().strip(), commit)
            self.assertEqual(b"working\n", git("show", f"{tree}:src/main.cc"))
            self.assertEqual(b"new source\n", git("show", f"{tree}:src/new.cc"))
            self.assertNotIn(b"src/removed.cc", git("ls-tree", "-r", "--name-only", tree))
            self.assertEqual(original_head, git("rev-parse", "HEAD"))
            self.assertEqual(original_index, git("diff", "--cached", "--binary", "HEAD"))
            with self.assertRaises(subprocess.CalledProcessError):
                source_revision(root)


class PortableExtractionTest(unittest.TestCase):
    def archive(self, linkname="original.txt", linktype=tarfile.SYMTYPE):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w") as output:
            member = tarfile.TarInfo("original.txt")
            member.size = 5
            output.addfile(member, io.BytesIO(b"hello"))
            link = tarfile.TarInfo("alias.txt")
            link.type = linktype
            link.linkname = linkname
            output.addfile(link)
        data.seek(0)
        return tarfile.open(fileobj=data)

    def test_internal_links_materialize_without_os_symlink_privileges(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary, self.archive(linktype=kind) as archive:
                extract_sources(archive, temporary, copy_links=True)
                alias = Path(temporary) / "alias.txt"
                self.assertEqual(b"hello", alias.read_bytes())
                self.assertFalse(alias.is_symlink())

    def test_links_cannot_escape_or_point_to_themselves(self):
        for target in ("../../outside.txt", "alias.txt"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary, self.archive(target) as archive:
                with self.assertRaises((ValueError, tarfile.FilterError)):
                    extract_sources(archive, temporary, copy_links=True)


class CanonicalPatchTest(unittest.TestCase):
    def test_windows_line_endings_use_the_locked_git_bytes_and_other_changes_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            (source / "example.txt").write_bytes(b"before\n")
            content = b"diff --git a/example.txt b/example.txt\n--- a/example.txt\n+++ b/example.txt\n@@ -1 +1 @@\n-before\n+after\n"
            patch = root / "change.patch"
            patch.write_bytes(content.replace(b"\n", b"\r\n"))
            digest = hashlib.sha256(content).hexdigest()
            apply_locked_patch(source, patch, digest)
            self.assertEqual(b"after\n", (source / "example.txt").read_bytes())
            patch.write_bytes(content.replace(b"after", b"tampered"))
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                apply_locked_patch(source, patch, digest)


class IncrementalSourceTest(unittest.TestCase):
    def test_unchanged_cpp_keeps_its_timestamp_while_edits_and_deletions_sync(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, destination = Path(temporary) / "incoming", Path(temporary) / "compiled"
            source.mkdir()
            destination.mkdir()
            for directory in (source, destination):
                (directory / "same.cc").write_bytes(b"same")
            (destination / "removed.cc").touch()
            (destination / "changed.cc").write_bytes(b"old")
            (source / "changed.cc").write_bytes(b"new")
            os.utime(destination / "same.cc", (1000, 1000))
            sync_source_tree(source, destination)
            self.assertEqual(1000, (destination / "same.cc").stat().st_mtime)
            self.assertEqual(b"new", (destination / "changed.cc").read_bytes())
            self.assertFalse((destination / "removed.cc").exists())

    def test_file_directory_type_changes_are_handled(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, destination = Path(temporary) / "incoming", Path(temporary) / "compiled"
            source.mkdir()
            destination.mkdir()
            (source / "header").mkdir()
            (source / "header/new.h").touch()
            (destination / "header").touch()
            sync_source_tree(source, destination)
            self.assertTrue((destination / "header/new.h").is_file())


if __name__ == "__main__":
    unittest.main()
