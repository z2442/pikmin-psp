#!/usr/bin/env python3
"""Check collision impulse arithmetic; optionally supply an older kmath.cpp."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src/plugPikiKando/kmath.cpp"
text = source.read_text()
start = text.index("f32 calcImpulse(")
end = text.index("\n}", start) + 2
matrix_text = (root / "src/sysCommon/sysMath.cpp").read_text()
matrix_start = matrix_text.index("void Vector3f::multMatrix(")
matrix_end = matrix_text.index("\n}", matrix_start) + 2
with tempfile.TemporaryDirectory(prefix="pikmin-impulse-") as directory:
    temporary = Path(directory)
    (temporary / "impulse.inc").write_text(text[start:end])
    (temporary / "mult_matrix.inc").write_text(matrix_text[matrix_start:matrix_end])
    binary = str(temporary / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g",
        "-fsanitize=undefined,float-divide-by-zero", "-fno-sanitize-recover=all",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), "-I", directory,
        str(root / "tests/psp_impulse.cpp"), "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True, timeout=10)
