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
    sequence_binary = str(Path(directory) / "sequence")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-fpermissive", "-w",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1", "-I", str(root / "include"),
        str(root / "tests/psp_audio_sequence.cpp"),
        "-x", "c++", str(root / "src/jaudio/file_seq.c"),
        str(root / "src/jaudio/seqsetup.c"), str(root / "src/jaudio/playercall.c"),
        "-o", sequence_binary,
    ], check=True)
    # Bound the regression: an asynchronous stop under AudioLock spins forever.
    subprocess.run([sequence_binary], check=True, env=environment, timeout=10)
    demo_binary = str(Path(directory) / "demo")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-fpermissive", "-w",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-DPLATFORM_PSP=1", "-DVERSION_GPIE01_01", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), str(root / "tests/psp_audio_demo.cpp"),
        "-o", demo_binary,
    ], check=True)
    subprocess.run([demo_binary], check=True, env=environment, timeout=10)
