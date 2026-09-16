"""Generate build data from our translation JSON and the verified game IPA."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

from package_probe import APP, UNITY, SOURCE_SHA256
from static_hook import plan, segments


def units(value):
    data = value.encode("utf-16-le")
    return struct.unpack("<" + "H" * (len(data) // 2), data)


def flatten(value):
    result = {}
    # nlohmann::json uses lexicographically sorted object keys in Local.cpp.
    for key, item in sorted(value.items()):
        if isinstance(item, dict):
            result.update(flatten(item))
        elif isinstance(item, str):
            result[key] = item
    return result


def prepare(ipa, translation, out):
    if hashlib.sha256(ipa.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError("IPA fingerprint mismatch")
    with zipfile.ZipFile(ipa) as archive:
        unity = archive.read(UNITY)
    hook = plan(unity)
    # Validate the three API RVAs against symbols in the game's nlist table.
    from package_probe import commands
    needed = {"_il2cpp_string_chars", "_il2cpp_string_length", "_il2cpp_string_new",
              "_il2cpp_string_new_utf16"}
    exports = {}
    for pos, cmd, _ in commands(unity):
        if cmd != 2:
            continue
        symoff, count, stroff, strsize = struct.unpack_from("<4I", unity, pos + 8)
        for i in range(count):
            no, typ, sect, desc, address = struct.unpack_from("<IBBHQ", unity, symoff + 16 * i)
            if not no or no >= strsize:
                continue
            end = unity.find(b"\0", stroff + no, stroff + strsize)
            name = unity[stroff + no:end].decode("utf-8", errors="replace")
            if name in needed and typ & 0x0E == 0x0E:
                exports[name] = address
    if set(exports) != needed:
        raise ValueError("Missing IL2CPP string API symbols")
    source = translation.read_bytes()
    dictionary = flatten(json.loads(source.decode("utf-8-sig")))
    ordered = sorted(dictionary.items(), key=lambda item: units(item[0]))
    pool, entries = [], []
    for key, value in ordered:
        ku, vu = units(key), units(value)
        ko = len(pool)
        pool.extend(ku)
        vo = len(pool)
        pool.extend(vu)
        entries.append((ko, len(ku), vo, len(vu)))
    out.mkdir(parents=True, exist_ok=True)
    lines = ["/* Generated from existing Android localization.json. Do not edit. */",
             "#pragma once", "#include <stdint.h>",
             "struct Translation { uint32_t key, key_len, value, value_len; };",
             "static const uint16_t translation_pool[] = {"]
    for i in range(0, len(pool), 24):
        lines.append(",".join(map(str, pool[i:i + 24])) + ",")
    lines += ["};", "static const struct Translation translations[] = {"]
    lines.extend("{" + ",".join(map(str, e)) + "}," for e in entries)
    lines += ["};", f"#define TRANSLATION_COUNT {len(entries)}"]
    (out / "translations.h").write_text("\n".join(lines) + "\n", encoding="utf-8")
    header = ["/* Verified against the 6.0.2 decrypted binary and device probe. */", "#pragma once"]
    for key in ("target_rva", "cave_rva", "slot_rva", "original_rva",
                "font_target_rva", "font_cave_rva", "font_slot_rva", "font_original_rva"):
        header.append(f"#define HOSHIMI_{key.upper()} 0x{hook[key]:x}ULL")
    for key in ("adv_target_rva", "adv_cave_rva", "adv_slot_rva", "adv_original_rva"):
        header.append(f"#define HOSHIMI_{key.upper()} 0x{hook[key]:x}ULL")
    for key in ("master_target_rva", "master_cave_rva", "master_slot_rva", "master_original_rva"):
        header.append(f"#define HOSHIMI_{key.upper()} 0x{hook[key]:x}ULL")
    for name in ("tmp_set_text", "tmp_populate", "tmp_settext_bool", "tmp_setchararray",
                 "textfield", "ui_text", "image_sprite", "image_override",
                 "image_texture", "image_enable"):
        for suffix in ("target_rva", "cave_rva", "slot_rva", "original_rva"):
            key = f"{name}_{suffix}"
            header.append(f"#define HOSHIMI_{key.upper()} 0x{hook[key]:x}ULL")
    for name, address in sorted(exports.items()):
        header.append(f"#define API{name.upper()} 0x{address:x}ULL")
    for key in ("gateway_hex", "patched_entry_hex", "font_gateway_hex", "font_patched_entry_hex",
                "adv_gateway_hex", "adv_patched_entry_hex"):
        
        header.append("static const unsigned char " + key + "[] = {" +
                      ",".join(str(x) for x in bytes.fromhex(hook[key])) + "};")
    for key in ("master_gateway_hex", "master_patched_entry_hex"):
        header.append("static const unsigned char " + key + "[] = {" +
                      ",".join(str(x) for x in bytes.fromhex(hook[key])) + "};")
    for name in ("tmp_set_text", "tmp_populate", "tmp_settext_bool", "tmp_setchararray",
                 "textfield", "ui_text", "image_sprite", "image_override",
                 "image_texture", "image_enable"):
        for suffix in ("gateway_hex", "patched_entry_hex", "target_original_hex"):
            key = f"{name}_{suffix}"
            header.append("static const unsigned char " + key + "[] = {" +
                          ",".join(str(x) for x in bytes.fromhex(hook[key])) + "};")
    (out / "hook_profile.h").write_text("\n".join(header) + "\n", encoding="utf-8")
    hook.update({"translation_count": len(entries), "translation_sha256": hashlib.sha256(source).hexdigest(),
                 "string_api_rvas": exports})
    (out / "hook-plan.json").write_text(json.dumps(hook, indent=2) + "\n", encoding="utf-8")
    (out / "gateway.bin").write_bytes(bytes.fromhex(hook["gateway_hex"]))
    print(json.dumps({k: v for k, v in hook.items() if k != "gateway_hex"}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ipa", required=True, type=Path)
    parser.add_argument("--translations", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.ipa, args.translations, args.out)
