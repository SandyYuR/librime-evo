"""Regressions for isolated dependency builds, patches and header caches."""
from pathlib import Path
import io
import json
import shutil
import subprocess
import tarfile
import tempfile
import unittest

from build import (CORE_SUBMODULES, EXTERNAL_PLUGINS, LOCK, MERGED_PLUGINS,
                   apply_patch, cmake_build, copy_licenses, core_source_key,
                   prepare_boost_headers, prepare_source, rime_version, sha256,
                   verify_core_build_graph, verify_core_submodule_pins)


ROOT = Path(__file__).resolve().parents[2]


class AndroidPluginSourcesTest(unittest.TestCase):
    def test_lua_plugin_and_spdlog_match_the_recorded_submodules(self):
        sources = json.loads(LOCK.read_text())["sources"]
        tree = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD^{tree}"], text=True
        ).strip()

        verify_core_submodule_pins(tree, sources)
        self.assertNotIn("glog", sources)
        self.assertEqual(("lua", "octagram", "predict"), MERGED_PLUGINS)

        sources["librime-evo-lua"]["commit"] = "f" * 40
        with self.assertRaisesRegex(ValueError, "plugins/lua"):
            verify_core_submodule_pins(tree, sources)

    def test_core_materialization_changes_with_luajit_and_external_plugin_inputs(self):
        sources = {name: {"commit": "a" * 40}
                   for name in (*CORE_SUBMODULES, "luajit", *EXTERNAL_PLUGINS)}
        original = core_source_key("1" * 40, sources)
        self.assertNotEqual(original, core_source_key("2" * 40, sources))
        for name in ("luajit", "librime-predict"):
            changed = {**sources, name: {"commit": "b" * 40}}
            self.assertNotEqual(original, core_source_key("1" * 40, changed))


