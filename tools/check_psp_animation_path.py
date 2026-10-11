#!/usr/bin/env python3
"""Sanitize the production animation-bundle loading block, including with LTO.

An optional animMgr.cpp path lets the same check reproduce the old overwrite.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src/plugPikiColin/animMgr.cpp"
code = source.read_text()
start = code.index("if (missingAnimCount != 0)")
end = code.index("\n\t\tFOREACH_NODE_REUSE", start)
with tempfile.TemporaryDirectory(prefix="pikmin-animation-path-") as directory:
    (Path(directory) / "bundle_load.inc").write_text(code[start:end])
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    for lto in (False, True):
        binary = str(Path(directory) / ("check-lto" if lto else "check"))
        subprocess.run([
            os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g",
            *(["-flto=auto"] if lto else []),
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
            "-I", str(root / "include"), "-I", directory,
            str(root / "tests/psp_animation_path.cpp"), "-o", binary,
        ], check=True)
        print("LTO enabled" if lto else "LTO disabled", flush=True)
        subprocess.run([binary], check=True, env=environment, timeout=10)
