"""Customize the PlatformIO upload step to push the packaged DFU file."""
from __future__ import annotations

from pathlib import Path
import sys

Import("env")  # type: ignore  # Provided by PlatformIO at runtime

project_dir = Path(env["PROJECT_DIR"])  # type: ignore[name-defined]
dfu_latest = project_dir / "artifacts" / "dfu" / "platformio-latest.dfu"
firmware_bin = Path(env.subst("$BUILD_DIR")) / f'{env.subst("${PROGNAME}")}.bin'  # type: ignore[name-defined]
safe_uploader = project_dir / "scripts" / "safe_dfu_upload.py"
wrapper = project_dir / "tools" / "bin_to_dfuse.py"
recovery_backup = project_dir / "backup" / "dumped_firmware.bin"

# PlatformIO exposes the dfu-util binary path via $DFUUTIL when using the
# built-in uploader. Fallback to "dfu-util" if the package is missing.
dfu_util = env.subst("$DFUUTIL")  # type: ignore[name-defined]
if not dfu_util or dfu_util == "$DFUUTIL":
    dfu_util = "dfu-util"

cmd = (
    f'"{sys.executable}" "{safe_uploader}" '
    f'--dfu-util "{dfu_util}" --bin "{firmware_bin}" '
    f'--dfu "{dfu_latest}" --backup "{recovery_backup}" '
    f'--wrapper "{wrapper}"'
)
env.Replace(UPLOADCMD=cmd)  # type: ignore[name-defined]
