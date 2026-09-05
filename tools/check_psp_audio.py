#!/usr/bin/env python3
"""Run native decoder/metadata checks; optionally pass an original pikibank.bx."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="pikmin-audio-") as directory:
    binary = str(Path(directory) / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1", "-I", str(root / "include"),
        str(root / "tests/psp_audio_decode.cpp"), str(root / "src/port/psp/PspAudioAssets.cpp"),
        "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    # LeakSanitizer cannot attach to its child process in some containers.
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([binary, *sys.argv[1:]], check=True, env=environment)
