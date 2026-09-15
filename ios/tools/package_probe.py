"""Embed the diagnostic dylib in the verified 6.0.2 IPA for external signing.

No input files are changed. No Apple account, network, or signing tool is used.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import struct
import zipfile

SOURCE_SHA256 = "3f7a5ee6b3a1670754a1293d5affb6b7b3e5d78ae9ad4a54154d1444a457bb0d"
APP = "Payload/IDOLY PRIDE.app/"
UNITY = APP + "Frameworks/UnityFramework.framework/UnityFramework"
FONT_ASSET = APP + "Data/sharedassets0.assets"
LIBRARY_NAME = "HoshimiProbe.dylib"
LOAD_PATH = "@loader_path/../" + LIBRARY_NAME
DEFAULT_BUNDLE_ID = "game.qualiarts.idolypride.kr"
DISPLAY_NAME = "아이프라"
APP_ID_NAME = "IdolyPrideKR"
PROBE_BUNDLE_ID = DEFAULT_BUNDLE_ID + ".probe"
SETTINGS_ROOT = APP + "Settings.bundle/Root.plist"
LOCAL_DATA_ROOT = APP + "HoshimiLocal/"
GENERIC_INDEX_NAME = LOCAL_DATA_ROOT + "generic.bin"
MASTER_INDEX_NAME = LOCAL_DATA_ROOT + "master.bin"
DOBBY_NAME = "libdobby.dylib"
DOBBY_ARCHIVE_NAME = APP + "Frameworks/" + DOBBY_NAME
DOBBY_REQUIRED_EXPORTS = (b"_DobbyHook\0", b"_DobbyDestroy\0")


def settings_plist():
    """Native Settings.app controls; values are stored in this app's defaults domain."""
    return plistlib.dumps({
        "StringsTable": "Root",
        "PreferenceSpecifiers": [
            {
                "Type": "PSGroupSpecifier",
                "Title": "아이프라 한글패치",
                "FooterText": "변경 후 게임을 완전히 종료하고 다시 실행하세요.",
            },
            {
                "Type": "PSToggleSwitchSpecifier",
                "Title": "한글패치 사용",
                "Key": "HoshimiLocalifyEnabled",
                "DefaultValue": True,
                "TrueValue": True,
                "FalseValue": False,
            },
            {
                "Type": "PSGroupSpecifier",
                "Title": "번역 기능",
                "FooterText": "각 항목은 게임을 완전히 종료한 뒤 다시 실행하면 적용됩니다.",
            },
            {
                "Type": "PSToggleSwitchSpecifier",
                "Title": "마스터 DB 번역",
                "Key": "useMasterTrans",
                "DefaultValue": True,
                "TrueValue": True,
                "FalseValue": False,
            },
            {
                "Type": "PSToggleSwitchSpecifier",
                "Title": "이미지 교체",
                "Key": "replaceImages",
                "DefaultValue": True,
                "TrueValue": True,
                "FalseValue": False,
            },
            {
                "Type": "PSToggleSwitchSpecifier",
                "Title": "전화 자막 사용",
                "Key": "usePhoneSubtitles",
                "DefaultValue": True,
                "TrueValue": True,
                "FalseValue": False,
            },
            {
                "Type": "PSGroupSpecifier",
                "Title": "개발자 기능",
            },
            {
                "Type": "PSToggleSwitchSpecifier",
                "Title": "주소 진단 모드",
                "Key": "HoshimiDiagnosticsEnabled",
                "DefaultValue": False,
                "TrueValue": True,
                "FalseValue": False,
            },
        ],
    }, fmt=plistlib.FMT_BINARY)


def collect_local_payload(root, include_adv=False, include_master=False):
    """Select files directly from the hoshimi-local checkout without rewriting them."""
    if not include_adv and not include_master:
        return []
    root = root.resolve()
    version = root / "version.txt"
    if not version.is_file():
        raise ValueError("hoshimi-local version.txt was not found")
    selected = [version]
    groups = []
    if include_adv:
        groups.append(root / "local-files" / "resource" / "adv")
    if include_master and not (root / "local-files" / "masterTrans").is_dir():
        raise ValueError("hoshimi-local MasterDB directory was not found")
    if include_adv or include_master:
        if not (root / "local-files" / "genericTrans").is_dir():
            raise ValueError("hoshimi-local genericTrans directory was not found")
    for group in groups:
        if not group.is_dir():
            raise ValueError(f"hoshimi-local data directory was not found: {group}")
        selected.extend(sorted((p for p in group.rglob("*") if p.is_file()),
                               key=lambda p: p.as_posix()))
    payload = []
    for path in selected:
        resolved = path.resolve()
        try:
            relative = resolved.relative_to(root)
        except ValueError as error:
            raise ValueError(f"Local data escaped its repository: {path}") from error
        archive_name = LOCAL_DATA_ROOT + relative.as_posix()
        payload.append((archive_name, resolved))
    return payload


