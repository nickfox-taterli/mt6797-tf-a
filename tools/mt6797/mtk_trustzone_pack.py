#!/usr/bin/env python3
"""Pack an MT6797 BL31 binary into the open-board TEE image format.

The X20 image is not a raw BL31 image.  It has an LK image header followed by
the MTK TEE KTM header and a fixed-size BL31 slot.  The inner header contains
an authentication area which this tool deliberately preserves from the
template; it does not have the signing key and must not be used for secure
devices.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


IMAGE_SIZE = 0x14000
OUTER_HEADER_SIZE = 0x200
INNER_OFFSET = OUTER_HEADER_SIZE
INNER_HEADER_SIZE = 0x240
CODE_OFFSET = INNER_OFFSET + INNER_HEADER_SIZE
CODE_SIZE = IMAGE_SIZE - CODE_OFFSET

OUTER_MAGIC = 0x58881688
OUTER_EXT_MAGIC = 0x58891689
# The MTK header stores this marker as the byte string "EET KTM @".
INNER_MAGIC = b"EET KTM @"


class ImageFormatError(ValueError):
    """The template is not the MT6797 image format expected by this tool."""


def u32(image: bytes, offset: int) -> int:
    return struct.unpack_from("<I", image, offset)[0]


def validate_template(template: bytes) -> None:
    if len(template) != IMAGE_SIZE:
        raise ImageFormatError(
            f"template size is 0x{len(template):x}, expected 0x{IMAGE_SIZE:x}"
        )
    if u32(template, 0x00) != OUTER_MAGIC:
        raise ImageFormatError("unexpected LK image magic")
    if u32(template, 0x04) != IMAGE_SIZE - OUTER_HEADER_SIZE:
        raise ImageFormatError("unexpected LK image data size")
    if u32(template, 0x30) != OUTER_EXT_MAGIC:
        raise ImageFormatError("unexpected LK extended-header magic")
    if u32(template, 0x34) != OUTER_HEADER_SIZE:
        raise ImageFormatError("unexpected LK extended-header size")
    if template[INNER_OFFSET : INNER_OFFSET + len(INNER_MAGIC)] != INNER_MAGIC:
        raise ImageFormatError("unexpected MTK TEE KTM magic")
    if u32(template, INNER_OFFSET + 0x08) != INNER_HEADER_SIZE:
        raise ImageFormatError("unexpected TEE KTM header size")
    payload_size = u32(template, INNER_OFFSET + 0x14)
    if payload_size != CODE_SIZE:
        raise ImageFormatError(
            f"unexpected TEE KTM payload size 0x{payload_size:x}"
        )
    if u32(template, INNER_OFFSET + 0x18) != payload_size:
        raise ImageFormatError("TEE KTM payload size fields differ")


def pack(template_path: Path, bl31_path: Path, output_path: Path) -> None:
    template = template_path.read_bytes()
    bl31 = bl31_path.read_bytes()
    validate_template(template)
    if not bl31:
        raise ImageFormatError("BL31 input is empty")
    if len(bl31) > CODE_SIZE:
        raise ImageFormatError(
            f"BL31 is 0x{len(bl31):x} bytes, slot is only 0x{CODE_SIZE:x} bytes"
        )

    image = bytearray(template)
    image[0x08 : 0x08 + 32] = b"\0" * 32
    image[0x08 : 0x0c] = b"atf\0"
    image[CODE_OFFSET : CODE_OFFSET + CODE_SIZE] = b"\0" * CODE_SIZE
    image[CODE_OFFSET : CODE_OFFSET + len(bl31)] = bl31
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--bl31", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--allow-preserved-signature",
        action="store_true",
        help=(
            "acknowledge that the template authentication area is preserved; "
            "this is for secure=no bring-up boards only"
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.allow_preserved_signature:
        print(
            "refusing to pack without --allow-preserved-signature; "
            "this tool cannot regenerate the inner TEE signature",
            file=sys.stderr,
        )
        return 2
    try:
        pack(args.template, args.bl31, args.output)
    except (OSError, ImageFormatError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(
        f"packed {args.bl31} into {args.output} "
        f"(BL31 0x{args.bl31.stat().st_size:x} bytes, "
        f"image 0x{IMAGE_SIZE:x} bytes)"
    )
    print(
        "warning: the template authentication area was preserved; "
        "use only on an unlocked secure=no bring-up device",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
