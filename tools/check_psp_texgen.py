#!/usr/bin/env python3
"""Check production DGX texgen arithmetic with the PSP vector implementation.

Optionally supply an older dgxGraphics.cpp to verify the regression.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src/sysDolphin/dgxGraphics.cpp"
text = source.read_text()
start = text.index("if (mHasTexGen)", text.index("void DGXGraphics::useMatrixQuick"))
end = text.index("STACK_PAD_VAR(3)", start)
with tempfile.TemporaryDirectory(prefix="pikmin-texgen-") as directory:
    temporary = Path(directory)
    (temporary / "texgen_block.inc").write_text(text[start:end])
    binary = str(temporary / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g",
        "-fsanitize=undefined,float-divide-by-zero", "-fno-sanitize-recover=all",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), "-I", directory,
        str(root / "tests/psp_texgen.cpp"),
        str(root / "src/port/psp/PspDolphinMtx.cpp"), "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True, timeout=10)
