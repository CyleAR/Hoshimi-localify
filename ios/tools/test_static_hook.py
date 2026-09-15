"""ARM64 instruction execution tests; requires capstone and unicorn.

These tests run the generated gateway with both a null slot and an armed hook,
including a nonzero ASLR slide. No iPad or game process is contacted.
"""
import unittest
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X16, UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_Q0

from static_hook import (TARGET, EXPECTED, FONT_TARGET, FONT_EXPECTED,
                         ADV_TARGET, ADV_EXPECTED, MASTER_TARGET, MASTER_EXPECTED,
                         TMP_SET_TEXT_TARGET, TMP_SET_TEXT_EXPECTED,
                         TMP_POPULATE_TARGET, TMP_POPULATE_EXPECTED,
                         TMP_SETTEXT_BOOL_TARGET, TMP_SETTEXT_BOOL_EXPECTED,
                         TMP_SETCHARARRAY_TARGET, TMP_SETCHARARRAY_EXPECTED,
                         TEXTFIELD_TARGET, TEXTFIELD_EXPECTED,
                         UI_TEXT_TARGET, UI_TEXT_EXPECTED,
                         branch, gateway)

CAVE = 0x9788050
SLOT = 0xAB944D8
FONT_CAVE = 0x9788070
FONT_SLOT = 0xAB944E0
ADV_CAVE = 0x9788090
ADV_SLOT = 0xAB944E8
MASTER_CAVE = 0x97880B0
MASTER_SLOT = 0xAB944F0
TMP_SET_TEXT_CAVE = 0x97880D0
TMP_SET_TEXT_SLOT = 0xAB944F8
TMP_POPULATE_CAVE = 0x97880F0
TMP_POPULATE_SLOT = 0xAB94500
TMP_SETTEXT_BOOL_CAVE = 0x9788110
TMP_SETTEXT_BOOL_SLOT = 0xAB94508
TMP_SETCHARARRAY_CAVE = 0x9788130
TMP_SETCHARARRAY_SLOT = 0xAB94510
TEXTFIELD_CAVE = 0x9788150
TEXTFIELD_SLOT = 0xAB94518
UI_TEXT_CAVE = 0x9788170
UI_TEXT_SLOT = 0xAB94520


