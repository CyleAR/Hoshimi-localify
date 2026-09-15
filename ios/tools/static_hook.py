"""Version-locked ARM64 hook: branch through a pointer in unused segment slack.

The input hash is the analyzed decrypted UnityFramework. This deliberately does
not implement a generic ARM64 relocator or take space from existing BSS objects.
"""
import hashlib
import struct

from package_probe import commands

UNITY_SHA256 = "1029ea4fbdfef985f9f208913deac78c7633974dc8b9fe3abde8643974bfb809"
TARGET = 0x6EFC218
HELPER = 0x6EFD66C
FONT_TARGET = 0x771D380
ADV_TARGET = 0x6E7E724
MASTER_TARGET = 0x677EF64
TMP_SET_TEXT_TARGET = 0x7737CD0
TMP_POPULATE_TARGET = 0x773C070
TMP_SETTEXT_BOOL_TARGET = 0x773D790
TMP_SETCHARARRAY_TARGET = 0x773E070
TEXTFIELD_TARGET = 0x7A4428C
UI_TEXT_TARGET = 0x7B3CE88
MODULE = 0x9DEEB28
TOKEN_ROW = 0x26F
HELPER_ROW = 0x27E
EXPECTED = bytes.fromhex("fa67bba9f85f01a9f65702a9f44f03a9")
FONT_EXPECTED = bytes.fromhex("f657bda9f44f01a9fd7b02a9fd830091")
ADV_EXPECTED = bytes.fromhex("f85fbca9f65701a9f44f02a9fd7b03a9")
MASTER_EXPECTED = bytes.fromhex("03008052040080d201000014ff8303d1")
TMP_SET_TEXT_EXPECTED = bytes.fromhex("f44fbea9fd7b01a9fd43009108804339")
TMP_POPULATE_EXPECTED = bytes.fromhex("f85fbca9f65701a9f44f02a9fd7b03a9")
TMP_SETTEXT_BOOL_EXPECTED = bytes.fromhex("f44fbea9fd7b01a9fd430091f30301aa")
TMP_SETCHARARRAY_EXPECTED = bytes.fromhex("f44fbea9fd7b01a9fd430091f30300aa")
TEXTFIELD_EXPECTED = bytes.fromhex("f85fbca9f65701a9f44f02a9fd7b03a9")
UI_TEXT_EXPECTED = bytes.fromhex("f657bda9f44f01a9fd7b02a9fd830091")


