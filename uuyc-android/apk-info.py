#!/usr/bin/env python3
"""Report the package facts the UU Remote Android launcher needs.

Arch Linux installs no `aapt`/`apktool` by default, so this inspects the APK
directly. It deliberately does not try to fully decode the binary
AndroidManifest.xml: the string-pool offset table in this APK does not line up
with the stored strings, so the facts that matter are recovered from the raw
UTF-16 text of the manifest, the ZIP index and the DEX string tables.

The launcher activity is resolved authoritatively at run time by
`cmd package resolve-activity` inside Waydroid; the candidates printed here are
only a hint for diagnostics.

SPDX-License-Identifier: 0BSD
"""

from __future__ import annotations

import os
import re
import struct
import sys
import zipfile

PACKAGE_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+){2,}$")
SKIP_PREFIXES = (
    "android.",
    "androidx.",
    "com.google.",
    "com.alipay.",
    "com.huawei.",
    "com.tencent.",
    "com.bytedance.",
)


def dex_strings(blob: bytes) -> list[str]:
    """Return the string table of a DEX file, or nothing for protected blobs."""
    # Packed/encrypted DEX payloads are shipped under assets/; only plain DEX
    # files carry the dex\n magic and a readable string table.
    if not blob.startswith((b"dex\n035", b"dex\n036", b"dex\n037", b"dex\n038", b"dex\n039")):
        return []
    try:
        count, table_offset = struct.unpack_from("<II", blob, 0x38)
    except struct.error:
        return []
    if count <= 0 or table_offset <= 0 or table_offset + 4 * count > len(blob):
        return []
    strings: list[str] = []
    for index in range(count):
        data_offset = struct.unpack_from("<I", blob, table_offset + 4 * index)[0]
        cursor = data_offset
        size = 0
        shift = 0
        while cursor < len(blob):
            byte = blob[cursor]
            cursor += 1
            size |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                break
        strings.append(blob[cursor : cursor + size].decode("utf-8", "replace"))
    return strings


def parse_apk(path: str) -> dict[str, object]:
    with zipfile.ZipFile(path) as apk:
        names = apk.namelist()
        manifest = apk.read("AndroidManifest.xml")
        dex_blobs = [apk.read(n) for n in sorted(names) if n.endswith(".dex")]

    text = manifest.decode("utf-16-le", "ignore")

    packages: list[str] = []
    for candidate in re.findall(r"[a-z][a-z0-9_]*(?:\.[a-z0-9_]+){2,}", text):
        if PACKAGE_RE.match(candidate) and not candidate.startswith(SKIP_PREFIXES):
            if candidate not in packages:
                packages.append(candidate)

    # The application id is the namespace that other component names extend,
    # for example com.netease.uuremote plus com.netease.uuremote.wxapi.*
    package = ""
    best = -1
    for candidate in packages:
        prefix = candidate + "."
        score = sum(1 for other in packages if other.startswith(prefix))
        if score > best or (score == best and len(candidate) > len(package)):
            best, package = score, candidate

    # Version: prefer the name of the file, fall back to the upstream-version
    # marker the package ships next to the APK.
    version = re.search(r"_(\d+\.\d+\.\d+)\.apk$", path)
    if version:
        version_name = version.group(1)
    else:
        marker = os.path.join(os.path.dirname(os.path.abspath(path)), "upstream-version")
        version_name = ""
        if os.path.isfile(marker):
            with open(marker, encoding="utf-8") as handle:
                version_name = handle.read().strip()

    min_sdk = ""
    match = re.search(r"minSdkVersion(?:\x00[^\x00]{0,32}){0,8}?\x00(\d{1,3})\x00", text)
    if match:
        min_sdk = match.group(1)
    target_sdk = ""
    match = re.search(r"targetSdkVersion(?:\x00[^\x00]{0,32}){0,8}?\x00(\d{1,3})\x00", text)
    if match:
        target_sdk = match.group(1)

    classes: set[str] = set()
    for blob in dex_blobs:
        for value in dex_strings(blob):
            if package and package.replace(".", "/") in value:
                classes.add(value.replace("/", "."))
    launcher_candidates = sorted(
        name for name in classes if name.endswith(("LaunchActivity", "DeepLinkActivity"))
    )

    abis = sorted({n.split("/")[1] for n in names if n.startswith("lib/") and n.count("/") >= 1})
    libs = [n for n in names if n.startswith("lib/") and n.endswith(".so")]

    return {
        "package": package,
        "version_name": version_name,
        "min_sdk": min_sdk,
        "target_sdk": target_sdk,
        "launcher_candidates": launcher_candidates,
        "abis": abis,
        "native_lib_count": len(libs),
        "dex_count": len(dex_blobs),
    }


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} APK", file=sys.stderr)
        return 2

    info = parse_apk(argv[1])
    print(f"package            : {info['package'] or '?'}")
    print(f"version            : {info['version_name'] or '?'}")
    print(f"min / target sdk   : {info['min_sdk'] or '?'} / {info['target_sdk'] or '?'}")
    print(f"native abis        : {', '.join(info['abis']) or 'none'}")  # type: ignore[arg-type]
    print(f"native libraries   : {info['native_lib_count']}")
    print(f"dex files          : {info['dex_count']}")
    print("launcher candidates:")
    for candidate in info["launcher_candidates"] or ["(resolve at run time)"]:  # type: ignore[union-attr]
        print(f"  - {candidate}")
    if info["abis"] and "x86_64" not in info["abis"]:  # type: ignore[operator]
        print()
        print("NOTE: no x86_64 native code is shipped. On an x86_64 host this APK needs")
        print("      ARM translation (Waydroid + libndk/libhoudini) or a native ARM device.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
