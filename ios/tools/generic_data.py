"""Compile Android ``generic.json`` data into a small native lookup index.

The Android loader keeps three maps: exact strings, format strings containing
``{0}`` style placeholders, and split fragments from ``generic.split.json``.
It also keeps every loaded Korean value in ``translatedText`` so an already
translated string is not translated a second time.  This module preserves all
four collections so the SDK-free iOS dylib follows the same lookup order
without shipping a JSON parser or C++ runtime.
"""
import json
from pathlib import Path
import re
import struct

MAGIC = b"HSGEN2\0"
HEADER = struct.Struct("<8s7I")
ENTRY = struct.Struct("<4I")
SPLIT_PREFIX = "[__split__]"
FORMAT_KEY = re.compile(r"\{\d+\}")
TAG_CONTENT = re.compile(r"<.*?>(.*?)</.*?>")


def _flatten(value):
    result = []
    if not isinstance(value, dict):
        return result
    for key, item in value.items():
        if isinstance(item, dict):
            result.extend(_flatten(item))
        elif isinstance(item, str):
            result.append((key, item))
    return result


def _is_pure(value):
    # Same character class as Local.cpp's StringParser split helper.  It is
    # used only for deriving mappings inside rich-text tags.
    return all(c in "0123456789:/.%,-+ x" or c == "\n" for c in value)


def _tag_parts(value):
    parts = []
    last_suffix = ""
    text = value
    while True:
        match = TAG_CONTENT.search(text)
        if not match:
            break
        contents = match.group(1)
        if _is_pure(contents):
            parts.append(text[:match.start()])
            last_suffix = text[match.end():]
        text = text[match.end():]
    if last_suffix:
        parts.append(last_suffix)
    return parts


def _load_file(path, split_file, exact, fmt, split_map, translated):
    document = json.loads(path.read_text(encoding="utf-8-sig"))
    for key, value in _flatten(document):
        if split_file and key.startswith(SPLIT_PREFIX) and value.startswith(SPLIT_PREFIX):
            split_value = value[len(SPLIT_PREFIX):]
            split_map[key[len(SPLIT_PREFIX):]] = split_value
            translated.add(split_value)
        elif FORMAT_KEY.search(key):
            fmt[key] = value
            translated.add(value)
        else:
            exact[key] = value
            translated.add(value)


def _collect_strings(value, translated):
    if isinstance(value, str):
        translated.add(value)
    elif isinstance(value, dict):
        for item in value.values():
            _collect_strings(item, translated)
    elif isinstance(value, list):
        for item in value:
            _collect_strings(item, translated)


def compile_generic(root: Path):
    local_files = root / "local-files"
    source = local_files / "genericTrans"
    if not source.is_dir():
        raise ValueError(f"genericTrans directory was not found: {source}")
    exact, fmt, split, translated = {}, {}, {}, set()
    # Local::LoadData loads local-files/generic.split.json and generic.json
    # first, followed by every JSON file under genericTrans.
    files = []
    for name in ("generic.split.json", "generic.json"):
        path = local_files / name
        if path.is_file():
            files.append(path)
    files.extend(sorted((p for p in source.rglob("*.json") if p.is_file()),
                        key=lambda p: p.relative_to(source).as_posix()))
    for path in files:
        _load_file(path, path.name.endswith(".split.json"), exact, fmt, split, translated)

    # Local::translatedText also receives localization.json and every string
    # value loaded by MasterLocal.  Keep that guard identical on iOS.
    localization = local_files / "localization.json"
    if localization.is_file():
        _collect_strings(json.loads(localization.read_text(encoding="utf-8-sig")), translated)
    master_root = local_files / "masterTrans"
    if master_root.is_dir():
        for path in sorted(master_root.glob("*.json")):
            document = json.loads(path.read_text(encoding="utf-8-sig"))
            _collect_strings(document.get("data", {}), translated)

    # Local::ProcessGenericTextLabels adds the text surrounding numeric rich
    # text tags as exact fallbacks.  Add the same derived pairs at compile time.
    derived = {}
    for key, value in list(exact.items()):
        original_parts = _tag_parts(key)
        if not original_parts:
            continue
        translated_parts = _tag_parts(value)
        if len(original_parts) != len(translated_parts):
            continue
        for original, translated_value in zip(original_parts, translated_parts):
            derived.setdefault(original, translated_value)
    for key, value in derived.items():
        exact.setdefault(key, value)

    def ordered(mapping):
        return sorted(mapping.items(), key=lambda item: item[0].encode("utf-8"))

    exact_items, fmt_items, split_items = ordered(exact), ordered(fmt), ordered(split)
    translated_items = sorted(translated, key=lambda value: value.encode("utf-8"))
    pool = bytearray(b"\0")
    offsets = {"": 0}

    def intern(value):
        if value not in offsets:
            offsets[value] = len(pool)
            pool.extend(value.encode("utf-8") + b"\0")
        return offsets[value]

    rows = []
    for group in (exact_items, fmt_items, split_items):
        for key, value in group:
            key_bytes = key.encode("utf-8")
            value_bytes = value.encode("utf-8")
            if len(key_bytes) > 0xFFFFFFFF or len(value_bytes) > 0xFFFFFFFF:
                raise ValueError("generic string is too large")
            rows.append((intern(key), intern(value), len(key_bytes), len(value_bytes)))
    for value in translated_items:
        value_bytes = value.encode("utf-8")
        rows.append((intern(value), 0, len(value_bytes), 0))
    exact_count, fmt_count, split_count = len(exact_items), len(fmt_items), len(split_items)
    translated_count = len(translated_items)
    entries_offset = HEADER.size
    pool_offset = entries_offset + len(rows) * ENTRY.size
    size = pool_offset + len(pool)
    header = HEADER.pack(MAGIC, 2, exact_count, fmt_count, split_count,
                         translated_count, pool_offset, size)
    payload = header + b"".join(ENTRY.pack(*row) for row in rows) + pool
    if len(payload) != size:
        raise AssertionError("generic binary size mismatch")
    return payload, {
        "exact": exact_count,
        "format": fmt_count,
        "split": split_count,
        "translated": translated_count,
        "derived": len(derived),
        "bytes": len(payload),
        "scope": "Android genericText, genericFmtText, genericSplitText, translatedText",
    }


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    blob, details = compile_generic(args.root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(json.dumps(details, ensure_ascii=False, indent=2))
