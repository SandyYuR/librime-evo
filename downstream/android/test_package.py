"""Packaging metadata must keep the provider and engine contracts stable."""
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from artifact_files import artifact_files, replace_tree, sha256, write_checksums
from package_engine import MAX_ENGINE_BYTES, validate_dynamic_contract, validate_elf_contract
from package_plugin_apk import DISCOVERY_ACTION, PACKAGE_NAME, package, render_manifest


class PluginManifestTest(unittest.TestCase):
    def test_provider_is_discoverable_without_shipping_executable_dex(self):
        commit = "a" * 40
        manifest = render_manifest({"librime_commit": commit, "librime_version": "1.18.2"}, "test")
        self.assertIn(f'package="{PACKAGE_NAME}"', manifest)
        self.assertIn(f'<action android:name="{DISCOVERY_ACTION}"', manifest)
        self.assertIn('android:hasCode="false"', manifest)
        self.assertIn('android:extractNativeLibs="true"', manifest)
        self.assertIn('org.rime.plugin.RIME_API_ABI" android:value="1"', manifest)
        self.assertIn('org.rime.plugin.SONAME" android:value="librime-evo.so"', manifest)
        self.assertIn('1.18.2-evo-aaaaaaaa', manifest)

    def test_untrusted_xml_fields_are_rejected(self):
        for commit, version, channel in (("short", "1.17.0", "test"),
                                         ("a" * 40, "${bad}", "test"),
                                         ("a" * 40, "1.17.0", "nightly")):
            with self.subTest(commit=commit, version=version, channel=channel), self.assertRaises(ValueError):
                render_manifest({"librime_commit": commit, "librime_version": version}, channel)


