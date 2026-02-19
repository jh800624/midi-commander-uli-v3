"""Customize the PlatformIO upload step to push the packaged DFU file."""
from __future__ import annotations

from pathlib import Path

Import("env")  # type: ignore  # Provided by PlatformIO at runtime

project_dir = Path(env["PROJECT_DIR"])  # type: ignore[name-defined]
dfu_latest = project_dir / "artifacts" / "dfu" / "platformio-latest.dfu"

# PlatformIO exposes the dfu-util binary path via $DFUUTIL when using the
# built-in uploader. Fallback to "dfu-util" if the package is missing.
dfu_util = env.subst("$DFUUTIL")  # type: ignore[name-defined]
if not dfu_util or dfu_util == "$DFUUTIL":
    dfu_util = "dfu-util"

cmd = f'"{dfu_util}" --alt 0 --download "{dfu_latest}"'
env.Replace(UPLOADCMD=cmd)  # type: ignore[name-defined]
