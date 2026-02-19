"""PlatformIO post-build hook that repackages the generated binary as a DfuSe DFU.

This script is executed by PlatformIO only for the midi_dfu environment (see
platformio.ini). It shells out to tools/bin_to_dfuse.py so we reuse the same
implementation for manual and automated workflows.
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path
from typing import Any
import shutil

Import("env")  # type: ignore  # Provided by PlatformIO at runtime

PROJECT_DIR = Path(env["PROJECT_DIR"])  # type: ignore[name-defined]
BUILD_DIR = Path(env.subst("$BUILD_DIR"))  # type: ignore[name-defined]
PROGNAME = env.subst("${PROGNAME}")  # type: ignore[name-defined]
BIN_PATH = BUILD_DIR / f"{PROGNAME}.bin"
WRAPPER = PROJECT_DIR / "tools" / "bin_to_dfuse.py"
DFU_DIR = PROJECT_DIR / "artifacts" / "dfu"


def _timestamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def _generate_dfuse(target: Any, source: Any, env: Any) -> None:  # noqa: ANN401
    dfu_path = DFU_DIR / f"platformio-{_timestamp()}.dfu"
    DFU_DIR.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable,
        str(WRAPPER),
        "--bin",
        str(BIN_PATH),
        "--out",
        str(dfu_path),
        "--overwrite",
    ]
    print("[post_build_dfuse] Generating", dfu_path)
    subprocess.check_call(cmd)
    latest = DFU_DIR / "platformio-latest.dfu"
    shutil.copy2(dfu_path, latest)
    print("[post_build_dfuse] Updated", latest)


env.AddPostAction(str(BIN_PATH), _generate_dfuse)  # type: ignore[name-defined]