def commands(data, alignment=8):
    if len(data) < 32:
        raise ValueError("Truncated Mach-O header")
    magic, cpu, _, _, count, size, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C:
        raise ValueError("Only thin little-endian arm64 Mach-O is supported")
    end = 32 + size
    if end > len(data):
        raise ValueError("Truncated load commands")
    pos = 32
    result = []
    for _ in range(count):
        if pos + 8 > end:
            raise ValueError("Invalid command count")
        cmd, length = struct.unpack_from("<II", data, pos)
        if length < 8 or length % alignment or pos + length > end:
            raise ValueError("Invalid load command size")
        if cmd in (0x21, 0x2C):
            if length < 20 or struct.unpack_from("<I", data, pos + 16)[0]:
                raise ValueError("Mach-O is still encrypted")
        result.append((pos, cmd, length))
        pos += length
    if pos != end:
        raise ValueError("Load command boundary mismatch")
    return result


def add_load_dylib(data, name):
    entries = commands(data)
    old_end = 32 + struct.unpack_from("<I", data, 20)[0]
    text_section_offsets = []
    for pos, cmd, size in entries:
        if cmd == 0xC:
            offset = struct.unpack_from("<I", data, pos + 8)[0]
            if data[pos + offset:pos + size].split(b"\0", 1)[0] == name.encode():
                raise ValueError("Library load command already exists")
        if cmd != 0x19:
            continue
        if size < 72:
            raise ValueError("Truncated segment")
        nsects = struct.unpack_from("<I", data, pos + 64)[0]
        if size != 72 + nsects * 80:
            raise ValueError("Unexpected section layout")
        if data[pos + 8:pos + 24].rstrip(b"\0") != b"__TEXT":
            continue
        for i in range(nsects):
            section = pos + 72 + 80 * i
            length = struct.unpack_from("<Q", data, section + 40)[0]
            offset = struct.unpack_from("<I", data, section + 48)[0]
            flags = struct.unpack_from("<I", data, section + 64)[0]
            if length and offset and flags & 0xFF not in (1, 12, 18):
                text_section_offsets.append(offset)
    if not text_section_offsets:
        raise ValueError("Cannot establish free header padding")
    path = name.encode("utf-8") + b"\0"
    size = (24 + len(path) + 7) & ~7
    limit = min(text_section_offsets)
    if old_end + size > limit or any(data[old_end:old_end + size]):
        raise ValueError("No verified zero header padding; refusing to move code")
    payload = struct.pack("<6I", 0xC, size, 24, 0, 0x10000, 0x10000) + path
    output = bytearray(data)
    output[old_end:old_end + size] = payload.ljust(size, b"\0")
    struct.pack_into("<II", output, 16, len(entries) + 1, old_end - 32 + size)
    commands(output)
    # Header-only modification: code addresses and every byte after padding stay fixed.
    if output[old_end + size:] != data[old_end + size:]:
        raise ValueError("Unexpected data modification")
    return bytes(output), {"load_command_offset": old_end, "load_command_size": size,
                           "first_text_section_offset": limit}


