"""Replace the serialized Unity Font payload used by SourceSansPro-Regular.

This is the same operation as the Android GitHub workflow's patch_font.py:
the TMP FontAsset remains in the game and its referenced Font object receives
the supplied OTF bytes.  The source file is never modified in place.
"""
import argparse
from pathlib import Path
import struct

import UnityPy


def _font_bytes(font_path: Path) -> bytes:
    if not font_path.is_file():
        raise ValueError(f"Font file not found: {font_path}")
    new_font_data = font_path.read_bytes()
    if len(new_font_data) < 1024 or new_font_data[:4] not in (b"OTTO", b"\x00\x01\x00\x00"):
        raise ValueError("Font input is not a recognized TrueType/OpenType file")
    return new_font_data


def patch_bytes(asset_bytes: bytes, new_font_data: bytes,
                target_name: str = "SourceSansPro-Regular") -> tuple[bytes, dict]:
    env = UnityPy.load(asset_bytes)
    matches = []
    for obj in env.objects:
        if obj.type.name != "Font":
            continue
        data = obj.read()
        if getattr(data, "m_Name", None) == target_name:
            matches.append((obj, data))
    if len(matches) != 1:
        raise ValueError(f"Expected one Font named {target_name!r}, found {len(matches)}")

    obj, data = matches[0]
    old_bytes = bytes(data.m_FontData)
    raw = bytes(obj.get_raw_data())
    data_offset = raw.find(old_bytes)
    if data_offset < 4 or data_offset + len(old_bytes) > len(raw):
        raise ValueError("Could not locate Font.m_FontData in serialized object")
    # UnityPy's Font serializer intentionally reads only the common prefix for
    # some Unity versions. Replace the length-prefixed payload directly so any
    # trailing version-specific fields remain byte-for-byte intact.
    size_offset = data_offset - 4
    if struct.unpack_from("<I", raw, size_offset)[0] != len(old_bytes):
        raise ValueError("Font data length prefix does not match parsed payload")
    patched_raw = (raw[:size_offset] + struct.pack("<I", len(new_font_data)) +
                   new_font_data + raw[data_offset + len(old_bytes):])
    obj.set_raw_data(patched_raw)
    old_size = len(old_bytes)

    saved = None
    for name, file in env.files.items():
        # UnityPy uses the supplied path for file-backed input and an opaque
        # identifier for bytes-backed input. Both represent our single asset.
        if "sharedassets0.assets" in str(name) or len(env.files) == 1:
            saved = file.save()
            break
    if saved is None:
        raise ValueError("UnityPy did not expose sharedassets0.assets for saving")
    return saved, {"target": target_name, "old_font_bytes": old_size,
                   "new_font_bytes": len(new_font_data)}


def patch(asset_path: Path, font_path: Path, output_path: Path,
          target_name: str = "SourceSansPro-Regular") -> dict:
    if not asset_path.is_file():
        raise ValueError(f"Unity asset file not found: {asset_path}")
    new_font_data = _font_bytes(font_path)
    saved, details = patch_bytes(asset_path.read_bytes(), new_font_data, target_name)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(saved)
    details["output"] = str(output_path)
    return details


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset", type=Path)
    parser.add_argument("font", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--target", default="SourceSansPro-Regular")
    args = parser.parse_args()
    output = args.output or args.asset.with_name(args.asset.name + ".fontpatched")
    print(patch(args.asset, args.font, output))
