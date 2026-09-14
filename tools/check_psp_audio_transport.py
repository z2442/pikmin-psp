#!/usr/bin/env python3
"""Check CPU/ME stream ownership and cache ranges with recorded cache calls."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = subprocess.check_output(["psp-config", "--pspsdk-path"], text=True).strip()
with tempfile.TemporaryDirectory(prefix="pikmin-audio-transport-") as directory:
    temporary = Path(directory)
    include = temporary / "me-core-mapper"
    include.mkdir()
    (include / "me-lib.h").write_text(
        '#pragma once\n#include "types.h"\n'
        'void meLibDcacheInvalidateRange(u32, u32);\n'
        'void meLibDcacheWritebackRange(u32, u32);\n'
    )
    binary = str(temporary / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-fpermissive", "-w",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", directory, "-I", str(root / "include"), "-I", str(Path(sdk) / "include"),
        str(root / "tests/psp_audio_transport.cpp"), "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([binary], check=True, env=environment, timeout=10)
