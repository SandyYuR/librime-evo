"""Regressions for isolated dependency builds, patches and header caches."""
from pathlib import Path
import io
import json
import shutil
import subprocess
import tarfile
import tempfile
import unittest

from build import (BUNDLED_LUA, EXTERNAL_PLUGINS, LOCK, MERGED_PLUGINS,
                   apply_patch, cmake_build, copy_licenses, core_source_key,
                   lua_version, prepare_boost_headers, prepare_source,
                   rime_version, sha256, verify_core_build_graph)


ROOT = Path(__file__).resolve().parents[2]


class AndroidPluginSourcesTest(unittest.TestCase):
    def test_evo_lua_is_bundled_and_only_missing_plugins_are_downloaded(self):
        locked = json.loads(LOCK.read_text())["sources"]

        self.assertNotIn("lua", locked)
        self.assertNotIn("librime-lua", locked)
        self.assertEqual({
            "librime-octagram": "octagram",
            "librime-predict": "predict",
        }, EXTERNAL_PLUGINS)
        self.assertEqual(("librime-lua", "octagram", "predict"), MERGED_PLUGINS)
        self.assertTrue((ROOT / BUNDLED_LUA / "CMakeLists.txt").is_file())
        self.assertEqual("5.4.9", lua_version(ROOT))

    def test_bundled_lua_guards_large_file_io_without_disabling_posix_popen(self):
        source = (ROOT / BUNDLED_LUA / "thirdparty/lua5.4/liolib.c").read_text(encoding="utf-8")
        popen = source.split("#if !defined(l_popen)", 1)[1].split(
            "#elif defined(LUA_USE_WINDOWS)", 1
        )[0]
        seek = source.split("#if !defined(l_fseek)", 1)[1].split(
            "#elif defined(LUA_USE_WINDOWS)", 1
        )[0]

        # popen/pclose are already available on 32-bit Android API 23. Only
        # fseeko/ftello with 64-bit offsets need the API 24 compatibility guard.
        self.assertRegex(popen, r"#if defined\(LUA_USE_POSIX\)\s*/\* \{ \*/")
        self.assertIn("popen(c,m)", popen)
        self.assertIn("pclose(file)", popen)
        self.assertNotIn("__ANDROID_API__", popen)
        guard = "!defined(__ANDROID__) || defined(__LP64__) || __ANDROID_API__ >= 24"
        self.assertIn(guard, seek)
        self.assertIn("fseeko(f,o,w)", seek)
        self.assertIn("ftello(f)", seek)
        self.assertEqual(1, source.count(guard))

    def test_core_materialization_changes_with_external_plugin_inputs(self):
        sources = {
            "librime-octagram": {"commit": "a" * 40},
            "librime-predict": {"commit": "b" * 40},
        }
        original = core_source_key("1" * 40, sources)
        sources["librime-predict"]["commit"] = "c" * 40

        self.assertNotEqual(original, core_source_key("1" * 40, sources))
        self.assertNotEqual(original, core_source_key("2" * 40, {
            "librime-octagram": {"commit": "a" * 40},
            "librime-predict": {"commit": "b" * 40},
        }))


class BundledLuaLicenseTest(unittest.TestCase):
    def test_product_keeps_plugin_and_lua_notices_without_external_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            core = root / "core"
            bundled = core / BUNDLED_LUA
            (bundled / "thirdparty/lua5.4").mkdir(parents=True)
            (bundled / "LICENSE").write_bytes(b"plugin license\n")
            lua_notice = (b"Copyright (C) 1994-2026 Lua.org, PUC-Rio.\n"
                          b"Permission is hereby granted, free of charge\n")
            (bundled / "thirdparty/lua5.4/lua.h").write_bytes(lua_notice)
            (core / "LICENSE").write_bytes(b"librime license\n")
            ndk = root / "ndk"
            ndk.mkdir()
            (ndk / "NOTICE").write_bytes(b"NDK notice\n")
            (ndk / "NOTICE.toolchain").write_bytes(b"toolchain notice\n")
            product = root / "product"
            (product / "licenses/stale").mkdir(parents=True)
            (product / "licenses/stale/removed.txt").touch()

            copy_licenses({}, core, product, ndk)

            self.assertEqual(b"plugin license\n",
                             (product / "licenses/librime-lua/LICENSE").read_bytes())
            self.assertEqual(lua_notice,
                             (product / "licenses/lua/Lua-license.h").read_bytes())
            self.assertFalse((product / "licenses/stale").exists())


class CoreBuildGraphTest(unittest.TestCase):
    def test_only_bundled_lua_and_expected_external_plugins_are_merged(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            graph = "\n".join((
                "plugins/librime-lua/src/types.cc",
                "plugins/librime-lua/thirdparty/lua5.4/lapi.c",
                "plugins/octagram/src/octagram.cc",
                "plugins/predict/src/predict.cc",
            ))
            (build / "build.ninja").write_text(graph)

            verify_core_build_graph(build)

            (build / "build.ninja").write_text(graph.replace(
                "plugins/librime-lua/src/types.cc\n", ""
            ))
            with self.assertRaisesRegex(ValueError, "plugins/librime-lua/src/"):
                verify_core_build_graph(build)

            (build / "build.ninja").write_text(graph + "\nplugins/lua/src/types.cc\n")
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
