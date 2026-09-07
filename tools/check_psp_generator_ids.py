#!/usr/bin/env python3
"""Check generator/route IDs and lookup; optionally pass stages/practice/default.gen."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="pikmin-generator-") as directory:
    binary = str(Path(directory) / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-fpermissive", "-fno-rtti", "-w",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DPLATFORM_PSP=1", "-DVERSION_GPIE01_01", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), str(root / "tests/psp_generator_ids.cpp"),
        "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([binary, *sys.argv[1:]], check=True, env=environment, timeout=10)
