#!/usr/bin/env python3
"""Check the PSP renderer; pass .mod/.bti assets or --texture-model map.mod."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = subprocess.check_output(["psp-config", "--pspsdk-path"], text=True).strip()
lto_flags = ["-flto=auto"] if os.environ.get("PIKMIN_PSP_TEST_LTO") == "1" else []
with tempfile.TemporaryDirectory(prefix="pikmin-gx-") as directory:
    binary = str(Path(directory) / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-w", "-fpermissive",
        *lto_flags,
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-rtti",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DPLATFORM_PSP=1", "-DVERSION_GPIE01_01", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), "-I", str(Path(sdk) / "include"),
        str(root / "tests/psp_gx_mesh.cpp"),
        str(root / "src/plugPikiYamashita/PUTRect.cpp"),
        str(root / "src/plugPikiYamashita/PSUList.cpp"), "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([binary, *sys.argv[1:]], check=True, env=environment)
