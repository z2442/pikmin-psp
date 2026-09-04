#include "Dolphin/dvd.h"

#include "port/psp/PspPlatform.h"

#include <pspiofilemgr.h>

#include <stdio.h>
#include <string.h>

namespace {

DVDDiskID sDiskId = { { 'G', 'P', 'I', 'E' }, { '0', '1' }, 0, 1, 0, 0, {} };
s32 sDriveStatus = DVD_STATE_END;

bool resolveDvdPath(char* output, u32 outputSize, const char* dvdPath)
{
	if (!dvdPath) return false;
	while (*dvdPath == '/') dvdPath++;
	char relative[384];
	int length = snprintf(relative, sizeof(relative), "data/%s", dvdPath);
	return length >= 0 && (u32)length < sizeof(relative) && PikminPsp::resolvePath(output, outputSize, relative);
}

SceUID fileDescriptor(const DVDFileInfo* fileInfo)
{
	return fileInfo ? (SceUID)(intptr_t)fileInfo->cBlock.userData : -1;
}

} // namespace

extern "C" {

OSThreadQueue __DVDThreadQueue = {};

void DVDInit(void) { sDriveStatus = DVD_STATE_END; }

BOOL DVDOpen(const char* filename, DVDFileInfo* fileInfo)
{
	if (!filename || !fileInfo) return FALSE;
	char path[512];
	if (!resolveDvdPath(path, sizeof(path), filename)) return FALSE;
	SceUID descriptor = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (descriptor < 0) {
		sDriveStatus = DVD_STATE_NO_DISK;
		return FALSE;
	}
	SceOff end = sceIoLseek(descriptor, 0, PSP_SEEK_END);
	sceIoLseek(descriptor, 0, PSP_SEEK_SET);
	memset(fileInfo, 0, sizeof(*fileInfo));
	fileInfo->length = end > 0 ? (u32)end : 0;
	fileInfo->cBlock.userData = (void*)(intptr_t)descriptor;
	sDriveStatus = DVD_STATE_END;
	return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* address, s32 length, s32 offset, s32)
{
	SceUID descriptor = fileDescriptor(fileInfo);
	if (descriptor < 0 || !address || length < 0 || offset < 0) return -1;
	if (sceIoLseek(descriptor, offset, PSP_SEEK_SET) < 0) return -1;
	sDriveStatus = DVD_STATE_BUSY;
	s32 read = sceIoRead(descriptor, address, length);
	if (read < 0) {
		sDriveStatus = DVD_STATE_FATAL_ERROR;
		return read;
	}
	if (read < length) memset(static_cast<u8*>(address) + read, 0, length - read);
	fileInfo->cBlock.transferredSize += length;
	sDriveStatus = DVD_STATE_END;
	return length;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* address, s32 length, s32 offset, DVDCallback callback, s32 priority)
{
	s32 result = DVDReadPrio(fileInfo, address, length, offset, priority);
	if (callback) callback(result, fileInfo);
	return result >= 0;
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
	SceUID descriptor = fileDescriptor(fileInfo);
	if (descriptor < 0) return FALSE;
	sceIoClose(descriptor);
	fileInfo->cBlock.userData = (void*)(intptr_t)-1;
	return TRUE;
}

s32 DVDGetDriveStatus(void) { return sDriveStatus; }
s32 DVDGetTransferredSize(DVDFileInfo* fileInfo) { return fileInfo ? (s32)fileInfo->cBlock.transferredSize : 0; }
s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block ? block->state : DVD_STATE_FATAL_ERROR; }
DVDDiskID* DVDGetCurrentDiskID(void) { return &sDiskId; }
BOOL DVDCheckDisk(void) { return PikminPsp::dataReady(); }
BOOL DVDSetAutoInvalidation(BOOL) { return TRUE; }
void DVDResume(void) { }
void DVDReset(void) { }

} // extern "C"
