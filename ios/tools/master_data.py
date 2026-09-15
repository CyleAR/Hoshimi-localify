"""Compile hoshimi-local's flat MasterDB JSON into a bounded runtime index.

The Android loader accepts both direct fields (``id|name``) and the nested
field rules used by protobuf messages (``id|levels.description``).  The iOS
index keeps the rule's local path in the field table and stores every matching
flat data key, including ``levels[0].description`` array entries.  The native
hook resolves that path after each protobuf merge.
"""
import json
from pathlib import Path
import re
import struct

MAGIC = b"HSMSTR1\0"
HEADER = struct.Struct("<8s6I")
TABLE = struct.Struct("<7I")
ENTRY = struct.Struct("<2I")


def compile_master(root: Path):
    source = root / "local-files" / "masterTrans"
    if not source.is_dir():
        raise ValueError(f"MasterDB directory was not found: {source}")
    pool = bytearray(b"\0")
    offsets = {"": 0}

    def intern(value):
        if value not in offsets:
            encoded = value.encode("utf-8") + b"\0"
            offsets[value] = len(pool)
            pool.extend(encoded)
        return offsets[value]

    table_rows, fields, entries = [], [], []
    for path in sorted(source.glob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8-sig"))
        rules = document.get("rule")
        data = document.get("data")
        if not isinstance(rules, list) or not isinstance(data, dict):
            raise ValueError(f"Unsupported MasterDB format: {path}")
        primary, local = [], []
        for rule in rules:
            parts = rule.split("|")
            if len(parts) < 2:
                continue
            for item in parts[:-1]:
                if item not in primary:
                    primary.append(item)
            if parts[-1] not in local:
                local.append(parts[-1])
        if not primary or not local:
            continue
        selected = []
        local_set = set(local)
        for key, value in data.items():
            if not isinstance(value, str):
                continue
            if len(key.split("|")) != len(primary) + 1:
                continue
            local_path = key.rsplit("|", 1)[-1]
            normalized_path = re.sub(r"\[\d+\]", "", local_path)
            if normalized_path not in local_set:
                continue
            selected.append((key, value))
        if not selected:
            continue
        selected.sort(key=lambda item: item[0].encode("utf-8"))
        primary_start = len(fields)
        fields.extend(intern(item) for item in primary)
        local_start = len(fields)
        fields.extend(intern(item) for item in local)
        entry_start = len(entries)
        entries.extend((intern(key), intern(value)) for key, value in selected)
        table_rows.append((intern(path.stem), primary_start, len(primary),
                           local_start, len(local), entry_start, len(selected)))
    tables_offset = HEADER.size
    fields_offset = tables_offset + len(table_rows) * TABLE.size
    entries_offset = fields_offset + len(fields) * 4
    pool_offset = entries_offset + len(entries) * ENTRY.size
    size = pool_offset + len(pool)
    header = HEADER.pack(MAGIC, 1, len(table_rows), len(fields), len(entries), pool_offset, size)
    payload = (header + b"".join(TABLE.pack(*row) for row in table_rows) +
               struct.pack("<" + "I" * len(fields), *fields) +
               b"".join(ENTRY.pack(*row) for row in entries) + pool)
    if len(payload) != size:
        raise AssertionError("MasterDB binary size mismatch")
    return payload, {"tables": len(table_rows), "fields": len(fields),
                     "entries": len(entries), "bytes": len(payload),
                     "scope": "all flat-rule string fields"}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    blob, details = compile_master(args.root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(json.dumps(details, ensure_ascii=False, indent=2))
