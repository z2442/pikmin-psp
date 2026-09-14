#!/usr/bin/env python3
"""Check PSP performance counter units and wraparound natively."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = subprocess.check_output(["psp-config", "--pspsdk-path"], text=True).strip()
with tempfile.TemporaryDirectory(prefix="pikmin-performance-") as directory:
    binary = str(Path(directory) / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g", "-fpermissive", "-w",
        "-fsanitize=undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), "-I", str(Path(sdk) / "include"),
        str(root / "tests/psp_performance.cpp"), "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True, timeout=10)
