#include "port/psp/PspCard.h"
#include "port/psp/PspPlatform.h"

#include <pspiofilemgr.h>
#include <pspkerror.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace {
const u32 kSector = 8192;
const u32 kCapacity = 2 * 1024 * 1024 - CARD_NUM_SYSTEM_BLOCK * kSector;
const u32 kHeaderSize = 128;
const u8 kMagic[8] = {'P','K','P','S','C','A','R','D'};
struct Entry { CARDStat state; u32 checksum; bool present; };
struct Card { Entry entries[CARD_MAX_FILE]; s32 result, transferred; bool mounted, broken; };
Card sCards[2];
char sCardRoot[512];
char sCardDevice[16];
OSMutex sMutex;
struct Lock { Lock() { OSLockMutex(&sMutex); } ~Lock() { OSUnlockMutex(&sMutex); } };

bool validChannel(s32 channel) { return channel >= 0 && channel < 2; }
s32 result(s32 channel, s32 code)
{
	if (validChannel(channel)) sCards[channel].result = code;
	return code;
}
s32 ioResult(s32 code)
{
	if (code >= 0) return CARD_RESULT_READY;
	u32 error = (u32)code & 0xffff;
	if (code == -ENOENT || error == ENOENT) return CARD_RESULT_NOFILE;
	if (code == -ENOSPC || error == ENOSPC) return CARD_RESULT_INSSPACE;
	return CARD_RESULT_IOERROR;
}
bool cardPath(char* path, u32 size, s32 channel, s32 fileNo = -1, const char* suffix = "")
{
	if (!sCardRoot[0]) return false;
	int length;
	if (fileNo < 0) length = snprintf(path, size, "%s/card_%c", sCardRoot, (int)('a' + channel));
	else length = snprintf(path, size, "%s/card_%c/%03d.pcs%s", sCardRoot, (int)('a' + channel), (int)fileNo, suffix);
	return length >= 0 && (u32)length < size;
}
s32 directory(const char* path)
{
	SceIoStat stat = {};
	s32 code = sceIoGetstat(path, &stat);
	if (code >= 0) return FIO_S_ISDIR(stat.st_mode) ? CARD_RESULT_READY : CARD_RESULT_IOERROR;
	if (ioResult(code) != CARD_RESULT_NOFILE) return CARD_RESULT_IOERROR;
	return ioResult(sceIoMkdir(path, 0777));
}
s32 selectCardRoot()
{
	if (sCardRoot[0]) return CARD_RESULT_READY;
	const char* executable = PikminPsp::executableRoot();
	// Use the same writable location that successfully detected the virtual
	// card before PSPLink saves were redirected. Root-directory stat is not
	// required: only the actual save directories are checked/created.
	if (!PikminPsp::resolvePath(sCardRoot, sizeof(sCardRoot), "saves")) {
		sCardRoot[0] = 0;
		return CARD_RESULT_IOERROR;
	}
	const char* root = strchr(sCardRoot, ':') ? sCardRoot : executable;
	const char* colon = strchr(root, ':');
	if (colon) {
		size_t length = colon - root + 1;
		if (length >= sizeof(sCardDevice)) { sCardRoot[0] = 0; return CARD_RESULT_IOERROR; }
		memcpy(sCardDevice, root, length); sCardDevice[length] = 0;
	}
	return CARD_RESULT_READY;
}
s32 ensureCard(s32 channel)
{
	if (!validChannel(channel)) return CARD_RESULT_FATAL_ERROR;
	s32 code = selectCardRoot();
	if (code < 0) return code;
	char path[512];
	code = directory(sCardRoot);
	if (code < 0) return code;
	if (!cardPath(path, sizeof(path), channel)) return CARD_RESULT_IOERROR;
	return directory(path);
}
s32 syncStorage()
{
	// Firmware and storage drivers expose different sync endpoints. Closing
	// and verifying the staged file is mandatory; driver sync may be unsupported.
	if (!sCardDevice[0]) return CARD_RESULT_READY;
	const char* endpoints[] = { sCardDevice, "msfat0:", "fatms0:" };
	u32 count = !strcmp(sCardDevice, "ms0:") ? ARRAY_SIZE(endpoints) : 1;
	for (u32 i = 0; i < count; ++i) {
		s32 code = sceIoSync(endpoints[i], 0);
		if (code >= 0) return CARD_RESULT_READY;
		// USB HostFS reports an unimplemented sync devctl as plain -1,
		// unlike the kernel/errno unsupported-operation codes below. This
		// is specific to host drivers; real media and other I/O errors fail.
		if (code == -1 && !strncmp(sCardDevice, "host", 4)) return CARD_RESULT_READY;
		// sceIoSync goes through the kernel device-control API. Its native
		// errors are not 0x80010000 | errno, so checking only the low bits
		// incorrectly rejects every save on an unsupported/restricted driver.
		switch ((u32)code) {
		case SCE_KERNEL_ERROR_NODEV:
		case SCE_KERNEL_ERROR_INVAL:
		case SCE_KERNEL_ERROR_UNSUP:
		case SCE_KERNEL_ERROR_ILLEGAL_PERM:
			continue;
		}
		u32 error = (u32)code & 0xffff;
		// Only unsupported device/operation errors permit another endpoint.
		// Media I/O errors and a full filesystem still fail the save.
		if (error != ENODEV && error != ENOENT && error != EINVAL && error != ENOSYS && error != ENOTSUP
		    && error != EPERM && error != EACCES)
			return ioResult(code);
	}
	return CARD_RESULT_READY;
}
s32 removeIfPresent(const char* path)
{
	s32 code = ioResult(sceIoRemove(path));
	return code == CARD_RESULT_NOFILE ? CARD_RESULT_READY : code;
}
s32 renameStorageFile(const char* from, const char* to)
{
	// The kernel resolves the source path, but forwards the destination to
	// USB HostFS verbatim. HostFS rebases a relative name onto the source's
	// directory; passing "host0:/..." creates an invalid host destination.
	if (!sCardDevice[0] || !strncmp(sCardDevice, "host", 4)) {
		const char* slash = strrchr(to, '/');
		if (slash) to = slash + 1;
	}
	return sceIoRename(from, to);
}
u32 crc(u32 value, const void* data, u32 bytes)
{
	static const u32 table[16] = {0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
	    0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c};
	const auto* input = static_cast<const u8*>(data);
	while (bytes--) {
		value ^= *input++;
		value = (value >> 4) ^ table[value & 15];
		value = (value >> 4) ^ table[value & 15];
	}
	return value;
}
void put32(u8* p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
u32 get32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
bool validName(const char* name)
{
	if (!name || !*name) return false;
	return strnlen(name, CARD_FILENAME_MAX + 1) <= CARD_FILENAME_MAX;
}
void iconOffsets(CARDStat& state)
{
	u32 offset = state.iconAddr;
	if (offset == ~0U) { state.bannerFormat = state.iconFormat = state.iconSpeed = 0; offset = 0; }
	state.offsetBanner = state.offsetBannerTlut = state.offsetIconTlut = ~0U;
	if (CARDGetBannerFormat(&state) == CARD_STAT_BANNER_C8) {
		state.offsetBanner = offset; offset += 3072;
		state.offsetBannerTlut = offset; offset += 512;
	} else if (CARDGetBannerFormat(&state) == CARD_STAT_BANNER_RGB5A3) {
		state.offsetBanner = offset; offset += 6144;
	}
	bool tlut = false;
	for (u32 i = 0; i < CARD_ICON_MAX; ++i) {
		state.offsetIcon[i] = ~0U;
		if (CARDGetIconFormat(&state, i) == CARD_STAT_ICON_C8) {
			state.offsetIcon[i] = offset; offset += 1024; tlut = true;
		} else if (CARDGetIconFormat(&state, i) == CARD_STAT_ICON_RGB5A3) {
			state.offsetIcon[i] = offset; offset += 2048;
		}
	}
	if (tlut) { state.offsetIconTlut = offset; offset += 512; }
	state.offsetData = offset;
}
bool validState(CARDStat& state)
{
	if (!state.length || state.length > kCapacity || state.length % kSector) return false;
	char name[CARD_FILENAME_MAX + 1]; memcpy(name, state.fileName, CARD_FILENAME_MAX); name[CARD_FILENAME_MAX] = 0;
	if (!validName(name)) return false;
	if (state.iconAddr != ~0U && state.iconAddr > state.length) return false;
	if (state.commentAddr != ~0U && (state.length < CARD_COMMENT_SIZE || state.commentAddr > state.length - CARD_COMMENT_SIZE)) return false;
	iconOffsets(state);
	return state.offsetData <= state.length;
}
void encode(u8 (&header)[kHeaderSize], const Entry& entry)
{
	memset(header, 0, sizeof(header)); memcpy(header, kMagic, sizeof(kMagic));
	put32(header + 8, 1); put32(header + 12, kHeaderSize);
	put32(header + 16, entry.state.length); put32(header + 20, entry.checksum);
	memcpy(header + 28, entry.state.fileName, CARD_FILENAME_MAX);
	put32(header + 60, entry.state.time);
	memcpy(header + 64, entry.state.gameName, 4); memcpy(header + 68, entry.state.company, 2);
	header[70] = entry.state.bannerFormat; put32(header + 72, entry.state.iconAddr);
	header[76] = entry.state.iconFormat >> 8; header[77] = entry.state.iconFormat;
	header[78] = entry.state.iconSpeed >> 8; header[79] = entry.state.iconSpeed;
	put32(header + 80, entry.state.commentAddr);
	put32(header + 24, crc(~0U, header, sizeof(header)) ^ ~0U);
}
bool decode(u8 (&header)[kHeaderSize], Entry& entry)
{
	u32 checksum = get32(header + 24); put32(header + 24, 0);
	if (memcmp(header, kMagic, sizeof(kMagic)) || get32(header + 8) != 1 || get32(header + 12) != kHeaderSize
	    || checksum != (crc(~0U, header, sizeof(header)) ^ ~0U)) return false;
	entry = {};
	entry.state.length = get32(header + 16); entry.checksum = get32(header + 20);
	memcpy(entry.state.fileName, header + 28, CARD_FILENAME_MAX); entry.state.time = get32(header + 60);
	memcpy(entry.state.gameName, header + 64, 4); memcpy(entry.state.company, header + 68, 2);
	entry.state.bannerFormat = header[70]; entry.state.iconAddr = get32(header + 72);
	entry.state.iconFormat = (u16)header[76] << 8 | header[77];
	entry.state.iconSpeed = (u16)header[78] << 8 | header[79]; entry.state.commentAddr = get32(header + 80);
	entry.present = validState(entry.state);
	return entry.present;
}
s32 transfer(SceUID fd, void* buffer, u32 bytes, bool write)
{
	u8* output = static_cast<u8*>(buffer);
	while (bytes) {
		s32 done = write ? sceIoWrite(fd, output, bytes) : sceIoRead(fd, output, bytes);
		if (done < 0) return ioResult(done);
		if (!done || (u32)done > bytes) return write ? CARD_RESULT_IOERROR : CARD_RESULT_BROKEN;
		output += done; bytes -= done;
	}
	return CARD_RESULT_READY;
}
s32 readEntry(const char* path, Entry& entry)
{
	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (fd < 0) return ioResult(fd);
	u8 header[kHeaderSize], buffer[1024];
	s32 code = transfer(fd, header, sizeof(header), false);
	if (code == CARD_RESULT_READY && !decode(header, entry)) code = CARD_RESULT_BROKEN;
	if (code == CARD_RESULT_READY) {
		SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
		if (size != (SceOff)(kHeaderSize + entry.state.length)) code = size < 0 ? CARD_RESULT_IOERROR : CARD_RESULT_BROKEN;
		if (sceIoLseek(fd, kHeaderSize, PSP_SEEK_SET) < 0) code = CARD_RESULT_IOERROR;
	}
	u32 checksum = ~0U;
	for (u32 remaining = code == CARD_RESULT_READY ? entry.state.length : 0; remaining && code == CARD_RESULT_READY;) {
		u32 bytes = MIN(remaining, (u32)sizeof(buffer));
		code = transfer(fd, buffer, bytes, false);
		if (code == CARD_RESULT_READY) checksum = crc(checksum, buffer, bytes);
		remaining -= bytes;
	}
	if (code == CARD_RESULT_READY && (checksum ^ ~0U) != entry.checksum) code = CARD_RESULT_BROKEN;
	if (sceIoClose(fd) < 0 && code == CARD_RESULT_READY) code = CARD_RESULT_IOERROR;
	return code;
}
s32 mounted(s32 channel)
{
	if (!validChannel(channel)) return CARD_RESULT_FATAL_ERROR;
	return sCards[channel].mounted ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}
s32 findName(s32 channel, const char* name)
{
	for (s32 i = 0; i < CARD_MAX_FILE; ++i)
		if (sCards[channel].entries[i].present && !strncmp(sCards[channel].entries[i].state.fileName, name, CARD_FILENAME_MAX)) return i;
	return -1;
}
u32 usedBytes(s32 channel)
{
	u32 used = 0;
	for (const auto& entry : sCards[channel].entries) if (entry.present) used += entry.state.length;
	return used;
}
s32 fileEntry(s32 channel, s32 fileNo)
{
	s32 code = mounted(channel);
	if (code < 0) return code;
	if (sCards[channel].broken) return CARD_RESULT_BROKEN;
	if (fileNo < 0 || fileNo >= CARD_MAX_FILE) return CARD_RESULT_FATAL_ERROR;
	return sCards[channel].entries[fileNo].present ? CARD_RESULT_READY : CARD_RESULT_NOFILE;
}
void fileInfo(CARDFileInfo& info, s32 channel, s32 fileNo)
{
	info = {}; info.chan = channel; info.fileNo = fileNo; info.length = sCards[channel].entries[fileNo].state.length;
}
s32 commit(s32 channel, s32 fileNo, Entry entry, const void* data, u32 offset, u32 length, bool preserve)
{
	char path[512], staged[512], backup[512];
	if (!cardPath(path, sizeof(path), channel, fileNo) || !cardPath(staged, sizeof(staged), channel, fileNo, ".new")
	    || !cardPath(backup, sizeof(backup), channel, fileNo, ".bak")) return CARD_RESULT_IOERROR;
	if (entry.state.length > kCapacity - usedBytes(channel) + (sCards[channel].entries[fileNo].present
	    ? sCards[channel].entries[fileNo].state.length : 0)) return CARD_RESULT_INSSPACE;
	SceUID input = preserve ? sceIoOpen(path, PSP_O_RDONLY, 0) : -1;
	if (preserve && input < 0) return ioResult(input);
	if (preserve && sceIoLseek(input, kHeaderSize, PSP_SEEK_SET) < 0) { sceIoClose(input); return CARD_RESULT_IOERROR; }
	SceUID output = sceIoOpen(staged, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
	if (output < 0) { if (input >= 0) sceIoClose(input); return ioResult(output); }
	u8 header[kHeaderSize] = {}, buffer[1024];
	s32 code = transfer(output, header, sizeof(header), true);
	u32 checksum = ~0U;
	for (u32 position = 0; position < entry.state.length && code == CARD_RESULT_READY;) {
		u32 bytes = MIN(entry.state.length - position, (u32)sizeof(buffer));
		if (preserve) code = transfer(input, buffer, bytes, false);
		else memset(buffer, 0, bytes);
		if (code < 0) break;
		if (data) {
			u32 start = MAX(position, offset), end = MIN(position + bytes, offset + length);
			if (start < end) memcpy(buffer + start - position, static_cast<const u8*>(data) + start - offset, end - start);
		}
		checksum = crc(checksum, buffer, bytes); code = transfer(output, buffer, bytes, true); position += bytes;
	}
	entry.checksum = checksum ^ ~0U;
	if (code == CARD_RESULT_READY) {
		encode(header, entry);
		if (sceIoLseek(output, 0, PSP_SEEK_SET) < 0) code = CARD_RESULT_IOERROR;
		else code = transfer(output, header, sizeof(header), true);
	}
	if (input >= 0 && sceIoClose(input) < 0 && code == CARD_RESULT_READY) code = CARD_RESULT_IOERROR;
	if (sceIoClose(output) < 0 && code == CARD_RESULT_READY) code = CARD_RESULT_IOERROR;
	if (code == CARD_RESULT_READY) code = syncStorage();
	if (code == CARD_RESULT_READY) {
		Entry verified = {};
		code = readEntry(staged, verified);
	}
	if (code < 0) { removeIfPresent(staged); return code; }
	if (sCards[channel].entries[fileNo].present) {
		code = removeIfPresent(backup);
		if (code == CARD_RESULT_READY) code = ioResult(renameStorageFile(path, backup));
		if (code < 0) return code;
		code = syncStorage();
		if (code < 0) { renameStorageFile(backup, path); return code; }
	}
	code = ioResult(renameStorageFile(staged, path));
	if (code < 0) {
		if (sCards[channel].entries[fileNo].present) renameStorageFile(backup, path);
		return code;
	}
	entry.present = true; sCards[channel].entries[fileNo] = entry;
	sCards[channel].transferred += length;
	return syncStorage();
}
s32 freeSlot(s32 channel)
{
	for (s32 i = 0; i < CARD_MAX_FILE; ++i) if (!sCards[channel].entries[i].present) return i;
	return -1;
}
s32 callbackResult(CARDCallback callback, s32 channel, s32 code)
{
	// Completion is immediate; game utility requests already run on its worker.
	// As on Dolphin, an operation rejected at submission has no callback.
	if (code >= 0 && callback) callback(channel, code);
	return code;
}
} // namespace

namespace PikminPsp {
s32 saveCardFile(s32 channel, CARDStat* state, const void* data, s32* fileNo)
{
	Lock lock;
	s32 code = mounted(channel);
	if (code < 0) return result(channel, code);
	if (sCards[channel].broken) return result(channel, CARD_RESULT_BROKEN);
	if (!state || !data || !fileNo) return result(channel, CARD_RESULT_FATAL_ERROR);
	Entry entry = {}; entry.state = *state;
	const DVDDiskID* disk = DVDGetCurrentDiskID();
	memcpy(entry.state.gameName, disk->gameName, 4); memcpy(entry.state.company, disk->company, 2);
	entry.state.time = (u32)OSTicksToSeconds(OSGetTime());
	if (!validState(entry.state)) return result(channel, CARD_RESULT_FATAL_ERROR);
	char name[CARD_FILENAME_MAX + 1]; memcpy(name, state->fileName, CARD_FILENAME_MAX); name[CARD_FILENAME_MAX] = 0;
	s32 slot = findName(channel, name);
	if (slot < 0) slot = freeSlot(channel);
	if (slot < 0) return result(channel, CARD_RESULT_NOENT);
	code = commit(channel, slot, entry, data, 0, entry.state.length, false);
	if (code == CARD_RESULT_READY) { *state = entry.state; *fileNo = slot; }
	return result(channel, code);
}
} // namespace PikminPsp

extern "C" {
void CARDInit(void)
{
	OSInitMutex(&sMutex);
	sCardRoot[0] = sCardDevice[0] = 0;
	memset(sCards, 0, sizeof(sCards));
	for (auto& card : sCards) card.result = CARD_RESULT_NOCARD;
}
BOOL CARDProbe(s32 channel) { Lock lock; return ensureCard(channel) == CARD_RESULT_READY; }
s32 CARDProbeEx(s32 channel, s32* memSize, s32* sectorSize)
{
	Lock lock; s32 code = ensureCard(channel);
	if (code == CARD_RESULT_READY) { if (memSize) *memSize = 16; if (sectorSize) *sectorSize = kSector; }
	return code;
}
s32 CARDGetResultCode(s32 channel) { Lock lock; return validChannel(channel) ? sCards[channel].result : CARD_RESULT_FATAL_ERROR; }
s32 CARDMount(s32 channel, CARDMemoryCard*, CARDCallback)
{
	Lock lock; s32 code = ensureCard(channel);
	if (code < 0) return result(channel, code);
	Card& card = sCards[channel]; card.mounted = false; card.broken = false; card.transferred = 0;
	memset(card.entries, 0, sizeof(card.entries));
	for (s32 i = 0; i < CARD_MAX_FILE; ++i) {
		char path[512], backup[512]; Entry entry = {};
		if (!cardPath(path, sizeof(path), channel, i) || !cardPath(backup, sizeof(backup), channel, i, ".bak"))
			return result(channel, CARD_RESULT_IOERROR);
		s32 fileCode = readEntry(path, entry);
		if (fileCode == CARD_RESULT_NOFILE || fileCode == CARD_RESULT_BROKEN) {
			Entry recovered = {}; s32 backupCode = readEntry(backup, recovered);
			if (backupCode == CARD_RESULT_READY) {
				// Retain a corrupt current file for inspection before restoring.
				if (fileCode == CARD_RESULT_BROKEN) {
					char bad[512]; cardPath(bad, sizeof(bad), channel, i, ".bad");
					if (removeIfPresent(bad) < 0 || renameStorageFile(path, bad) < 0) return result(channel, CARD_RESULT_IOERROR);
				}
				if (renameStorageFile(backup, path) < 0 || syncStorage() < 0) return result(channel, CARD_RESULT_IOERROR);
				entry = recovered; fileCode = CARD_RESULT_READY;
			} else if (backupCode == CARD_RESULT_BROKEN && fileCode == CARD_RESULT_NOFILE) fileCode = CARD_RESULT_BROKEN;
			else if (backupCode != CARD_RESULT_NOFILE && backupCode != CARD_RESULT_BROKEN) return result(channel, backupCode);
		}
		if (fileCode == CARD_RESULT_NOFILE) continue;
		if (fileCode < 0) {
			if (fileCode == CARD_RESULT_BROKEN) { card.mounted = true; card.broken = true; }
			return result(channel, fileCode);
		}
		card.entries[i] = entry;
	}
	if (usedBytes(channel) > kCapacity) {
		card.mounted = true; card.broken = true; return result(channel, CARD_RESULT_BROKEN);
	}
	card.mounted = true;
	return result(channel, CARD_RESULT_READY);
}
s32 CARDMountAsync(s32 channel, CARDMemoryCard* work, CARDCallback detach, CARDCallback attach)
{
	s32 code = CARDMount(channel, work, detach);
	// A readable but corrupt card completes mounting with BROKEN. It is not
	// a rejected request: callers inspect that completion and may offer format.
	if (code == CARD_RESULT_BROKEN) { if (attach) attach(channel, code); return CARD_RESULT_READY; }
	return callbackResult(attach, channel, code);
}
s32 CARDUnmount(s32 channel)
{
	Lock lock; if (!validChannel(channel)) return CARD_RESULT_FATAL_ERROR;
	sCards[channel].mounted = false; return result(channel, CARD_RESULT_READY);
}
s32 CARDCheck(s32 channel)
{
	Lock lock; s32 code = mounted(channel);
	if (code == CARD_RESULT_READY && sCards[channel].broken) code = CARD_RESULT_BROKEN;
	return result(channel, code);
}
s32 CARDCheckAsync(s32 channel, CARDCallback callback)
{
	s32 code = CARDCheck(channel);
	if (code == CARD_RESULT_BROKEN) { if (callback) callback(channel, code); return CARD_RESULT_READY; }
	return callbackResult(callback, channel, code);
}
s32 CARDCheckExAsync(s32 channel, s32* bytes, CARDCallback callback)
{ if (bytes) *bytes = 0; return CARDCheckAsync(channel, callback); }
s32 CARDFreeBlocks(s32 channel, s32* bytes, s32* files)
{
	Lock lock; s32 code = mounted(channel);
	if (code < 0) return result(channel, code);
	if (sCards[channel].broken) return result(channel, CARD_RESULT_BROKEN);
	s32 unused = CARD_MAX_FILE; for (const auto& entry : sCards[channel].entries) if (entry.present) --unused;
	if (bytes) *bytes = kCapacity - usedBytes(channel);
	if (files) *files = unused;
	return result(channel, CARD_RESULT_READY);
}
s32 CARDGetSectorSize(s32 channel, u32* size)
{
	Lock lock; s32 code = mounted(channel);
	if (code == CARD_RESULT_READY && size) *size = kSector;
	return result(channel, code);
}
s32 CARDFastOpen(s32 channel, s32 number, CARDFileInfo* info)
{
	Lock lock; s32 code = fileEntry(channel, number);
	if (!info) code = CARD_RESULT_FATAL_ERROR;
	if (code == CARD_RESULT_READY) fileInfo(*info, channel, number);
	return result(channel, code);
}
s32 CARDOpen(s32 channel, const char* name, CARDFileInfo* info)
{
	Lock lock; s32 code = mounted(channel);
	if (code < 0) return result(channel, code);
	if (sCards[channel].broken) return result(channel, CARD_RESULT_BROKEN);
	if (!validName(name) || !info) return result(channel, CARD_RESULT_FATAL_ERROR);
	s32 number = findName(channel, name);
	if (number < 0) return result(channel, CARD_RESULT_NOFILE);
	fileInfo(*info, channel, number); return result(channel, CARD_RESULT_READY);
}
s32 CARDClose(CARDFileInfo* info)
{
	Lock lock; if (!info) return CARD_RESULT_FATAL_ERROR;
	s32 code = fileEntry(info->chan, info->fileNo), channel = info->chan;
	info->chan = info->fileNo = -1; return result(channel, code);
}
s32 CARDCreate(s32 channel, const char* name, u32 size, CARDFileInfo* info)
{
	Lock lock; s32 code = mounted(channel);
	if (code < 0) return result(channel, code);
	if (sCards[channel].broken) return result(channel, CARD_RESULT_BROKEN);
	if (!info || !name) return result(channel, CARD_RESULT_FATAL_ERROR);
	if (!validName(name)) return result(channel, CARD_RESULT_NAMETOOLONG);
	if (findName(channel, name) >= 0) return result(channel, CARD_RESULT_EXIST);
	s32 slot = freeSlot(channel); if (slot < 0) return result(channel, CARD_RESULT_NOENT);
	Entry entry = {}; memcpy(entry.state.fileName, name, strlen(name)); entry.state.length = size;
	const DVDDiskID* disk = DVDGetCurrentDiskID();
	memcpy(entry.state.gameName, disk->gameName, 4); memcpy(entry.state.company, disk->company, 2);
	entry.state.time = (u32)OSTicksToSeconds(OSGetTime()); entry.state.iconAddr = entry.state.commentAddr = ~0U;
	if (!validState(entry.state)) return result(channel, CARD_RESULT_FATAL_ERROR);
	code = commit(channel, slot, entry, nullptr, 0, 0, false);
	if (code == CARD_RESULT_READY) fileInfo(*info, channel, slot);
	return result(channel, code);
}
s32 CARDCreateAsync(s32 channel, const char* name, u32 size, CARDFileInfo* info, CARDCallback callback)
{ return callbackResult(callback, channel, CARDCreate(channel, name, size, info)); }
s32 CARDRead(CARDFileInfo* info, void* data, s32 length, s32 offset)
{
	Lock lock; if (!info) return CARD_RESULT_FATAL_ERROR;
	s32 channel = info->chan, code = fileEntry(channel, info->fileNo);
	if (code < 0) return result(channel, code);
	u32 size = sCards[channel].entries[info->fileNo].state.length;
	if (!data || length < 0 || offset < 0 || (u32)offset > size || (u32)length > size - offset)
		return result(channel, CARD_RESULT_FATAL_ERROR);
	char path[512]; if (!cardPath(path, sizeof(path), channel, info->fileNo)) return result(channel, CARD_RESULT_IOERROR);
	SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0); if (fd < 0) return result(channel, ioResult(fd));
	code = sceIoLseek(fd, kHeaderSize + offset, PSP_SEEK_SET) < 0 ? CARD_RESULT_IOERROR : transfer(fd, data, length, false);
	if (sceIoClose(fd) < 0 && code == CARD_RESULT_READY) code = CARD_RESULT_IOERROR;
	if (code == CARD_RESULT_READY) { sCards[channel].transferred += length; info->offset = offset + length; }
	return result(channel, code);
}
s32 CARDReadAsync(CARDFileInfo* info, void* data, s32 length, s32 offset, CARDCallback callback)
{ s32 channel = info ? info->chan : -1; return callbackResult(callback, channel, CARDRead(info, data, length, offset)); }
s32 CARDWrite(CARDFileInfo* info, void* data, s32 length, s32 offset)
{
	Lock lock; if (!info) return CARD_RESULT_FATAL_ERROR;
	s32 channel = info->chan, code = fileEntry(channel, info->fileNo);
	if (code < 0) return result(channel, code);
	Entry entry = sCards[channel].entries[info->fileNo];
	if (!data || length < 0 || offset < 0 || (u32)offset > entry.state.length || (u32)length > entry.state.length - offset)
		return result(channel, CARD_RESULT_FATAL_ERROR);
	entry.state.time = (u32)OSTicksToSeconds(OSGetTime());
	code = commit(channel, info->fileNo, entry, data, offset, length, true);
	if (code == CARD_RESULT_READY) info->offset = offset + length;
	return result(channel, code);
}
s32 CARDWriteAsync(CARDFileInfo* info, void* data, s32 length, s32 offset, CARDCallback callback)
{ s32 channel = info ? info->chan : -1; return callbackResult(callback, channel, CARDWrite(info, data, length, offset)); }
s32 CARDGetXferredBytes(s32 channel)
{ Lock lock; return validChannel(channel) ? sCards[channel].transferred : 0; }
s32 CARDGetStatus(s32 channel, s32 number, CARDStat* state)
{
	Lock lock; s32 code = fileEntry(channel, number);
	if (!state) code = CARD_RESULT_FATAL_ERROR;
	if (code == CARD_RESULT_READY) *state = sCards[channel].entries[number].state;
	return result(channel, code);
}
s32 CARDSetStatus(s32 channel, s32 number, CARDStat* state)
{
	Lock lock; s32 code = fileEntry(channel, number);
	if (code < 0) return result(channel, code);
	if (!state) return result(channel, CARD_RESULT_FATAL_ERROR);
	Entry entry = sCards[channel].entries[number];
	// Dolphin SetStatus edits display metadata, not the filename or length.
	entry.state.bannerFormat = state->bannerFormat; entry.state.iconAddr = state->iconAddr;
	entry.state.iconFormat = state->iconFormat; entry.state.iconSpeed = state->iconSpeed; entry.state.commentAddr = state->commentAddr;
	entry.state.time = (u32)OSTicksToSeconds(OSGetTime());
	if (!validState(entry.state)) return result(channel, CARD_RESULT_FATAL_ERROR);
	code = commit(channel, number, entry, nullptr, 0, 0, true);
	if (code == CARD_RESULT_READY) *state = entry.state;
	return result(channel, code);
}
s32 CARDSetStatusAsync(s32 channel, s32 number, CARDStat* state, CARDCallback callback)
{ return callbackResult(callback, channel, CARDSetStatus(channel, number, state)); }
s32 CARDRename(s32 channel, const char* oldName, const char* newName)
{
	Lock lock; s32 code = mounted(channel);
	if (code < 0) return result(channel, code);
	if (sCards[channel].broken) return result(channel, CARD_RESULT_BROKEN);
	if (!validName(oldName) || !validName(newName)) return result(channel, CARD_RESULT_NAMETOOLONG);
	s32 number = findName(channel, oldName);
	if (number < 0) return result(channel, CARD_RESULT_NOFILE);
	if (findName(channel, newName) >= 0) return result(channel, CARD_RESULT_EXIST);
	Entry entry = sCards[channel].entries[number]; memset(entry.state.fileName, 0, CARD_FILENAME_MAX);
	memcpy(entry.state.fileName, newName, strlen(newName));
	return result(channel, commit(channel, number, entry, nullptr, 0, 0, true));
}
s32 CARDRenameAsync(s32 channel, const char* oldName, const char* newName, CARDCallback callback)
{ return callbackResult(callback, channel, CARDRename(channel, oldName, newName)); }
s32 CARDFastDelete(s32 channel, s32 number)
{
	Lock lock; s32 code = fileEntry(channel, number);
	if (code < 0) return result(channel, code);
	char path[512], backup[512];
	if (!cardPath(path, sizeof(path), channel, number) || !cardPath(backup, sizeof(backup), channel, number, ".bak"))
		return result(channel, CARD_RESULT_IOERROR);
	// Remove the backup first so an interrupted deletion cannot resurrect it.
	code = removeIfPresent(backup);
	if (code == CARD_RESULT_READY) code = ioResult(sceIoRemove(path));
	if (code == CARD_RESULT_READY) { sCards[channel].entries[number] = {}; code = syncStorage(); }
	return result(channel, code);
}
s32 CARDFastDeleteAsync(s32 channel, s32 number, CARDCallback callback)
{ return callbackResult(callback, channel, CARDFastDelete(channel, number)); }
s32 CARDFormat(s32 channel)
{
	Lock lock; s32 code = ensureCard(channel);
	if (code < 0) return result(channel, code);
	sCards[channel].broken = true;
	for (s32 number = 0; number < CARD_MAX_FILE; ++number) for (const char* suffix : {"", ".bak", ".new", ".bad"}) {
		char path[512];
		if (!cardPath(path, sizeof(path), channel, number, suffix)) return result(channel, CARD_RESULT_IOERROR);
		code = removeIfPresent(path); if (code < 0) return result(channel, code);
	}
	memset(sCards[channel].entries, 0, sizeof(sCards[channel].entries));
	sCards[channel].mounted = true; sCards[channel].broken = false;
	return result(channel, syncStorage());
}
s32 CARDFormatAsync(s32 channel, CARDCallback callback) { return callbackResult(callback, channel, CARDFormat(channel)); }
s32 CARDGetSerialNo(s32 channel, u64* serial)
{
	Lock lock; s32 code = mounted(channel); if (!serial) code = CARD_RESULT_FATAL_ERROR;
	if (code == CARD_RESULT_READY) *serial = 0x504b505350000000ULL + channel;
	return result(channel, code);
}
} // extern "C"
