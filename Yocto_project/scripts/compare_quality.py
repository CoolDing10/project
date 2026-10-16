#!/usr/bin/env python3
"""Compare fixed-rate and MPC recordings against the same reference video."""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict


def run_filter(reference: Path, distorted: Path, metric: str) -> str:
    filter_graph = (
        "[0:v]setpts=PTS-STARTPTS[reference];"
        "[1:v]setpts=PTS-STARTPTS[distorted];"
        f"[reference][distorted]{metric}"
    )
    command = [
        "ffmpeg", "-hide_banner", "-nostdin", "-i", str(reference),
        "-i", str(distorted), "-lavfi", filter_graph, "-f", "null", "-",
    ]
    result = subprocess.run(command, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip().splitlines()[-1])
    return result.stderr


def measure(reference: Path, distorted: Path) -> Dict[str, float]:
    ssim_log = run_filter(reference, distorted, "ssim")
    psnr_log = run_filter(reference, distorted, "psnr")
    ssim_match = re.findall(r"All:([0-9.]+)", ssim_log)
    psnr_match = re.findall(r"average:([0-9.]+)", psnr_log)
    if not ssim_match or not psnr_match:
        raise RuntimeError("FFmpeg did not report PSNR/SSIM; check video dimensions and duration")
    return {"ssim": float(ssim_match[-1]), "psnr": float(psnr_match[-1])}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare fixed and MPC output quality against one reference")
    parser.add_argument("reference", type=Path)
    parser.add_argument("fixed", type=Path)
    parser.add_argument("mpc", type=Path)
    parser.add_argument("--required-improvement", type=float, default=35.0,
                        help="required SSIM error reduction percentage (default: 35)")
    args = parser.parse_args()
    if shutil.which("ffmpeg") is None:
        print("ffmpeg executable not found", file=sys.stderr)
        return 2
    try:
        fixed = measure(args.reference, args.fixed)
        mpc = measure(args.reference, args.mpc)
    except (OSError, RuntimeError) as error:
        print(f"quality comparison failed: {error}", file=sys.stderr)
        return 1
    fixed_error = max(1e-12, 1.0 - fixed["ssim"])
    mpc_error = max(0.0, 1.0 - mpc["ssim"])
    improvement = (fixed_error - mpc_error) / fixed_error * 100.0
    print("mode | PSNR(dB) | SSIM")
    print("--- | ---: | ---:")
    print(f"fixed | {fixed['psnr']:.3f} | {fixed['ssim']:.6f}")
    print(f"mpc | {mpc['psnr']:.3f} | {mpc['ssim']:.6f}")
    print(f"SSIM error reduction: {improvement:.2f}%")
    if improvement < args.required_improvement:
        print(f"FAIL: required at least {args.required_improvement:.2f}%", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

