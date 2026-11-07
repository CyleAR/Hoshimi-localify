"""Inspect photo request methods in an Android ARM64 IL2CPP ELF using dump.cs RVAs.

Dependencies: pyelftools, capstone. This reads binaries; it does not patch them.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile


TARGETS = {
    "PhotoCreateExpressionShootingsRequest": {".ctor", "get_CreateShootingParams", "Clone"},
    "PhotoCreateExpressionShootingParam": {".ctor", "Clone"},
    "PhotoPanoramaCreateShootingsRequest": {"get_MainCharacterId", "get_StartCharacterId"},
    "UserGetResponse": {"pb::Google.Protobuf.IBufferMessage.InternalMergeFrom"},
    "UpdatedData": {"pb::Google.Protobuf.IBufferMessage.InternalMergeFrom"},
}


def methods(path):
    class_name, namespace, address = "", "", None
    with path.open(encoding="utf-8-sig") as stream:
        for number, line in enumerate(stream, 1):
            if line.startswith("// Namespace:"):
                namespace = line.partition(":")[2].strip()
                address = None
            match = re.match(r"public (?:sealed |abstract |static )?class (\S+)", line)
            if match:
                class_name, address = match[1], None
            if "// RVA:" in line:
                match = re.search(r"RVA: (0x[0-9A-Fa-f]+)", line)
                address = int(match[1], 16) if match else None
            if class_name in TARGETS and "(" in line:
                for method in TARGETS[class_name]:
                    if method + "(" in line and address:
                        yield dict(namespace=namespace, class_name=class_name,
                                   signature=line.strip(), dump_line=number, rva=address)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"elf_sha256": hashlib.sha256(args.elf.read_bytes()).hexdigest(), "methods": []}
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    with args.elf.open("rb") as stream:
        elf = ELFFile(stream)
        if elf["e_machine"] != "EM_AARCH64":
            raise ValueError("Expected an ARM64 ELF")
        segments = [segment for segment in elf.iter_segments() if segment["p_type"] == "PT_LOAD"]
        for method in methods(args.dump):
            address = method["rva"]
            segment = next((s for s in segments if s["p_vaddr"] <= address < s["p_vaddr"] + s["p_filesz"]), None)
            if segment is None:
                raise ValueError(f"Unmapped method RVA: {address:#x}")
            offset = segment["p_offset"] + address - segment["p_vaddr"]
            stream.seek(offset)
            instructions = []
            for instruction in decoder.disasm(stream.read(768), address):
                instructions.append(f"{instruction.address:#x}: {instruction.mnemonic} {instruction.op_str}")
                if instruction.mnemonic == "ret":
                    break
            method.update(file_offset=offset, instructions=instructions)
            report["methods"].append(method)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Inspected {len(report['methods'])} methods; SHA256 {report['elf_sha256']}")


if __name__ == "__main__":
    main()
