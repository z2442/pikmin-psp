#!/usr/bin/env python3
"""Check PSP card storage and the production card utility save/wait paths."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = subprocess.check_output(["psp-config", "--pspsdk-path"], text=True).strip()


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


cardutil = (root / "src/plugPikiColin/cardutil.cpp").read_text()
memory = (root / "src/plugPikiColin/memoryCard.cpp").read_text()
stream = (root / "src/sysCommon/stream.cpp").read_text()
prefs = (root / "src/plugPikiColin/gamePrefs.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="pikmin-card-") as directory:
    path = Path(directory)
    signatures = ["void CardUtilLockDirectory()", "void CardUtilUnlockDirectory()",
                  "s32* CardUtilByteNotUsed()", "s32* CardUtilFilesNotUsed()", "u32* CardUtilSectorSize()",
                  "static s32 DoMount(", "static s32 DoUnmount(", "static s32 DoFormat(", "static s32 DoErase(", "static s32 DoList(",
                  "static s32 DoOpen(", "static s32 DoWrite(", "static s32 DoSave(",
                  "s32 CardUtilResultCode()", "bool CardUtilIsCardBusy()", "void CardUtilIdleWhileBusy()", "static s32 CardUtilCommand("]
    (path / "cardutil.inc").write_text("\n".join(function(cardutil, sig) for sig in signatures))
    (path / "memory_wait.inc").write_text(function(memory, "bool MemoryCard::hasCardFinished()")
                                         + "\n" + function(memory, "void MemoryCard::waitPolling()")
                                         + "\n" + function(memory, "u32 MemoryCard::calcChecksum(")
                                         + "\n" + "\n".join(function(memory, sig) for sig in (
                                             "s32 MemoryCard::waitWhileBusy(", "bool MemoryCard::getCardStatus(",
                                             "void MemoryCard::checkUseFile()", "s32 MemoryCard::getMemoryCardState(",
                                             "void MemoryCard::loadCurrentFile()")))
    (path / "card_assets.inc").write_text(memory[memory.index("static immut char p_icon_palette"):
                                                memory.index("\n/**", memory.index("static immut char* icon_pals"))])
    (path / "default_file.inc").write_text("\n".join(function(memory, sig) for sig in (
        "int MemoryCard::getOptionsOffset(", "int MemoryCard::getGameFileOffset(", "void* MemoryCard::getBannerPtr()",
        "void* MemoryCard::getOptionsPtr(", "void* MemoryCard::getGameFilePtr(", "RamStream* MemoryCard::getBannerStream()",
        "RamStream* MemoryCard::getOptionsStream(", "RamStream* MemoryCard::getGameFileStream(",
        "void MemoryCard::createFile(", "void MemoryCard::initBannerArea(", "void MemoryCard::initOptionsArea(",
        "void MemoryCard::initFileArea(", "s32 MemoryCard::makeDefaultFile()", "u32 MemoryCard::getOkSections()",
        "bool MemoryCard::isFileBroken()", "bool MemoryCard::didSaveFail()")))
    (path / "play_state.inc").write_text(function(prefs, "void PlayState::write(") + "\n" + function(prefs, "void PlayState::read("))
    (path / "stream.inc").write_text("\n".join(function(stream, sig) for sig in (
        "int Stream::readInt()", "u8 Stream::readByte()", "short Stream::readShort()", "f32 Stream::readFloat()",
        "void Stream::writeInt(", "void Stream::writeByte(", "void Stream::writeShort(", "void Stream::writeFloat(")))
    binary = str(path / "check")
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g", "-w", "-fpermissive",
        "-flto=auto", "-fno-strict-aliasing", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
        "-DPLATFORM_PSP=1", "-DVERSION_GPIE01_01", "-DDTK_CONFIG_NONMATCHING=1",
        "-I", str(root / "include"), "-I", str(Path(sdk) / "include"), "-I", directory,
        str(root / "tests/psp_card.cpp"), "-o", binary,
    ], check=True)
    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    storage = str(path / "storage")
    Path(storage).mkdir()
    subprocess.run([binary, storage], check=True, env=environment, timeout=60)
    subprocess.run([binary, storage, "restart"], check=True, env=environment, timeout=20)