def branch(pc, target):
    delta = target - pc
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError("ARM64 branch out of range or unaligned")
    return 0x14000000 | ((delta // 4) & 0x3FFFFFF)


def gateway(cave, slot, target, displaced):
    if cave % 4 or slot % 8 or len(displaced) != 4:
        raise ValueError("Invalid gateway alignment/size")
    # Only relocate the exact STP x26,x25,[sp,#-80]! prologue. It neither
    # addresses PC-relative data nor consumes ABI scratch registers x16/x17.
    if displaced not in (EXPECTED[:4], FONT_EXPECTED[:4], ADV_EXPECTED[:4],
                          MASTER_EXPECTED[:4], TMP_SET_TEXT_EXPECTED[:4],
                          TMP_POPULATE_EXPECTED[:4], TMP_SETTEXT_BOOL_EXPECTED[:4],
                          TMP_SETCHARARRAY_EXPECTED[:4], TEXTFIELD_EXPECTED[:4],
                          UI_TEXT_EXPECTED[:4]):
        raise ValueError("Unsupported displaced instruction")
    page_delta = (slot >> 12) - (cave >> 12)
    if not -(1 << 20) <= page_delta < (1 << 20):
        raise ValueError("ADRP target out of range")
    adrp = 0x90000010 | ((page_delta & 3) << 29) | (((page_delta >> 2) & 0x7FFFF) << 5)
    add = 0x91000210 | ((slot & 0xFFF) << 10)
    # x16 is an AAPCS64 inter-procedure-call scratch register. x0-x8, vectors,
    # stack, frame pointer and LR are unchanged until the original STP runs.
    return (struct.pack("<5I", adrp, add, 0xC8DFFE10, 0xB4000050, 0xD61F0200)
            + displaced + struct.pack("<I", branch(cave + 24, target + 4)))


def segments(data):
    result = {}
    for pos, cmd, size in commands(data):
        if cmd != 0x19:
            continue
        name, va, vs, off, fs, mp, ip, count, flags = struct.unpack_from("<16s4Q4I", data, pos + 8)
        if size != 72 + count * 80:
            raise ValueError("Invalid segment sections")
        sections = []
        for i in range(count):
            vals = struct.unpack_from("<16s16sQQIIIIIIII", data, pos + 72 + i * 80)
            sections.append({"name": vals[0].rstrip(b"\0").decode(), "va": vals[2],
                             "size": vals[3], "offset": vals[4], "flags": vals[8]})
        result[name.rstrip(b"\0").decode()] = {
            "va": va, "vmsize": vs, "offset": off, "filesize": fs,
            "prot": ip, "sections": sections, "flags": flags}
    return result


def plan(data):
    if hashlib.sha256(data).hexdigest() != UNITY_SHA256:
        raise ValueError("UnityFramework fingerprint mismatch")
    segs = segments(data)
    text, rw = segs["__TEXT"], segs["__DATA"]
    if text["va"] != 0 or text["offset"] != 0 or text["prot"] != 5 or rw["prot"] != 3:
        raise ValueError("Unexpected segment mapping/protections")
    if rw["flags"] & 0x10:  # SG_READ_ONLY: do not allocate a mutable slot here.
        raise ValueError("Data segment may be made read-only")

    def va_to_offset(address, length=8):
        for s in segs.values():
            relative = address - s["va"]
            if 0 <= relative and relative + length <= s["filesize"]:
                return s["offset"] + relative
        raise ValueError("Address is not file-backed")

    # Correlate the runtime-reported wrapper with the IL2CPP registration table,
    # then use token 0x0600026f for the actual Android-hooked I18n.SetValue.
    name, count, table = struct.unpack_from("<3Q", data, va_to_offset(MODULE, 24))
    name_off = va_to_offset(name)
    if data[name_off:name_off + 23] != b"quaunity-ui.Runtime.dll":
        raise ValueError("Codegen module name mismatch")
    if count < HELPER_ROW:
        raise ValueError("Codegen method count mismatch")
    for row, expected in ((TOKEN_ROW, TARGET), (HELPER_ROW, HELPER)):
        actual = struct.unpack_from("<Q", data, va_to_offset(table + 8 * (row - 1)))[0]
        if actual != expected:
            raise ValueError("Codegen method pointer mismatch")
    tail = HELPER + 52
    if struct.unpack_from("<I", data, tail)[0] != branch(tail, TARGET):
        raise ValueError("Helper no longer tail-calls I18n.SetValue")
    if data[TARGET:TARGET + len(EXPECTED)] != EXPECTED:
        raise ValueError("Unexpected target prologue")
    if data[FONT_TARGET:FONT_TARGET + len(FONT_EXPECTED)] != FONT_EXPECTED:
        raise ValueError("Unexpected TMP_FontAsset.Awake prologue")
    if data[ADV_TARGET:ADV_TARGET + len(ADV_EXPECTED)] != ADV_EXPECTED:
        raise ValueError("Unexpected OctoResourceLoader ADV prologue")
    if data[MASTER_TARGET:MASTER_TARGET + len(MASTER_EXPECTED)] != MASTER_EXPECTED:
        raise ValueError("Unexpected MessageExtensions.MergeFrom prologue")
    text_sites = [
        ("tmp_set_text", TMP_SET_TEXT_TARGET, TMP_SET_TEXT_EXPECTED),
        ("tmp_populate", TMP_POPULATE_TARGET, TMP_POPULATE_EXPECTED),
        ("tmp_settext_bool", TMP_SETTEXT_BOOL_TARGET, TMP_SETTEXT_BOOL_EXPECTED),
        ("tmp_setchararray", TMP_SETCHARARRAY_TARGET, TMP_SETCHARARRAY_EXPECTED),
        ("textfield", TEXTFIELD_TARGET, TEXTFIELD_EXPECTED),
        ("ui_text", UI_TEXT_TARGET, UI_TEXT_EXPECTED),
    ]
    for name, target, expected in text_sites:
        if data[target:target + len(expected)] != expected:
            raise ValueError(f"Unexpected {name} prologue")

    # Reserve after ALL declared sections, never within __bss/__common.
    text_end = max(s["va"] + s["size"] for s in text["sections"])
    data_end = max(s["va"] + s["size"] for s in rw["sections"])
    cave = (text_end + 15) & ~15
    slot = (data_end + 7) & ~7
    main_code = gateway(cave, slot, TARGET, EXPECTED[:4])
    font_cave = (cave + len(main_code) + 15) & ~15
    font_slot = slot + 8
    font_code = gateway(font_cave, font_slot, FONT_TARGET, FONT_EXPECTED[:4])
    adv_cave = (font_cave + len(font_code) + 15) & ~15
    adv_slot = font_slot + 8
    adv_code = gateway(adv_cave, adv_slot, ADV_TARGET, ADV_EXPECTED[:4])
    master_cave = (adv_cave + len(adv_code) + 15) & ~15
    master_slot = adv_slot + 8
    master_code = gateway(master_cave, master_slot, MASTER_TARGET, MASTER_EXPECTED[:4])
    next_cave = (master_cave + len(master_code) + 15) & ~15
    next_slot = master_slot + 8
    generated_sites = []
    for name, target, expected in text_sites:
        cave_address = next_cave
        slot_address = next_slot
        code = gateway(cave_address, slot_address, target, expected[:4])
        generated_sites.append((name, target, expected, cave_address, slot_address, code))
        next_cave = (cave_address + len(code) + 15) & ~15
        next_slot += 8
    if next_cave > text["va"] + min(text["filesize"], text["vmsize"]):
        raise ValueError("Insufficient executable segment slack")
    if (any(data[cave:cave + len(main_code)]) or
            any(data[font_cave:font_cave + len(font_code)]) or
            any(data[adv_cave:adv_cave + len(adv_code)]) or
            any(data[master_cave:master_cave + len(master_code)]) or
            any(any(data[c:c + len(code)]) for _, _, _, c, _, code in generated_sites)):
        raise ValueError("Executable slack is not zero")
    if not (slot >= rw["va"] + rw["filesize"] and next_slot <= rw["va"] + rw["vmsize"]):
        raise ValueError("No unused zero-fill data tail for hook pointer")
    result = {"target_rva": TARGET, "cave_rva": cave, "slot_rva": slot,
            "original_rva": cave + 20, "gateway_hex": main_code.hex(),
            "patched_entry_hex": struct.pack("<I", branch(TARGET, cave)).hex(),
            "target_original_hex": EXPECTED.hex(), "data_sections_end": data_end,
            "text_sections_end": text_end, "unity_sha256": UNITY_SHA256,
            "codegen_module_rva": MODULE, "method_token": "0x0600026f",
            "font_target_rva": FONT_TARGET, "font_cave_rva": font_cave,
            "font_slot_rva": font_slot, "font_original_rva": font_cave + 20,
            "font_gateway_hex": font_code.hex(),
            "font_patched_entry_hex": struct.pack("<I", branch(FONT_TARGET, font_cave)).hex(),
            "font_target_original_hex": FONT_EXPECTED.hex(),
            "adv_target_rva": ADV_TARGET, "adv_cave_rva": adv_cave,
            "adv_slot_rva": adv_slot, "adv_original_rva": adv_cave + 20,
            "adv_gateway_hex": adv_code.hex(),
            "adv_patched_entry_hex": struct.pack("<I", branch(ADV_TARGET, adv_cave)).hex(),
            "adv_target_original_hex": ADV_EXPECTED.hex(),
            "master_target_rva": MASTER_TARGET, "master_cave_rva": master_cave,
            "master_slot_rva": master_slot, "master_original_rva": master_cave + 20,
            "master_gateway_hex": master_code.hex(),
            "master_patched_entry_hex": struct.pack("<I", branch(MASTER_TARGET, master_cave)).hex(),
            "master_target_original_hex": MASTER_EXPECTED.hex()}
    for name, target, expected, cave_address, slot_address, code in generated_sites:
        result.update({
            f"{name}_target_rva": target,
            f"{name}_cave_rva": cave_address,
            f"{name}_slot_rva": slot_address,
            f"{name}_original_rva": cave_address + 20,
            f"{name}_gateway_hex": code.hex(),
            f"{name}_patched_entry_hex": struct.pack("<I", branch(target, cave_address)).hex(),
            f"{name}_target_original_hex": expected.hex(),
        })
    return result


def patch(data, expected_plan):
    actual = plan(data)
    if any(expected_plan.get(key) != value for key, value in actual.items()):
        raise ValueError("Build and package hook plans differ")
    output = bytearray(data)
    cave = actual["cave_rva"]
    code = bytes.fromhex(actual["gateway_hex"])
    output[cave:cave + len(code)] = code
    output[TARGET:TARGET + 4] = bytes.fromhex(actual["patched_entry_hex"])
    font_cave = actual["font_cave_rva"]
    font_code = bytes.fromhex(actual["font_gateway_hex"])
    output[font_cave:font_cave + len(font_code)] = font_code
    output[FONT_TARGET:FONT_TARGET + 4] = bytes.fromhex(actual["font_patched_entry_hex"])
    adv_cave = actual["adv_cave_rva"]
    adv_code = bytes.fromhex(actual["adv_gateway_hex"])
    output[adv_cave:adv_cave + len(adv_code)] = adv_code
    output[ADV_TARGET:ADV_TARGET + 4] = bytes.fromhex(actual["adv_patched_entry_hex"])
    master_cave = actual["master_cave_rva"]
    master_code = bytes.fromhex(actual["master_gateway_hex"])
    output[master_cave:master_cave + len(master_code)] = master_code
    output[MASTER_TARGET:MASTER_TARGET + 4] = bytes.fromhex(actual["master_patched_entry_hex"])
    # Text APIs are installed at runtime by the bundled arm64 Dobby library.
    # Keep their original prologues intact; stacking static gateways at every
    # nested TMP level duplicated numeric HUD strings on the device.
    return bytes(output)