class EngineLicenseTest(unittest.TestCase):
    def test_product_keeps_distinct_lua_plugin_runtime_and_logging_notices(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            core = root / "core"
            core.mkdir()
            (core / "LICENSE").write_bytes(b"librime license")
            ndk = root / "ndk"
            ndk.mkdir()
            (ndk / "NOTICE").write_bytes(b"NDK notice")
            (ndk / "NOTICE.toolchain").write_bytes(b"toolchain notice")
            notices = {
                "librime-evo-lua": ("LICENSE", b"plugin license"),
                "luajit": ("COPYRIGHT", b"runtime license"),
                "spdlog": ("LICENSE", b"logging license"),
            }
            sources = {}
            for name, (file, content) in notices.items():
                directory = root / name
                directory.mkdir()
                (directory / file).write_bytes(content)
                sources[name] = directory
            fmt_license = sources["spdlog"] / "include/spdlog/fmt/bundled/fmt.license.rst"
            fmt_license.parent.mkdir(parents=True)
            fmt_license.write_bytes(b"fmt license")
            product = root / "product"
            (product / "licenses/stale").mkdir(parents=True)

            copy_licenses(sources, core, product, ndk)

            for name, (file, content) in notices.items():
                self.assertEqual(content, (product / "licenses" / name / file).read_bytes())
            self.assertEqual(b"fmt license",
                             (product / "licenses/spdlog/fmt.license.rst").read_bytes())
            self.assertFalse((product / "licenses/stale").exists())


class CoreBuildGraphTest(unittest.TestCase):
    def test_luajit_and_expected_external_plugins_are_merged(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            graph = "\n".join((
                "plugins/lua/src/types.cc",
                "plugins/lua/luajit/Release/objects/lj_vm.o",
                "deps/spdlog/libspdlog.a",
                "plugins/octagram/src/octagram.cc",
                "plugins/predict/src/predict.cc",
            ))
            (build / "build.ninja").write_text(graph)

            verify_core_build_graph(build)

            (build / "build.ninja").write_text(graph.replace(
                "plugins/lua/luajit/Release/objects/lj_vm.o\n", ""
            ))
            with self.assertRaisesRegex(ValueError, "lj_vm.o"):
                verify_core_build_graph(build)

            (build / "build.ninja").write_text(graph + "\nplugins/librime-lua/src/types.cc\n")
            with self.assertRaisesRegex(ValueError, "legacy_lua=True"):
                verify_core_build_graph(build)


class PreparedSourceRecoveryTest(unittest.TestCase):
    def test_retry_replaces_an_incomplete_source_tree_without_nesting_the_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = root / "sources"
            cache = root / "cache"
            cache.mkdir()
            archive = root / "source.tar.gz"
            content = b"verified source\n"
            with tarfile.open(archive, "w:gz") as output:
                entry = tarfile.TarInfo("dependency-release/example.txt")
                entry.size = len(content)
                output.addfile(entry, io.BytesIO(content))
            digest = sha256(archive)
            archive.rename(cache / digest)
            target = sources / "dependency"
            target.mkdir(parents=True)
            (target / "example.txt").write_text("interrupted patch\n")
            (target / "obsolete.txt").touch()
            item = {"url": "https://example.invalid/source.tar.gz", "sha256": digest, "patches": []}

            restored = prepare_source("dependency", item, sources, cache)

            self.assertEqual(content, (restored / "example.txt").read_bytes())
            self.assertFalse((restored / "obsolete.txt").exists())
            self.assertFalse((restored / "dependency-release").exists())
            self.assertTrue((restored / ".android-source").is_file())


class SourcePatchTest(unittest.TestCase):
    def test_patch_changes_extracted_source_inside_another_git_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            subprocess.run(["git", "init", "-q", str(outer)], check=True)
            source = outer / "build/sources/dependency"
            source.mkdir(parents=True)
            (source / "example.txt").write_text("before\n", newline="\n")
            patch = outer / "dependency.patch"
            patch.write_text("diff --git a/example.txt b/example.txt\n"
                             "--- a/example.txt\n+++ b/example.txt\n"
                             "@@ -1 +1 @@\n-before\n+after\n", newline="\n")
            apply_patch(source, patch)
            self.assertEqual("after\n", (source / "example.txt").read_text())
            self.assertFalse((outer / "example.txt").exists())


class BoostHeadersTest(unittest.TestCase):
    def test_source_upgrade_replaces_cached_headers_and_removes_obsolete_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            headers = source / "libs/config/include/boost"
            headers.mkdir(parents=True)
            (headers / "version.hpp").write_text("old version")
            (headers / "obsolete.hpp").touch()
            include = root / "work/boost/include"
            prepare_boost_headers(source, include, "old source digest")
            (headers / "version.hpp").write_text("new version")
            (headers / "obsolete.hpp").unlink()
            prepare_boost_headers(source, include, "new source digest")
            self.assertEqual("new version", (include / "boost/version.hpp").read_text())
            self.assertFalse((include / "boost/obsolete.hpp").exists())


class RimeVersionTest(unittest.TestCase):
    def test_version_comes_from_the_committed_cmake_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / "CMakeLists.txt").write_text("set(rime_version 1.17.0)\n")
            self.assertEqual("1.17.0", rime_version(source))
            (source / "CMakeLists.txt").write_text("set(rime_version ${UNTRUSTED})\n")
            with self.assertRaises(ValueError):
                rime_version(source)


class SourceBuildIsolationTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "CMake and Ninja required")
    def test_dependency_cannot_read_enclosing_repository_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            subprocess.run(["git", "init", "-q", str(outer)], check=True)
            subprocess.run(["git", "-C", str(outer), "-c", "user.name=Test", "-c",
                            "user.email=test@example.com", "commit", "-q", "--allow-empty", "-m", "fixture"], check=True)
            source = outer / "build/sources/dependency"
            source.mkdir(parents=True)
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.15)\nproject(Isolation LANGUAGES NONE)\n'
                'find_package(Git REQUIRED)\n'
                'execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel\n'
                '  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" RESULT_VARIABLE result ERROR_QUIET)\n'
                'if(result EQUAL 0)\nmessage(FATAL_ERROR "Dependency read enclosing repository")\nendif()\n')
            cmake_build(shutil.which("cmake"), source, outer / "output", outer / "install", 1, install=False)


if __name__ == "__main__":
    unittest.main()
