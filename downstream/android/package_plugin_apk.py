#!/usr/bin/env python3
"""Package a verified engine DSO as an org.rime.plugin.MANIFEST provider APK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

from artifact_files import artifact_files, replace_tree, separate_directories, write_checksums

PACKAGE_NAME = "org.rime.plugin.sandyyur.librime_evo"
DISCOVERY_ACTION = "org.rime.plugin.MANIFEST"


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(*args, capture=False):
    command = [str(value) for value in args]
    if capture:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    subprocess.run(command, check=True)


def render_manifest(metadata, channel):
    commit = metadata["librime_commit"]
    version = metadata.get("librime_version", "")
    if (not re.fullmatch(r"[0-9a-f]{40}", commit) or
            not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}(?:[-+][0-9A-Za-z.-]+)?", version) or
            channel not in ("release", "test")):
        raise ValueError("Invalid plugin metadata")
    short = commit[:8]
    provider_version = f"{version}-evo-{short}"
    return f'''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{PACKAGE_NAME}">
    <uses-sdk android:minSdkVersion="23" android:targetSdkVersion="36" />
    <application android:allowBackup="false" android:extractNativeLibs="true" android:hasCode="false"
        android:label="@string/app_name">
        <meta-data android:name="org.rime.plugin.API_VERSION" android:value="1" />
        <meta-data android:name="org.rime.plugin.RIME_API_ABI" android:value="1" />
        <meta-data android:name="org.rime.plugin.PLUGIN_ID" android:value="sandyyur-librime-evo" />
        <meta-data android:name="org.rime.plugin.SONAME" android:value="librime-evo.so" />
        <meta-data android:name="org.rime.plugin.LIBRIME_VERSION" android:value="{provider_version}" />
        <meta-data android:name="org.rime.plugin.CHANNEL" android:value="{channel}" />
        <activity android:name="android.app.Activity" android:exported="true">
            <intent-filter>
                <action android:name="{DISCOVERY_ACTION}" />
                <category android:name="android.intent.category.DEFAULT" />
            </intent-filter>
        </activity>
    </application>
</manifest>
'''


def package(engine, engine_manifest, output, channel):
    engine, engine_manifest, output = engine.resolve(), engine_manifest.resolve(), output.resolve()
    # Validate before writing the APK or replacing metadata. Resolve license paths
    # separately because a symlink may point outside the manifest's directory.
    separate_directories(engine_manifest.parent, output)
    separate_directories(engine_manifest.parent / "licenses", output / "licenses")
    if engine.is_relative_to(output):
        raise ValueError("Engine input must be outside the APK output directory")
    metadata = json.loads(engine_manifest.read_text(encoding="utf-8"))
    abi = metadata["abi"]
    if (sha256(engine) != metadata["sha256"] or abi not in ("arm64-v8a", "x86_64") or
            metadata.get("soname") != "librime-evo.so" or
            metadata.get("entry_symbol") != "rime_get_api"):
        raise ValueError("Engine does not match its verified Android manifest")
    sdk = Path(os.environ["ANDROID_SDK_ROOT"]).resolve()
    build_tools = os.environ.get("BUILD_TOOLS_VERSION", "36.1.0")
    tools = sdk / "build-tools" / build_tools
    android_jar = sdk / "platforms/android-36/android.jar"
    aapt2 = tools / ("aapt2.exe" if os.name == "nt" else "aapt2")
    zipalign = tools / ("zipalign.exe" if os.name == "nt" else "zipalign")
    apksigner = tools / ("apksigner.bat" if os.name == "nt" else "apksigner")
    for path in (aapt2, zipalign, apksigner, android_jar):
        if not path.exists():
            raise FileNotFoundError(path)
    keystore = Path(os.environ["KEYSTORE_FILE"]).resolve()
    store_password = os.environ["KEYSTORE_PASSWORD"]
    key_alias = os.environ["KEY_ALIAS"]
    key_password = os.environ["KEY_PASSWORD"]
    output.mkdir(parents=True, exist_ok=True)
    short = metadata["librime_commit"][:8]
    provider_version = f"{metadata['librime_version']}-evo-{short}"
    apk = output / f"librime-evo-engine-{short}-{abi}.apk"
    manifest_text = render_manifest(metadata, channel)
    with tempfile.TemporaryDirectory(dir=output) as temporary:
        root = Path(temporary)
        manifest = root / "AndroidManifest.xml"
        resources = root / "res/values"
        compiled_resources = root / "resources.zip"
        unsigned = root / "unsigned.apk"
        packed = root / "packed.apk"
        aligned = root / "aligned.apk"
        manifest.write_text(manifest_text, encoding="utf-8")
        resources.mkdir(parents=True)
        (resources / "strings.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<resources><string name="app_name">librime-evo 引擎</string></resources>\n',
            encoding="utf-8",
        )
        run(aapt2, "compile", "--dir", root / "res", "-o", compiled_resources)
        run(aapt2, "link", "-o", unsigned, "-I", android_jar,
            "--manifest", manifest, "--min-sdk-version", "23", "--target-sdk-version", "36",
            "--version-code", "1", "--version-name", provider_version, compiled_resources)
        shutil.copy2(unsigned, packed)
        with zipfile.ZipFile(packed, "a", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            info = zipfile.ZipInfo(f"lib/{abi}/librime-evo.so", (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, engine.read_bytes())
        run(zipalign, "-f", "-p", "4", packed, aligned)
        run(apksigner, "sign", "--ks", keystore, "--ks-pass", f"pass:{store_password}",
            "--ks-key-alias", key_alias, "--key-pass", f"pass:{key_password}",
            "--out", apk, aligned)
        verification = run(apksigner, "verify", "--verbose", "--print-certs", apk, capture=True)
    with zipfile.ZipFile(apk) as archive:
        payload = archive.read(f"lib/{abi}/librime-evo.so")
    if hashlib.sha256(payload).hexdigest() != metadata["sha256"]:
        raise ValueError("APK payload does not match the engine")
    match = re.search(r"certificate SHA-256 digest: ([0-9a-f]+)", verification, re.IGNORECASE)
    if not match:
        raise ValueError("Could not read the APK signer certificate")
    record = {
        "schema_version": 1,
        "package_name": PACKAGE_NAME,
        "version_name": provider_version,
        "channel": channel,
        "abi": abi,
        "certificate_sha256": match.group(1).lower(),
        "engine_sha256": metadata["sha256"],
        "apk_sha256": sha256(apk),
        "apk_bytes": apk.stat().st_size,
    }
    result = output / "apk-manifest.json"
    result.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    engine_record = output / "engine-manifest.json"
    if engine_manifest != engine_record:
        shutil.copy2(engine_manifest, engine_record)
    licenses = replace_tree(engine_manifest.parent / "licenses", output / "licenses")
    write_checksums(output, artifact_files(apk, result, engine_record, licenses))
    print(json.dumps({"apk": str(apk), **record}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--engine-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--channel", choices=("release", "test"), required=True)
    args = parser.parse_args()
    package(args.engine, args.engine_manifest, args.output, args.channel)