class GatewayExecutionTests(unittest.TestCase):
    def run_gateway(self, armed, slide, trampoline=False, target=TARGET, cave=CAVE,
                    slot=SLOT, expected=EXPECTED, saved=(26, 25), stack_size=80,
                    changed_register=None, changed_value=0):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        hook = slide + 0x20000000
        stack = slide + 0x30000000
        for page in {((slide + a) & ~0xFFF) for a in (target, cave, slot)} | {hook, stack}:
            uc.mem_map(page, 4096)
        uc.mem_write(slide + target, struct.pack("<I", branch(target, cave)))
        uc.mem_write(slide + cave, gateway(cave, slot, target, expected[:4]))
        uc.mem_write(slide + slot, struct.pack("<Q", hook if armed else 0))
        sp = stack + 2048
        before = {}
        # The Unicorn register enum is contiguous for x0..x28 (x29/x30 are separate).
        for i in range(29):
            before[i] = 0x110000 + i * 4096
            uc.reg_write(UC_ARM64_REG_X0 + i, before[i])
        from unicorn.arm64_const import UC_ARM64_REG_X29, UC_ARM64_REG_X30
        uc.reg_write(UC_ARM64_REG_X29, 0xF029)
        uc.reg_write(UC_ARM64_REG_X30, 0xF030)
        for i in range(8):
            uc.reg_write(UC_ARM64_REG_Q0 + i, (0x12345678 << 64) + i)
        uc.reg_write(UC_ARM64_REG_SP, sp)
        stop = hook if armed and not trampoline else slide + target + 4

        def stop_at_destination(emulator, address, size, user):
            if address == stop:
                emulator.emu_stop()
        uc.hook_add(UC_HOOK_CODE, stop_at_destination)
        start = slide + cave + 20 if trampoline else slide + target
        uc.emu_start(start, 0, count=32)
        self.assertEqual(uc.reg_read(UC_ARM64_REG_PC), stop)
        for i, value in before.items():
            if i not in (16, changed_register):
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X0 + i), value, f"x{i} changed")
        if changed_register is not None and stop != hook:
            self.assertEqual(uc.reg_read(UC_ARM64_REG_X0 + changed_register), changed_value)
        self.assertEqual(uc.reg_read(UC_ARM64_REG_X29), 0xF029)
        self.assertEqual(uc.reg_read(UC_ARM64_REG_X30), 0xF030)
        for i in range(8):
            self.assertEqual(uc.reg_read(UC_ARM64_REG_Q0 + i), (0x12345678 << 64) + i)
        if stop == hook:
            self.assertEqual(uc.reg_read(UC_ARM64_REG_SP), sp)
        else:
            self.assertEqual(uc.reg_read(UC_ARM64_REG_SP), sp - stack_size)
            if saved:
                self.assertEqual(bytes(uc.mem_read(sp - stack_size, 16)),
                                 struct.pack("<QQ", before[saved[0]], before[saved[1]]))

    def test_unarmed_falls_back_to_original(self):
        self.run_gateway(False, 0)

    def test_armed_preserves_function_arguments(self):
        self.run_gateway(True, 0)

    def test_original_trampoline_preserves_prologue(self):
        self.run_gateway(True, 0, trampoline=True)

    def test_aslr_unarmed(self):
        self.run_gateway(False, 0x103A40000)

    def test_aslr_armed(self):
        self.run_gateway(True, 0x103A40000)

    def test_font_awake_unarmed_falls_back(self):
        self.run_gateway(False, 0, target=FONT_TARGET, cave=FONT_CAVE, slot=FONT_SLOT,
                         expected=FONT_EXPECTED, saved=(22, 21), stack_size=48)

    def test_font_awake_armed_preserves_arguments(self):
        self.run_gateway(True, 0x103A40000, target=FONT_TARGET, cave=FONT_CAVE,
                         slot=FONT_SLOT, expected=FONT_EXPECTED, saved=(22, 21), stack_size=48)

    def test_font_awake_original_trampoline(self):
        self.run_gateway(True, 0, trampoline=True, target=FONT_TARGET, cave=FONT_CAVE,
                         slot=FONT_SLOT, expected=FONT_EXPECTED, saved=(22, 21), stack_size=48)

    def test_adv_unarmed_falls_back(self):
        self.run_gateway(False, 0, target=ADV_TARGET, cave=ADV_CAVE, slot=ADV_SLOT,
                         expected=ADV_EXPECTED, saved=(24, 23), stack_size=64)

    def test_adv_armed_preserves_arguments(self):
        self.run_gateway(True, 0x103A40000, target=ADV_TARGET, cave=ADV_CAVE, slot=ADV_SLOT,
                         expected=ADV_EXPECTED, saved=(24, 23), stack_size=64)

    def test_adv_original_trampoline(self):
        self.run_gateway(True, 0, trampoline=True, target=ADV_TARGET, cave=ADV_CAVE,
                         slot=ADV_SLOT, expected=ADV_EXPECTED, saved=(24, 23), stack_size=64)

    def test_master_unarmed_falls_back(self):
        self.run_gateway(False, 0, target=MASTER_TARGET, cave=MASTER_CAVE, slot=MASTER_SLOT,
                         expected=MASTER_EXPECTED, saved=None, stack_size=0,
                         changed_register=3)

    def test_master_armed_preserves_arguments(self):
        self.run_gateway(True, 0x103A40000, target=MASTER_TARGET, cave=MASTER_CAVE,
                         slot=MASTER_SLOT, expected=MASTER_EXPECTED, saved=None, stack_size=0)

    def test_master_original_trampoline(self):
        self.run_gateway(True, 0, trampoline=True, target=MASTER_TARGET, cave=MASTER_CAVE,
                         slot=MASTER_SLOT, expected=MASTER_EXPECTED, saved=None, stack_size=0,
                         changed_register=3)

    def test_generic_text_gateways_preserve_arguments(self):
        sites = ((TMP_SET_TEXT_TARGET, TMP_SET_TEXT_CAVE, TMP_SET_TEXT_SLOT, TMP_SET_TEXT_EXPECTED, (20, 19), 32),
                 (TMP_POPULATE_TARGET, TMP_POPULATE_CAVE, TMP_POPULATE_SLOT, TMP_POPULATE_EXPECTED, (24, 23), 64),
                 (TMP_SETTEXT_BOOL_TARGET, TMP_SETTEXT_BOOL_CAVE, TMP_SETTEXT_BOOL_SLOT, TMP_SETTEXT_BOOL_EXPECTED, (20, 19), 32),
                 (TMP_SETCHARARRAY_TARGET, TMP_SETCHARARRAY_CAVE, TMP_SETCHARARRAY_SLOT, TMP_SETCHARARRAY_EXPECTED, (20, 19), 32),
                 (TEXTFIELD_TARGET, TEXTFIELD_CAVE, TEXTFIELD_SLOT, TEXTFIELD_EXPECTED, (24, 23), 64),
                 (UI_TEXT_TARGET, UI_TEXT_CAVE, UI_TEXT_SLOT, UI_TEXT_EXPECTED, (22, 21), 48))
        for target, cave, slot, expected, saved, stack_size in sites:
            with self.subTest(target=hex(target)):
                self.run_gateway(True, 0x103A40000, target=target, cave=cave, slot=slot,
                                 expected=expected, saved=saved, stack_size=stack_size)

    def test_machine_code_decodes_as_intended(self):
        instructions = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(gateway(CAVE, SLOT, TARGET, EXPECTED[:4]), CAVE))
        self.assertEqual([i.mnemonic for i in instructions], ["adrp", "add", "ldar", "cbz", "br", "stp", "b"])
        self.assertEqual(instructions[2].op_str, "x16, [x16]")
        self.assertEqual(instructions[5].op_str, "x26, x25, [sp, #-0x50]!")

    def test_font_machine_code_decodes_as_intended(self):
        instructions = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(
            gateway(FONT_CAVE, FONT_SLOT, FONT_TARGET, FONT_EXPECTED[:4]), FONT_CAVE))
        self.assertEqual([i.mnemonic for i in instructions], ["adrp", "add", "ldar", "cbz", "br", "stp", "b"])
        self.assertEqual(instructions[5].op_str, "x22, x21, [sp, #-0x30]!")

    def test_adv_machine_code_decodes_as_intended(self):
        instructions = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(
            gateway(ADV_CAVE, ADV_SLOT, ADV_TARGET, ADV_EXPECTED[:4]), ADV_CAVE))
        self.assertEqual([i.mnemonic for i in instructions], ["adrp", "add", "ldar", "cbz", "br", "stp", "b"])
        self.assertEqual(instructions[5].op_str, "x24, x23, [sp, #-0x40]!")

    def test_master_machine_code_decodes_as_intended(self):
        instructions = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(
            gateway(MASTER_CAVE, MASTER_SLOT, MASTER_TARGET, MASTER_EXPECTED[:4]), MASTER_CAVE))
        self.assertEqual([i.mnemonic for i in instructions], ["adrp", "add", "ldar", "cbz", "br", "mov", "b"])
        self.assertEqual(instructions[5].op_str, "w3, #0")

    def test_rejects_unknown_displaced_instruction(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            gateway(CAVE, SLOT, TARGET, b"\0" * 4)

    def test_rejects_branch_out_of_range(self):
        with self.assertRaisesRegex(ValueError, "range"):
            branch(0, 1 << 27)


if __name__ == "__main__":
    unittest.main()
