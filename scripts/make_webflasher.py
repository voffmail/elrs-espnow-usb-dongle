#!/usr/bin/env python3
"""Assemble the GitHub Pages web flasher from the merged ESP32-C3 image."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--source", type=Path, default=Path("webflasher"))
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    source = (root / args.source) if not args.source.is_absolute() else args.source
    binary = args.binary if args.binary.is_absolute() else (root / args.binary)
    out = args.out if args.out.is_absolute() else (root / args.out)

    index = source / "index.html"
    if not index.is_file():
        raise SystemExit(f"missing page template: {index}")
    if not binary.is_file():
        raise SystemExit(f"missing firmware image: {binary}")

    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(index, out / "index.html")
    shutil.copyfile(binary, out / "firmware-merged.bin")
    (out / ".nojekyll").write_text("", encoding="utf-8")

    manifest = {
        "name": "ELRS ESP-NOW USB Dongle",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-C3",
                "serialType": "cdc",
                "parts": [
                    {"path": "firmware-merged.bin", "offset": 0},
                ],
            }
        ],
    }
    (out / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )

    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    (out / "SHA256SUMS").write_text(
        f"{digest}  firmware-merged.bin\n", encoding="utf-8"
    )
    print(f"web flasher: {out} (version {args.version}, sha256 {digest})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
