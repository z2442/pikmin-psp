#!/usr/bin/env python3
"""Sanitize the condition tree built by TaiPalmStrategy::start.

An optional source path allows checking the same regression against older code.
Only the construction block is extracted; game-world leaf queries are faked.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src/plugPikiNakata/taipalm.cpp"
text = source.read_text()
start = text.index("f32 rad = 2.0f * teki.getParameterF(TPF_CollisionRadius);")
start = text.index(";", start) + 1
end = text.index("Creature* neighbor =", start)
with tempfile.TemporaryDirectory(prefix="pikmin-conditions-") as directory:
    temporary = Path(directory)
    (temporary / "palm_conditions.inc").write_text(text[start:end])
    binary = str(temporary / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g", "-fpermissive",
        "-fsanitize=address,undefined", "-fsanitize-address-use-after-scope",
        "-fno-omit-frame-pointer", "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), "-I", directory,
        str(root / "tests/psp_condition_lifetime.cpp"), "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([binary], check=True, env=environment, timeout=10)
