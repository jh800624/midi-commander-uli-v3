#!/usr/bin/env python3
"""Package a raw STM32 firmware binary into an ST DfuSe-compatible DFU file.

This script mirrors the Windows-only DfuFileMgr flow so developers working
entirely on macOS/Linux can still generate DFU images for distribution.
"""
from __future__ import annotations

import argparse
import time
import zlib
from pathlib import Path

DFUSE_SIGNATURE = b"DfuSe"
TARGET_SIGNATURE = b"Target"
SUFFIX_SIGNATURE = b"UFD"
DEFAULT_NAME = "ST..."
DEFAULT_VENDOR = 0x0483  # STMicroelectronics
DEFAULT_PRODUCT = 0xDF11  # STM32 DFU
DEFAULT_DEVICE = 0x0000
DEFAULT_DFU = 0x011A
DEFAULT_ADDRESS = 0x08003000
DEFAULT_ALT = 0


def parse_int(value: str) -> int:
    """Parse integers that may be expressed in hex (0x123) or decimal."""
    return int(value, 0)


def build_target_blob(
    payload: bytes,
    load_address: int,
    alt_setting: int,
    target_name: str,
) -> bytes:
    """Construct the DFU target block that wraps the raw payload."""
    name_bytes = target_name.encode("ascii", "ignore")[:255]
    name_bytes = name_bytes.ljust(255, b"\x00")
    element = struct_pack("<II", load_address, len(payload)) + payload
    target_size = len(element)

    header = b"".join(
        [
            TARGET_SIGNATURE,
            bytes([alt_setting & 0xFF]),
            struct_pack("<I", 1 if target_name else 0),
            name_bytes,
            struct_pack("<I", target_size),
            struct_pack("<I", 1),  # number of elements
        ]
    )
    return header + element


def struct_pack(fmt: str, *values: int) -> bytes:
    """Helper with small wrapper for struct.pack."""
    import struct

    return struct.pack(fmt, *values)


def build_prefix(body_length: int, target_count: int) -> bytes:
    """Build the DFU prefix that precedes the target descriptors."""
    size = 11 + body_length  # prefix (11 bytes) + targets
    return b"".join(
        [
            DFUSE_SIGNATURE,
            b"\x01",  # version
            struct_pack("<I", size),
            bytes([target_count & 0xFF]),
        ]
    )


def build_suffix(
    bcd_device: int,
    product: int,
    vendor: int,
    bcd_dfu: int,
    body: bytes,
) -> bytes:
    """Append the DFU suffix (including CRC32)."""
    suffix_no_crc = b"".join(
        [
            struct_pack("<H", bcd_device & 0xFFFF),
            struct_pack("<H", product & 0xFFFF),
            struct_pack("<H", vendor & 0xFFFF),
            struct_pack("<H", bcd_dfu & 0xFFFF),
            SUFFIX_SIGNATURE,
            b"\x10",  # length of suffix including CRC
        ]
    )
    # DFU suffix CRC uses the standard polynomial with a final XOR.
    crc = (~zlib.crc32(body + suffix_no_crc) & 0xFFFFFFFF)
    return suffix_no_crc + struct_pack("<I", crc)


def generate_dfuse(
    payload: bytes,
    *,
    load_address: int,
    alt_setting: int,
    target_name: str,
    vendor: int,
    product: int,
    device: int,
    dfu_version: int,
) -> bytes:
    """Create the entire DFU image in memory."""
    target = build_target_blob(payload, load_address, alt_setting, target_name)
    prefix = build_prefix(len(target), target_count=1)
    body = prefix + target
    suffix = build_suffix(device, product, vendor, dfu_version, body)
    return body + suffix


def make_default_output_path() -> Path:
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    return Path("artifacts/dfu") / f"platformio-{timestamp}.dfu"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Wrap a raw firmware .bin into an ST DfuSe .dfu file",
    )
    parser.add_argument(
        "--bin",
        dest="bin_path",
        type=Path,
        default=Path(".pio/build/midi_dfu/firmware.bin"),
        help="Path to the PlatformIO-generated firmware.bin",
    )
    parser.add_argument(
        "--out",
        dest="output_path",
        type=Path,
        default=None,
        help="Output DFU path (defaults to artifacts/dfu/platformio-<timestamp>.dfu)",
    )
    parser.add_argument(
        "--address",
        dest="address",
        type=parse_int,
        default=DEFAULT_ADDRESS,
        help="Flash load address (default: 0x08003000)",
    )
    parser.add_argument(
        "--alt",
        dest="alt_setting",
        type=parse_int,
        default=DEFAULT_ALT,
        help="DFU alternate setting number (default: 0)",
    )
    parser.add_argument(
        "--vendor",
        dest="vendor",
        type=parse_int,
        default=DEFAULT_VENDOR,
        help="USB vendor ID (default: 0x0483)",
    )
    parser.add_argument(
        "--product",
        dest="product",
        type=parse_int,
        default=DEFAULT_PRODUCT,
        help="USB product ID (default: 0xDF11)",
    )
    parser.add_argument(
        "--device",
        dest="device",
        type=parse_int,
        default=DEFAULT_DEVICE,
        help="USB device version (default: 0x0000)",
    )
    parser.add_argument(
        "--dfu-version",
        dest="dfu_version",
        type=parse_int,
        default=DEFAULT_DFU,
        help="bcdDFU version stored in the suffix (default: 0x011A)",
    )
    parser.add_argument(
        "--name",
        dest="target_name",
        default=DEFAULT_NAME,
        help="Target name stored in the DFU descriptor (default: ST...)",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow overwriting the output file if it already exists",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    payload = args.bin_path.read_bytes()
    output_path = args.output_path or make_default_output_path()
    if output_path.exists() and not args.overwrite:
        raise FileExistsError(
            f"Refusing to overwrite existing file {output_path}. Use --overwrite to force."
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    dfu_bytes = generate_dfuse(
        payload,
        load_address=args.address,
        alt_setting=args.alt_setting,
        target_name=args.target_name,
        vendor=args.vendor,
        product=args.product,
        device=args.device,
        dfu_version=args.dfu_version,
    )
    output_path.write_bytes(dfu_bytes)
    print(f"Wrote {output_path} ({len(dfu_bytes)} bytes)")


if __name__ == "__main__":
    main()