class PluginPackagingInputsTest(unittest.TestCase):
    def test_overlapping_output_is_rejected_before_any_input_or_tool_access(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "so"
            source.mkdir()
            engine = source / "engine.so"
            manifest = source / "engine-manifest.json"
            license_file = source / "licenses" / "LICENSE"
            license_file.parent.mkdir()
            engine.write_bytes(b"engine")
            manifest.write_bytes(b"manifest")
            license_file.write_bytes(b"license")
            original = {path.relative_to(root): path.read_bytes()
                        for path in root.rglob("*") if path.is_file()}
            for output in (source, root, source / "nested"):
                with self.subTest(output=output), mock.patch.dict(os.environ, {}, clear=True), \
                        self.assertRaisesRegex(ValueError, "must not overlap"):
                    package(engine, manifest, output, "test")
                self.assertEqual(original, {path.relative_to(root): path.read_bytes()
                                           for path in root.rglob("*") if path.is_file()})
            self.assertFalse((source / "nested").exists())

    def test_engine_in_output_is_rejected_before_packaging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "apk"
            output.mkdir()
            engine = output / "engine.so"
            engine.write_bytes(b"engine")
            with self.assertRaisesRegex(ValueError, "Engine input must be outside"):
                package(engine, root / "so" / "engine-manifest.json", output, "test")
            self.assertEqual(b"engine", engine.read_bytes())

    def test_resources_are_utf8_even_with_a_legacy_default_encoding(self):
        class ResourcesChecked(Exception):
            pass

        original_write_text = Path.write_text

        def legacy_write_text(path, text, encoding=None, **kwargs):
            return original_write_text(path, text, encoding=encoding or "cp936", **kwargs)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "so"
            source.mkdir()
            engine = source / "engine.so"
            engine.write_bytes(b"engine")
            manifest = source / "engine-manifest.json"
            manifest.write_text(json.dumps({
                "abi": "arm64-v8a", "sha256": sha256(engine), "soname": "librime-evo.so",
                "entry_symbol": "rime_get_api", "librime_commit": "a" * 40,
                "librime_version": "1.17.0",
            }), encoding="utf-8")
            sdk = root / "sdk"
            tools = sdk / "build-tools" / "36.1.0"
            tools.mkdir(parents=True)
            for name in (("aapt2.exe", "zipalign.exe", "apksigner.bat") if os.name == "nt"
                         else ("aapt2", "zipalign", "apksigner")):
                (tools / name).touch()
            android_jar = sdk / "platforms" / "android-36" / "android.jar"
            android_jar.parent.mkdir(parents=True)
            android_jar.touch()

            def check_resources(*args, **kwargs):
                self.assertEqual("compile", args[1])
                resource_dir = Path(args[3])
                resources = ET.fromstring((resource_dir / "values" / "strings.xml").read_bytes())
                self.assertEqual("librime-evo 引擎", resources.find("string").text)
                app_manifest = ET.fromstring((resource_dir.parent / "AndroidManifest.xml").read_bytes())
                self.assertEqual(PACKAGE_NAME, app_manifest.attrib["package"])
                raise ResourcesChecked

            environment = {
                "ANDROID_SDK_ROOT": str(sdk), "BUILD_TOOLS_VERSION": "36.1.0",
                "KEYSTORE_FILE": str(root / "unused.jks"), "KEYSTORE_PASSWORD": "unused",
                "KEY_ALIAS": "unused", "KEY_PASSWORD": "unused",
            }
            with mock.patch.dict(os.environ, environment), \
                    mock.patch.object(Path, "write_text", legacy_write_text), \
                    mock.patch("package_plugin_apk.run", side_effect=check_resources), \
                    self.assertRaises(ResourcesChecked):
                package(engine, manifest, root / "apk", "test")


class EngineElfContractTest(unittest.TestCase):
    headers = """ELF Header:
  Type:                              DYN (Shared object file)
  Machine:                           AArch64
"""
    segments = """Program Headers:
  LOAD 0x000000 0x0 0x0 0x1000 0x1000 R   0x4000
  LOAD 0x004000 0x4000 0x4000 0x2000 0x2000 R E 0x4000
  LOAD 0x008000 0x8000 0x8000 0x1000 0x1800 RW  0x4000
  GNU_STACK 0x000000 0x0 0x0 0x0 0x0 RW 0x0
"""
    dynamic = """Dynamic section:
  0x0000000000000001 (NEEDED) Shared library: [libc.so]
  0x000000000000000e (SONAME) Library soname: [librime-evo.so]
"""

    def test_safe_shared_object_is_accepted(self):
        self.assertEqual([0x4000, 0x4000, 0x4000],
                         validate_elf_contract(self.headers, self.segments, "arm64-v8a", 4096))

    def test_executable_or_unsafe_images_are_rejected(self):
        cases = (
            (self.headers.replace("DYN (Shared object file)", "EXEC (Executable file)"), self.segments, 4096),
            (self.headers, self.segments.replace("0x1800 RW  0x4000", "0x1800 RWE 0x4000"), 4096),
            (self.headers, self.segments.replace("GNU_STACK 0x000000 0x0 0x0 0x0 0x0 RW 0x0",
                                                 "GNU_STACK 0x000000 0x0 0x0 0x0 0x0 RWE 0x0"), 4096),
            (self.headers, self.segments, MAX_ENGINE_BYTES + 1),
        )
        for headers, segments, size in cases:
            with self.subTest(size=size, headers=headers), self.assertRaises(ValueError):
                validate_elf_contract(headers, segments, "arm64-v8a", size)

    def test_dynamic_contract_pins_system_dependencies_and_soname(self):
        self.assertEqual((["libc.so"], "librime-evo.so"),
                         validate_dynamic_contract(self.dynamic))
        for dynamic in (self.dynamic.replace("librime-evo.so", "librime.so"),
                        self.dynamic + "  0x1 (NEEDED) Shared library: [libevil.so]\n",
                        self.dynamic + "  0x1d (RUNPATH) Library runpath: [/tmp]\n"):
            with self.subTest(dynamic=dynamic), self.assertRaises(ValueError):
                validate_dynamic_contract(dynamic)


class ArtifactChecksumsTest(unittest.TestCase):
    def test_replace_tree_rejects_overlap_without_removing_either_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "licenses"
            child = source / "nested"
            child.mkdir(parents=True)
            (source / "LICENSE").write_bytes(b"outer license")
            (child / "LICENSE").write_bytes(b"inner license")
            original = {path.relative_to(root): path.read_bytes()
                        for path in root.rglob("*") if path.is_file()}
            for first, second in ((source, source), (source, child), (child, source)):
                with self.subTest(source=first, destination=second), \
                        self.assertRaisesRegex(ValueError, "must not overlap"):
                    replace_tree(first, second)
                self.assertEqual(original, {path.relative_to(root): path.read_bytes()
                                           for path in root.rglob("*") if path.is_file()})

    def test_checksum_manifest_is_relative_and_self_contained(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            engine = root / "engine.so"
            source_licenses = root / "source-licenses"
            source_license = source_licenses / "dependency/LICENSE"
            source_license.parent.mkdir(parents=True)
            source_license.write_bytes(b"license")
            licenses = replace_tree(source_licenses, root / "licenses")
            license_file = licenses / "dependency/LICENSE"
            engine.write_bytes(b"engine")
            sums = write_checksums(root, artifact_files(engine, licenses))
            self.assertEqual(
                f"{sha256(engine)}  engine.so\n"
                f"{sha256(license_file)}  licenses/dependency/LICENSE\n",
                sums.read_text(),
            )
            self.assertNotIn(b"\r", sums.read_bytes())
            for line in sums.read_text().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(digest, sha256(root / name))


if __name__ == "__main__":
    unittest.main()