def package(source, dylib, output, bundle_id, hook_plan_path=None, font_path=None,
            local_data_root=None, include_adv=False, include_master=False,
            patch_revision=None, dobby_path=None):
    if output.exists() or output.with_suffix(".report.json").exists():
        raise ValueError("Output already exists; choose a new output filename")
    if bundle_id != DEFAULT_BUNDLE_ID and not bundle_id.startswith("game.qualiarts.idolypride."):
        raise ValueError("Use game.qualiarts.idolypride.kr or a separate test bundle ID under game.qualiarts.idolypride.*")
    if any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-" for c in bundle_id):
        raise ValueError("Invalid bundle ID: Apple allows only letters, numbers, periods, and hyphens")
    if patch_revision is not None and (not str(patch_revision).isdigit() or int(patch_revision) <= 0):
        raise ValueError("Patch revision must be a positive integer")
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError("Input SHA-256 differs from the verified decrypted 6.0.2 IPA")
    lib = dylib.read_bytes()
    dobby = dobby_path.read_bytes() if dobby_path else None
    if hook_plan_path and not dobby:
        raise ValueError("--dobby is required for the runtime text hooks")
    if dobby:
        # This prebuilt uses valid 4-byte-aligned LC_RPATH commands (44/28
        # bytes), while the game binaries use the stricter 8-byte layout.
        commands(dobby, alignment=4)
        if struct.unpack_from("<I", dobby, 12)[0] != 6:
            raise ValueError("Dobby payload must be a Mach-O dylib")
        for symbol in DOBBY_REQUIRED_EXPORTS:
            if symbol not in dobby:
                raise ValueError(f"Dobby payload is missing required export: {symbol[:-1].decode()}")
    if (include_adv or include_master) and not local_data_root:
        raise ValueError("--local-data-root is required when embedding translation data")
    local_payload = collect_local_payload(local_data_root, include_adv, include_master) \
        if local_data_root else []
    master_blob = None
    master_details = None
    generic_blob = None
    generic_details = None
    if include_master:
        from master_data import compile_master
        master_blob, master_details = compile_master(local_data_root.resolve())
    if include_adv or include_master:
        from generic_data import compile_generic
        generic_blob, generic_details = compile_generic(local_data_root.resolve())
    hook_plan = json.loads(hook_plan_path.read_text(encoding="utf-8")) if hook_plan_path else None
    library_name = "HoshimiLocalify.dylib" if hook_plan else LIBRARY_NAME
    load_path = "@loader_path/../" + library_name
    commands(lib)
    if struct.unpack_from("<I", lib, 12)[0] != 6:
        raise ValueError("Payload must be a Mach-O dylib")
    with zipfile.ZipFile(source) as src:
        names = src.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate ZIP entries")
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name:
                raise ValueError("Unsafe archive path")
        if APP + "Frameworks/" + library_name in names:
            raise ValueError("Probe already exists")
        if dobby and DOBBY_ARCHIVE_NAME in names:
            raise ValueError("Dobby already exists")
        # Check every executable reported in the original IPA analysis.
        for name in (APP + "IDOLY PRIDE", UNITY,
                     APP + "Frameworks/AppLovinSDK.framework/AppLovinSDK",
                     APP + "Frameworks/libswift_Concurrency.dylib"):
            commands(src.read(name))
        unity = src.read(UNITY)
        if hook_plan:
            from static_hook import patch
            unity = patch(unity, hook_plan)
        patched_font_asset = None
        font_details = None
        if font_path:
            from patch_font_assets import _font_bytes, patch_bytes
            patched_font_asset, font_details = patch_bytes(
                src.read(FONT_ASSET), _font_bytes(font_path))
        unity, details = add_load_dylib(unity, load_path)
        info = plistlib.loads(src.read(APP + "Info.plist"))
        if (info.get("CFBundleIdentifier"), info.get("CFBundleShortVersionString"),
            info.get("CFBundleVersion")) != ("game.qualiarts.idolypride", "6.0.2", "141"):
            raise ValueError("Unexpected game identity")
        is_standalone_probe = not hook_plan and bundle_id == PROBE_BUNDLE_ID
        display_name = "아이프라 진단" if is_standalone_probe else DISPLAY_NAME
        app_id_name = "IdolyPrideKRProbe" if is_standalone_probe else APP_ID_NAME
        info["CFBundleIdentifier"] = bundle_id
        info["CFBundleDisplayName"] = display_name
        # iLoader sends CFBundleName as Apple's developer App ID name.
        # Keep that metadata ASCII while the user-facing display name stays Korean.
        info["CFBundleName"] = app_id_name
        if patch_revision is not None:
            info["CFBundleVersion"] = "141." + str(patch_revision)
        info["UIFileSharingEnabled"] = True
        info["LSSupportsOpeningDocumentsInPlace"] = True
        info_bytes = plistlib.dumps(info, fmt=plistlib.FMT_BINARY)
        settings_bytes = settings_plist()
        report = {"stage": "static-i18n-setvalue-hook" if hook_plan else "loader-and-il2cpp-resolver-only", "source_sha256": digest,
                  "dylib_sha256": hashlib.sha256(lib).hexdigest(), "bundle_id": bundle_id,
                  "display_name": display_name, "app_id_name": app_id_name,
                  "bundle_version": info["CFBundleVersion"],
                  "settings_toggle": "HoshimiLocalifyEnabled",
                  "diagnostics_toggle": "HoshimiDiagnosticsEnabled",
                  "load_path": load_path, "signing": "must be re-signed by iLoader or SideStore",
                  "log": "Documents/hoshimi-ios-hook.log" if hook_plan else "Documents/hoshimi-ios-probe.log", **details}
        if dobby:
            report["dobby"] = {"archive": DOBBY_ARCHIVE_NAME,
                               "sha256": hashlib.sha256(dobby).hexdigest(),
                               "mode": "runtime text hooks"}
        if local_payload:
            report["local_data"] = {
                "version": (local_data_root.resolve() / "version.txt").read_text(
                    encoding="utf-8-sig").strip(),
                "adv": include_adv,
                "master": include_master,
                "generic": bool(generic_blob),
                "files": len(local_payload),
                "bytes": sum(path.stat().st_size for _, path in local_payload) +
                         (len(master_blob) if master_blob else 0) +
                         (len(generic_blob) if generic_blob else 0),
                "source": "app/src/main/assets/hoshimi-local",
            }
            if master_details:
                report["local_data"]["master_index"] = master_details
            if generic_details:
                report["local_data"]["generic_index"] = generic_details
        if hook_plan:
            report.update({"hook": hook_plan, "font_replaced": False})
        if font_details:
            report.update({"font_replaced": True, "font": font_details})
        elif hook_plan:
            report["font_replaced"] = False
        output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(output, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as dst:
            for item in src.infolist():
                # Old resource signatures and provisioning profiles must not be reused.
                if "/_CodeSignature/" in item.filename or item.filename.endswith("/embedded.mobileprovision"):
                    continue
                if item.filename.startswith(APP + "Settings.bundle/"):
                    continue
                if item.filename.startswith(LOCAL_DATA_ROOT):
                    continue
                if item.filename == UNITY:
                    data = unity
                elif item.filename == FONT_ASSET and patched_font_asset is not None:
                    data = patched_font_asset
                elif item.filename == APP + "Info.plist":
                    data = info_bytes
                else:
                    data = src.read(item)
                dst.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
            entry = zipfile.ZipInfo(APP + "Frameworks/" + library_name)
            entry.create_system = 3
            entry.external_attr = 0o100755 << 16
            dst.writestr(entry, lib, compress_type=zipfile.ZIP_DEFLATED)
            if dobby:
                dobby_entry = zipfile.ZipInfo(DOBBY_ARCHIVE_NAME)
                dobby_entry.create_system = 3
                dobby_entry.external_attr = 0o100755 << 16
                dst.writestr(dobby_entry, dobby, compress_type=zipfile.ZIP_DEFLATED)
            dst.writestr(SETTINGS_ROOT, settings_bytes, compress_type=zipfile.ZIP_DEFLATED)
            for archive_name, path in local_payload:
                dst.write(path, archive_name, compress_type=zipfile.ZIP_DEFLATED,
                          compresslevel=6)
            if master_blob:
                dst.writestr(MASTER_INDEX_NAME, master_blob,
                             compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
            if generic_blob:
                dst.writestr(GENERIC_INDEX_NAME, generic_blob,
                             compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
            dst.writestr(APP + "hoshimi-probe.json", json.dumps(report, indent=2))
    with zipfile.ZipFile(output) as check:
        if check.testzip() is not None or check.read(UNITY) != unity:
            raise ValueError("Output ZIP verification failed")
        if check.read(APP + "Frameworks/" + library_name) != lib:
            raise ValueError("Embedded dylib mismatch")
        if dobby and check.read(DOBBY_ARCHIVE_NAME) != dobby:
            raise ValueError("Embedded Dobby mismatch")
        if check.read(SETTINGS_ROOT) != settings_bytes:
            raise ValueError("Embedded Settings.bundle mismatch")
        for archive_name, path in local_payload:
            if check.read(archive_name) != path.read_bytes():
                raise ValueError(f"Embedded local data mismatch: {archive_name}")
        if master_blob and check.read(LOCAL_DATA_ROOT + "master.bin") != master_blob:
            raise ValueError("Embedded MasterDB index mismatch")
        if generic_blob and check.read(GENERIC_INDEX_NAME) != generic_blob:
            raise ValueError("Embedded generic index mismatch")
    report["output_sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(".report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ipa", required=True, type=Path)
    parser.add_argument("--dylib", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    parser.add_argument("--hook-plan", type=Path, help="Verified hook-plan.json from build-hook.ps1")
    parser.add_argument("--font-file", type=Path,
                        help="OTF/TTF to replace SourceSansPro-Regular in sharedassets0.assets")
    parser.add_argument("--local-data-root", type=Path,
                        help="hoshimi-local repository root containing version.txt and local-files")
    parser.add_argument("--include-adv", action="store_true",
                        help="Embed local-files/resource/adv directly from hoshimi-local")
    parser.add_argument("--include-master", action="store_true",
                        help="Compile and embed all local-files/masterTrans translations")
    parser.add_argument("--patch-revision", type=int,
                        help="Use 141.<revision> as CFBundleVersion so iOS treats it as an update")
    parser.add_argument("--dobby", type=Path,
                        help="arm64 libdobby.dylib used for runtime text hooks")
    args = parser.parse_args()
    package(args.ipa, args.dylib, args.output, args.bundle_id, args.hook_plan, args.font_file,
            args.local_data_root, args.include_adv, args.include_master, args.patch_revision,
            args.dobby)
