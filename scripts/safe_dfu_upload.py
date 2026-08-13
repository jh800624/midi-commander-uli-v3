#!/usr/bin/env python3
"""Validate the recovery backup and DFU image before invoking dfu-util."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

APP_START = 0x08003000
FLASH_GUARD_START = 0x0807F000
FLASH_BYTES = 512 * 1024
STM32_DFU_ID = "0483:df11"
EXPECTED_RECOVERY_SHA256 = "c296af705132ce3997c896552281c66ac522df7a158ee198a0d6a1283a9d755f"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dfu-util", required=True)
    parser.add_argument("--bin", dest="bin_path", type=Path, required=True)
    parser.add_argument("--dfu", dest="dfu_path", type=Path, required=True)
    parser.add_argument("--backup", type=Path, required=True)
    parser.add_argument("--wrapper", type=Path, required=True)
    return parser.parse_args()


def validate_vector(image: bytes, base: int) -> None:
    if len(image) < 8:
        raise RuntimeError("firmware image is too short to contain a vector table")
    stack, reset = struct.unpack_from("<II", image)
    if not 0x20000000 <= stack <= 0x20010000:
        raise RuntimeError(f"invalid initial stack pointer: 0x{stack:08X}")
    reset_address = reset & ~1
    if not base <= reset_address < base + len(image) or (reset & 1) == 0:
        raise RuntimeError(f"invalid reset vector: 0x{reset:08X}")


def validate_backup(path: Path) -> None:
    backup = path.read_bytes()
    if len(backup) != FLASH_BYTES:
        raise RuntimeError(f"recovery backup must be {FLASH_BYTES} bytes: {path}")
    validate_vector(backup, 0x08000000)
    digest = hashlib.sha256(backup).hexdigest()
    if digest != EXPECTED_RECOVERY_SHA256:
        raise RuntimeError(f"recovery backup SHA-256 mismatch: {path}")


def validate_dfuse(path: Path, expected_payload: bytes) -> None:
    data = path.read_bytes()
    if len(data) < 298 or data[:5] != b"DfuSe" or data[11:17] != b"Target":
        raise RuntimeError("generated file is not a supported single-target DfuSe image")
    target_count = data[10]
    element_count = struct.unpack_from("<I", data, 11 + 270)[0]
    address, size = struct.unpack_from("<II", data, 11 + 274)
    payload = data[11 + 282 : 11 + 282 + size]
    stored_crc = struct.unpack_from("<I", data, len(data) - 4)[0]
    calculated_crc = (~zlib.crc32(data[:-4])) & 0xFFFFFFFF
    if target_count != 1 or element_count != 1:
        raise RuntimeError("DFU must contain exactly one target and one element")
    if address != APP_START or size != len(expected_payload) or payload != expected_payload:
        raise RuntimeError("DFU payload/address does not match the current DFU build")
    if address + size > FLASH_GUARD_START:
        raise RuntimeError("DFU payload overlaps the internal Flash guard region")
    if stored_crc != calculated_crc:
        raise RuntimeError("DFU suffix CRC is invalid")


def find_one_target(dfu_util: str) -> None:
    result = subprocess.run(
        [dfu_util, "--list"], check=True, text=True, capture_output=True
    )
    output = result.stdout + result.stderr
    matches = [
        line
        for line in output.splitlines()
        if re.search(r"\[0483:df11\]", line, re.IGNORECASE)
        and re.search(r"alt=0\b", line)
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one STM32 DFU alt=0 target, found {len(matches)}"
        )


def main() -> None:
    args = parse_args()
    validate_backup(args.backup)
    payload = args.bin_path.read_bytes()
    validate_vector(payload, APP_START)
    if len(payload) > FLASH_GUARD_START - APP_START:
        raise RuntimeError("firmware binary overlaps the internal Flash guard region")

    subprocess.run(
        [
            sys.executable,
            str(args.wrapper),
            "--bin",
            str(args.bin_path),
            "--out",
            str(args.dfu_path),
            "--address",
            hex(APP_START),
            "--overwrite",
        ],
        check=True,
    )
    validate_dfuse(args.dfu_path, payload)
    find_one_target(args.dfu_util)

    print(f"Validated payload SHA-256: {hashlib.sha256(payload).hexdigest()}")
    print("Bootloader range 0x08000000-0x08002FFF is not in the DFU payload")
    subprocess.run(
        [
            args.dfu_util,
            "--device",
            STM32_DFU_ID,
            "--alt",
            "0",
            "--download",
            str(args.dfu_path),
        ],
        check=True,
    )


if __name__ == "__main__":
    main()
