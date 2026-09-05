#!/usr/bin/env python3
"""Check native H4M video/audio decoding against extracted movie files."""
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="pikmin-movies-") as directory:
    binary = str(Path(directory) / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g", "-fpermissive", "-w",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1", "-I", str(root / "include"),
        str(root / "tests/psp_movie_decode.cpp"), str(root / "src/port/psp/PspMovieDecode.cpp"),
        "-x", "c++", str(root / "src/hvqm4dec/hvqm4dec.c"), "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    subprocess.run([binary, *sys.argv[1:]], check=True, env=environment, timeout=600)
    if len(sys.argv) > 1:
        # Keep original compressed records; rewrite only container counts for
        # a two-frame fixture that exercises the actual playback API through EOF.
        with open(sys.argv[1], "rb") as source:
            header = bytearray(source.read(68))
            source.read(20)
            records = bytearray()
            videos = audios = 0
            while videos < 2:
                record = source.read(8)
                kind, flags, size = struct.unpack(">HHI", record)
                records += record + source.read(size)
                videos += kind == 1
                audios += kind == 0
        block = struct.pack(">5I", 0, len(records), videos, audios, 0x01000000)
        struct.pack_into(">4I", header, 20, len(block) + len(records), 1, videos, audios)
        fixture = Path(directory) / "two-frames.h4m"
        fixture.write_bytes(header + block + records)
        invalid = []
        damaged = bytearray(fixture.read_bytes())
        # Preserve container size but corrupt the first video's section offset.
        offset = 88
        while struct.unpack_from(">H", damaged, offset)[0] != 1:
            offset += 8 + struct.unpack_from(">I", damaged, offset + 4)[0]
        struct.pack_into(">I", damaged, offset + 8 + 4 + 8, 0xffffffff)
        for name, data in (("short-header", header[:32]),
                           ("short-record", fixture.read_bytes()[:-40]),
                           ("bad-video-offset", damaged)):
            path = Path(directory) / (name + ".h4m")
            path.write_bytes(data)
            invalid.append(str(path))
        playback = str(Path(directory) / "playback")
        sdk = subprocess.check_output(["psp-config", "--pspsdk-path"], text=True).strip()
        subprocess.run([
            os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g", "-fpermissive", "-w",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-DPLATFORM_PSP=1", "-DDTK_CONFIG_NONMATCHING=1",
            "-I", str(root / "include"), "-I", str(Path(sdk) / "include"),
            str(root / "tests/psp_movie_playback.cpp"), str(root / "src/port/psp/PspMovie.cpp"),
            str(root / "src/port/psp/PspMovieDecode.cpp"), "-x", "c++",
            str(root / "src/hvqm4dec/hvqm4dec.c"), "-o", playback,
        ], check=True)
        subprocess.run([playback, str(fixture), *invalid], check=True, env=environment, timeout=20)
