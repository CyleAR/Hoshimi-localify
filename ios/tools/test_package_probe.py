import struct
import unittest

from package_probe import add_load_dylib, commands, LOAD_PATH, settings_plist, PROBE_BUNDLE_ID
import plistlib
from pathlib import Path
from tempfile import TemporaryDirectory

from package_probe import collect_local_payload, LOCAL_DATA_ROOT


def sample_image():
    data = bytearray(8192)
    struct.pack_into("<8I", data, 0, 0xFEEDFACF, 0x0100000C, 0, 6, 2, 176, 0, 0)
    struct.pack_into("<II16sQQQQIIII", data, 32,
                     0x19, 152, b"__TEXT", 0, 8192, 0, 8192, 5, 5, 1, 0)
    struct.pack_into("<16s16sQQIIIIIIII", data, 104,
                     b"__text", b"__TEXT", 4096, 4096, 4096, 2, 0, 0, 0, 0, 0, 0)
    struct.pack_into("<6I", data, 184, 0x2C, 24, 4096, 4096, 0, 0)
    data[4096:] = b"\xA5" * 4096
    return data


class HeaderInjectionTests(unittest.TestCase):
    def test_settings_toggle_defaults_to_enabled(self):
        root = plistlib.loads(settings_plist())
        toggles = {item["Key"]: item for item in root["PreferenceSpecifiers"] if "Key" in item}
        toggle = toggles["HoshimiLocalifyEnabled"]
        self.assertEqual(toggle["Key"], "HoshimiLocalifyEnabled")
        self.assertIs(toggle["DefaultValue"], True)
        self.assertEqual(toggle["Type"], "PSToggleSwitchSpecifier")
        for key in ("useMasterTrans", "replaceImages", "usePhoneSubtitles"):
            self.assertIs(toggles[key]["DefaultValue"], True)
        diagnostics = toggles["HoshimiDiagnosticsEnabled"]
        self.assertEqual(diagnostics["Key"], "HoshimiDiagnosticsEnabled")
        self.assertIs(diagnostics["DefaultValue"], False)

    def test_probe_bundle_id_is_separate_from_main_patch(self):
        self.assertEqual(PROBE_BUNDLE_ID, "game.qualiarts.idolypride.kr.probe")

    def test_local_payload_uses_hoshimi_local_layout(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "version.txt").write_text("20260915", encoding="utf-8")
            adv = root / "local-files" / "resource" / "adv"
            master = root / "local-files" / "masterTrans"
            generic = root / "local-files" / "genericTrans"
            adv.mkdir(parents=True)
            master.mkdir(parents=True)
            generic.mkdir(parents=True)
            (adv / "story.txt").write_text("대사", encoding="utf-8")
            (master / "Idol.json").write_text(
                '{"rule":["id|name"],"data":{"idol-1|name":"아이돌"}}', encoding="utf-8")
            (generic / "generic.json").write_text('{"일본어":"한국어"}', encoding="utf-8")
            payload = collect_local_payload(root, True, True)
            self.assertEqual([name for name, _ in payload], [
                LOCAL_DATA_ROOT + "version.txt",
                LOCAL_DATA_ROOT + "local-files/resource/adv/story.txt",
            ])
            from master_data import compile_master
            blob, details = compile_master(root)
            self.assertTrue(blob.startswith(b"HSMSTR1\0"))
            self.assertEqual(details["entries"], 1)
            from generic_data import compile_generic
            generic_blob, generic_details = compile_generic(root)
            self.assertTrue(generic_blob.startswith(b"HSGEN2\0"))
            self.assertEqual(generic_details["exact"], 1)
            self.assertEqual(generic_details["translated"], 2)

    def test_image_payload_is_opt_in_and_preserves_resource_paths(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "version.txt").write_text("test", encoding="utf-8")
            img = root / "local-files/resource/img/nested"
            img.mkdir(parents=True)
            data = b"PNG fixture bytes"
            (img / "button.png").write_bytes(data)
            self.assertEqual(collect_local_payload(root), [])
            payload = dict(collect_local_payload(root, include_images=True))
            name = LOCAL_DATA_ROOT + "local-files/resource/img/nested/button.png"
            self.assertEqual(payload[name].read_bytes(), data)
            self.assertEqual(len(payload), 2)

    def test_generic_compiler_preserves_format_and_split_maps(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            generic = root / "local-files" / "genericTrans"
            generic.mkdir(parents=True)
            (generic / "generic.split.json").write_text(
                '{"[__split__]日":"[__split__]일"}', encoding="utf-8")
            (generic / "generic.json").write_text(
                '{"通常":"보통","{0}日":"{0}일"}', encoding="utf-8")
            from generic_data import compile_generic, HEADER, ENTRY
            blob, details = compile_generic(root)
            self.assertEqual((details["exact"], details["format"], details["split"]), (1, 1, 1))
            magic, version, exact, fmt, split, translated, pool, size = HEADER.unpack_from(blob)
            self.assertEqual((magic.rstrip(b"\0"), version, exact, fmt, split,
                              translated, size),
                             (b"HSGEN2", 2, 1, 1, 1, 3, len(blob)))
            rows = [ENTRY.unpack_from(blob, HEADER.size + i * ENTRY.size)
                    for i in range(exact + fmt + split)]
            strings = blob[pool:]
            values = [(strings[row[0]:].split(b"\0", 1)[0].decode(),
                       strings[row[1]:].split(b"\0", 1)[0].decode()) for row in rows]
            self.assertIn(("通常", "보통"), values)
            self.assertIn(("{0}日", "{0}일"), values)
            self.assertIn(("日", "일"), values)

    def test_generic_compiler_collects_android_translated_text_guard(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            local = root / "local-files"
            generic = local / "genericTrans"
            master = local / "masterTrans"
            generic.mkdir(parents=True)
            master.mkdir(parents=True)
            (local / "localization.json").write_text(
                '{"menu":{"yes":"예"}}', encoding="utf-8")
            (generic / "generic.json").write_text(
                '{"通常":"보통"}', encoding="utf-8")
            (master / "Idol.json").write_text(
                '{"rule":["id|name"],"data":{"1|name":"아이돌"}}', encoding="utf-8")
            from generic_data import compile_generic, HEADER, ENTRY
            blob, details = compile_generic(root)
            self.assertEqual(details["translated"], 3)
            _, _, exact, fmt, split, translated, pool, _ = HEADER.unpack_from(blob)
            start = exact + fmt + split
            rows = [ENTRY.unpack_from(blob, HEADER.size + (start + i) * ENTRY.size)
                    for i in range(translated)]
            strings = blob[pool:]
            guarded = {strings[row[0]:].split(b"\0", 1)[0].decode() for row in rows}
            self.assertEqual(guarded, {"예", "보통", "아이돌"})

    def test_load_command_preserves_code_and_existing_commands(self):
        source = sample_image()
        output, report = add_load_dylib(source, LOAD_PATH)
        self.assertEqual(len(source), len(output))
        self.assertEqual(source[32:208], output[32:208])
        self.assertEqual(source[4096:], output[4096:])
        self.assertEqual(len(commands(output)), 3)
        start = report["load_command_offset"]
        self.assertEqual(output[start + 24:].split(b"\0", 1)[0], LOAD_PATH.encode())

    def test_rejects_encrypted_input(self):
        source = sample_image()
        struct.pack_into("<I", source, 200, 1)
        with self.assertRaisesRegex(ValueError, "encrypted"):
            add_load_dylib(source, LOAD_PATH)

    def test_rejects_nonempty_padding(self):
        source = sample_image()
        source[210] = 1
        with self.assertRaisesRegex(ValueError, "padding"):
            add_load_dylib(source, LOAD_PATH)

    def test_rejects_short_padding(self):
        source = sample_image()
        struct.pack_into("<I", source, 152, 216)
        with self.assertRaisesRegex(ValueError, "padding"):
            add_load_dylib(source, LOAD_PATH)

    def test_rejects_duplicate_injection(self):
        output, _ = add_load_dylib(sample_image(), LOAD_PATH)
        with self.assertRaisesRegex(ValueError, "already exists"):
            add_load_dylib(output, LOAD_PATH)

    def test_rejects_truncated_commands(self):
        with self.assertRaisesRegex(ValueError, "Truncated"):
            commands(sample_image()[:100])


if __name__ == "__main__":
    unittest.main()
