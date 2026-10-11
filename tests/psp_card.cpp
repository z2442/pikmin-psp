// Exercise the production backend against real native files with PSP I/O
// shims, including interrupted transactions and the game's own save helpers.
#include "../src/port/psp/PspDolphinCard.cpp"
#include "CardUtil.h"
#include "endianness.h"
#include <pspthreadman.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::string storageRoot;
std::string launchRoot = "ms0:/PSP/GAME/pikmin";
int hostWrites;
bool rejectStorageRootStat;
std::vector<std::string> syncDevices;
std::string nativePath(const char* path)
{
	if (!strncmp(path, "ms0:", 4)) return storageRoot + "/ms0" + (path + 4);
	if (!strncmp(path, "ef0:", 4)) return storageRoot + "/ef0" + (path + 4);
	if (!strncmp(path, "host0:", 6)) return storageRoot + "/host" + (path + 6);
	return path;
}
void countHostWrite(const char* path) { if (!strncmp(path, "host0:", 6)) ++hostWrites; }
std::recursive_mutex nativeMutex;
std::set<int> writeDescriptors;
int failWrite, failRename, failClose, failSync, failRemove;
int writeCalls, renameCalls, closeCalls, syncCalls, removeCalls, mutationCalls, crashAt;
int shortTransfer;
bool failedRead;
int callbackCalls, sleepCalls;
bool completeJobOnSleep;
bool sCardThreadReady;
enum class SyncRoute { SdkDriver, MountAlias, FatDriver, Unsupported, KernelUnsupported, KernelNoDevice, KernelPermission, KernelInvalid,
                       KernelIoFailure, KernelNoMemory, GenericUnsupported };
SyncRoute syncRoute = SyncRoute::SdkDriver;
bool corruptStaged;
std::string lastWritePath;
int errorCode(int error) { return (int)(0x80010000U | error); }
int ioFailure() { return errorCode(errno); }
void mutation() { if (crashAt && ++mutationCalls == crashAt) _exit(77); }
void resetFaults()
{
	failWrite = failRename = failClose = failSync = failRemove = 0;
	writeCalls = renameCalls = closeCalls = syncCalls = removeCalls = mutationCalls = crashAt = shortTransfer = 0;
	failedRead = false;
	corruptStaged = false; syncRoute = SyncRoute::SdkDriver;
	syncDevices.clear();
}
} // namespace

CardUtilControl CardControl = {};
struct RamStream;
struct Stream;
struct PlayState;
struct MemoryCard {
	bool mDidSaveFail = false;
	int mTempFileIndex = -1, mCardChannel = -1, mSaveFileIndex = -1;
	u32 mRequiredFreeSpace = 0x26000, mErrorCode = 0, mSectorSize = 0;
	char mFilePath[32] = {};
	u32 mOkSectionsMask = 0;
	BOOL mValidSlots[4] = {}; int mValidBlockCount = 0, mValidOptionsCount = 0;
	bool hasCardFinished(); void waitPolling(); u32 calcChecksum(void*, u32);
	s32 waitWhileBusy(int); bool getCardStatus(int); void checkUseFile(); s32 getMemoryCardState(bool); void loadCurrentFile();
	int getOptionsOffset(int); int getGameFileOffset(int);
	void* getBannerPtr(); void* getOptionsPtr(int); void* getGameFilePtr(int);
	RamStream* getBannerStream(); RamStream* getOptionsStream(int); RamStream* getGameFileStream(int);
	void createFile(CARDStat&); void initBannerArea(CARDStat&, const char*); void initOptionsArea(int); void initFileArea(int,int);
	s32 makeDefaultFile(); u32 getOkSections(); bool isFileBroken(); bool didSaveFail();
	void writeCurrentGame(Stream*, PlayState&);
};
static CARDMemoryCard CardWorkArea;
static CARDStat cst;
static const char basecardname[] = "Pikmin dataFile";
#define CARD_DATA_SIZE 0x26000
u8 cardData[CARD_DATA_SIZE];
#define PRINT(...) ((void)0)
struct Stream {
	std::vector<u8> bytes;
	int position = 0;
	u8* external = nullptr; int capacity = 0;
	int readInt(); u8 readByte(); short readShort(); f32 readFloat();
	void writeInt(int); void writeByte(u8); void writeShort(short); void writeFloat(f32);
	void read(void* data, int size) {
		assert(position + size <= (external ? capacity : (int)bytes.size()));
		memcpy(data, (external ? external : bytes.data()) + position, size); position += size;
	}
	void write(const void* data, int size) {
		if (external) { assert(position + size <= capacity); memcpy(external + position, data, size); }
		else { bytes.resize(position + size); memcpy(bytes.data() + position, data, size); }
		position += size;
	}
	void setPosition(int value) { position = value; }
	void padFileTo(u32 amount, u32 offset) { assert(amount >= (u32)position + offset); while ((u32)position < amount - offset) writeByte(0); }
};
using RandomAccessStream = Stream;
struct RamStream : Stream {
	int& mPosition = position;
	RamStream(void* buffer, int size) { external = static_cast<u8*>(buffer); capacity = size; }
};
struct PlayState {
	u8 mSaveStatus = 1, mSaveSlot = 0, mSavedDay = 1, mShipPartsCount = 0;
	int mRedPikiCount = -1, mYellowPikiCount = -1, mBluePikiCount = -1; u32 mCourseOpenFlags = 1;
	void Initialise() { mSaveStatus = 1; mSavedDay = 1; mShipPartsCount = 0; mRedPikiCount = mYellowPikiCount = mBluePikiCount = -1; mCourseOpenFlags = 1; }
	void write(Stream&); void read(Stream&);
};
struct { MemoryCard mMemoryCard; struct {
	int mMostRecentOptionsIndex = 0, mMostRecentSaveIndex = 0;
	void write(Stream& output) { output.writeInt(1); output.writeByte(8); output.writeByte(8); output.writeByte(0); }
} mGamePrefs; } gameflow;
struct { bool mIsCardSaving = false; } fakeSystem, *gsys = &fakeSystem;
#include "stream.inc"
#include "play_state.inc"
void MemoryCard::writeCurrentGame(Stream* output, PlayState& state) { state.write(*output); }

namespace PikminPsp {
const char* executableRoot() { return launchRoot.c_str(); }
bool resolvePath(char* output, u32 size, const char* relative)
{
	const char* root = !strncmp(launchRoot.c_str(), "host0:", 6) ? launchRoot.c_str() : storageRoot.c_str();
	int count = snprintf(output, size, "%s/%s", root, relative);
	return count >= 0 && (u32)count < size;
}
}
extern "C" {
u32 __OSBusClock = 4000000;
void OSInitMutex(OSMutex* mutex) { memset(mutex, 0, sizeof(*mutex)); }
void OSLockMutex(OSMutex*) { nativeMutex.lock(); }
void OSUnlockMutex(OSMutex*) { nativeMutex.unlock(); }
void OSSignalCond(OSCond*) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
OSTime OSGetTime() { return 123000000; }
void OSTicksToCalendarTime(OSTime, OSCalendarTime* calendar) { memset(calendar, 0, sizeof(*calendar)); }
DVDDiskID* DVDGetCurrentDiskID()
{
	static DVDDiskID disk = {{'G','P','I','E'}, {'0','1'}, 0, 1, 0, 0, {}};
	return &disk;
}
int sceKernelDelayThread(SceUInt microseconds)
{
	assert(microseconds > 0); ++sleepCalls;
	if (completeJobOnSleep) CardControl.mChannel = -1;
	return 0;
}
SceUID sceIoOpen(const char* path, int flags, SceMode mode)
{
	int nativeFlags = (flags & PSP_O_RDWR) == PSP_O_RDWR ? O_RDWR : flags & PSP_O_WRONLY ? O_WRONLY : O_RDONLY;
	if (flags & PSP_O_CREAT) nativeFlags |= O_CREAT;
	if (flags & PSP_O_TRUNC) nativeFlags |= O_TRUNC;
	if (flags & (PSP_O_WRONLY | PSP_O_CREAT)) countHostWrite(path);
	std::string native = nativePath(path);
	int fd = open(native.c_str(), nativeFlags, mode);
	if (fd < 0) return ioFailure();
	if (flags & PSP_O_WRONLY) { writeDescriptors.insert(fd); lastWritePath = native; }
	return fd;
}
int sceIoClose(SceUID fd)
{
	bool writer = writeDescriptors.erase(fd) != 0;
	int code = close(fd);
	if (writer) {
		if (++closeCalls == failClose) return errorCode(EIO);
		if (corruptStaged) {
			corruptStaged = false;
			int corrupt = open(lastWritePath.c_str(), O_RDWR); assert(corrupt >= 0);
			u8 byte; assert(pread(corrupt, &byte, 1, 128) == 1); byte ^= 1;
			assert(pwrite(corrupt, &byte, 1, 128) == 1); close(corrupt);
		}
		mutation();
	}
	return code < 0 ? ioFailure() : 0;
}
int sceIoRead(SceUID fd, void* data, SceSize size)
{
	if (failedRead) { failedRead = false; return errorCode(EIO); }
	if (shortTransfer) size = std::min<SceSize>(size, shortTransfer);
	ssize_t count = read(fd, data, size); return count < 0 ? ioFailure() : count;
}
int sceIoWrite(SceUID fd, const void* data, SceSize size)
{
	if (++writeCalls == failWrite) return errorCode(ENOSPC);
	if (shortTransfer) size = std::min<SceSize>(size, shortTransfer);
	ssize_t count = write(fd, data, size);
	mutation(); return count < 0 ? ioFailure() : count;
}
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)
{ off_t result = lseek(fd, offset, whence); return result < 0 ? ioFailure() : result; }
int sceIoGetstat(const char* path, SceIoStat* output)
{
	if (rejectStorageRootStat && (!strcmp(path, "ms0:/") || !strcmp(path, "ef0:/"))) return errorCode(EINVAL);
	struct stat info;
	if (stat(nativePath(path).c_str(), &info) < 0) return ioFailure();
	memset(output, 0, sizeof(*output));
	output->st_mode = S_ISDIR(info.st_mode) ? FIO_S_IFDIR : FIO_S_IFREG; output->st_size = info.st_size;
	return 0;
}
int sceIoMkdir(const char* path, SceMode mode)
{ countHostWrite(path); return mkdir(nativePath(path).c_str(), mode) < 0 ? ioFailure() : 0; }
int sceIoRemove(const char* path)
{
	if (++removeCalls == failRemove) return errorCode(EIO);
	countHostWrite(path);
	int code = unlink(nativePath(path).c_str()); mutation(); return code < 0 ? ioFailure() : 0;
}
int sceIoRename(const char* from, const char* to)
{
	if (++renameCalls == failRename) return errorCode(EIO);
	countHostWrite(from);
	// Firmware expands the source, but USB HostFS receives the destination
	// verbatim. A device-qualified destination becomes an invalid host path.
	if (!strncmp(from, "host0:", 6) && strchr(to, ':')) return errorCode(ENOENT);
	std::string destination = to;
	if (!strchr(to, ':') && to[0] != '/') {
		const char* slash = strrchr(from, '/');
		assert(slash);
		destination = std::string(from, slash - from + 1) + to;
	}
	// PSP need not support replacing an existing destination.
	if (std::filesystem::exists(nativePath(destination.c_str()))) return errorCode(EEXIST);
	int code = rename(nativePath(from).c_str(), nativePath(destination.c_str()).c_str()); mutation(); return code < 0 ? ioFailure() : 0;
}
int sceIoSync(const char* device, unsigned int)
{
	syncDevices.emplace_back(device);
	if (syncRoute == SyncRoute::GenericUnsupported) return -1;
	// Drivers may report either errno-family errors or PSP kernel I/O errors.
	// These kernel codes are distinct from 0x80010000 | errno.
	if (syncRoute == SyncRoute::KernelUnsupported) return (int)0x80020325U;
	if (syncRoute == SyncRoute::KernelNoDevice) return (int)0x80020321U;
	if (syncRoute == SyncRoute::KernelPermission) return (int)0x800200d1U;
	if (syncRoute == SyncRoute::KernelInvalid) return (int)0x80020324U;
	if (syncRoute == SyncRoute::KernelIoFailure) return (int)0x800203e9U;
	if (syncRoute == SyncRoute::KernelNoMemory) return (int)0x800203eaU;
	const char* supported = syncRoute == SyncRoute::SdkDriver ? "msfat0:"
	    : syncRoute == SyncRoute::MountAlias ? "ms0:" : "fatms0:";
	if (syncRoute == SyncRoute::Unsupported || strcmp(device, supported)) return errorCode(ENOTSUP);
	if (++syncCalls == failSync) return errorCode(EIO);
	mutation(); return 0;
}
}

#include "cardutil.inc"
void CardUtilUnmount(s32 channel) { CardControl.mResultCode = CARDUnmount(channel); }
void CardUtilMount(s32 channel, void* work) { CardControl.mResultCode = DoMount(channel, work); }
void CardUtilOpen(s32 channel, s32 number, void* data) { CardControl.mResultCode = DoOpen(channel, number, data); }
void CardUtilErase(s32 channel, s32 number) { CardControl.mResultCode = CARDFastDelete(channel, number); }
void CardUtilSave(s32 channel, CARDStat* state, void* data) { CardUtilCommand(channel, CARDCMD_Save, reinterpret_cast<intptr_t>(state), data, 0, 0); }
#include "memory_wait.inc"
#include "card_assets.inc"
#include "default_file.inc"

namespace {
std::vector<u8> pattern(u32 bytes, u8 seed)
{
	std::vector<u8> data(bytes);
	for (u32 i = 0; i < bytes; ++i) data[i] = (i * 37 + seed) & 255;
	return data;
}
CARDStat saveState(u32 size = 0x26000)
{
	CARDStat state = {}; strcpy(state.fileName, "Pikmin dataFile"); state.length = size;
	state.commentAddr = 0; state.iconAddr = 64; state.bannerFormat = CARD_STAT_BANNER_C8 | CARD_STAT_ANIM_BOUNCE;
	state.iconFormat = 5; state.iconSpeed = 15;
	return state;
}
std::vector<u8> readSave(s32 channel = 0, const char* name = "Pikmin dataFile")
{
	CARDFileInfo info; assert(CARDOpen(channel, name, &info) == 0);
	std::vector<u8> data(info.length);
	assert(CARDRead(&info, data.data(), data.size(), 0) == 0);
	assert(CARDClose(&info) == 0); return data;
}
void restart()
{
	resetFaults(); CARDInit(); assert(CARDMount(0, nullptr, nullptr) == 0);
}
void callback(s32 channel, s32 code)
{
	assert(channel == 0 && code == 0); ++callbackCalls;
	// A callback must run after the backend releases its mutex.
	std::thread other([channel] { assert(CARDGetResultCode(channel) == 0); }); other.join();
}
void basicSave()
{
	CARDInit(); assert(!CARDProbe(-1) && !CARDProbe(2));
	assert(CARDOpen(0, "missing", nullptr) == CARD_RESULT_NOCARD);
	assert(CARDProbe(0) && CARDProbe(1));
	assert(CARDMountAsync(0, nullptr, nullptr, callback) == 0 && callbackCalls == 1);
	assert(CARDCheckAsync(0, callback) == 0 && callbackCalls == 2);
	s32 bytes, files; assert(CARDFreeBlocks(0, &bytes, &files) == 0 && bytes == (s32)kCapacity && files == 127);
	u32 sector; assert(CARDGetSectorSize(0, &sector) == 0 && sector == 8192);
	assert((crc(~0U, "123456789", 9) ^ ~0U) == 0xcbf43926);
	CARDStat state = saveState(); auto expected = pattern(state.length, 3);
	std::vector<CardUtilDirent> listing(CARD_MAX_FILE);
	CardControl.mDirentList = listing.data();
	assert(DoSave(0, &state, expected.data()) == 0);
	assert(CardControl.mDirentCount == 1 && listing[0].mFileNo == 0);
	assert(!memcmp(listing[0].mFileCommentData, expected.data(), CARD_COMMENT_SIZE));
	assert(state.offsetBanner == 64 && state.offsetBannerTlut == 3136);
	assert(state.offsetIcon[0] == 3648 && state.offsetIcon[1] == 4672 && state.offsetIconTlut == 5696 && state.offsetData == 6208);
	assert(readSave() == expected);
	assert(CARDFreeBlocks(0, &bytes, &files) == 0 && files == 126 && bytes == (s32)(kCapacity - state.length));
	assert(DoMount(0, nullptr) == 0);
	assert(DoList(0, listing.data()) == 0 && CardControl.mDirentCount == 1);
	assert(!memcmp(listing[0].mFileCommentData, expected.data(), CARD_COMMENT_SIZE));
	assert(!memcmp(listing[0].mFileData, expected.data() + 64, state.offsetData - state.iconAddr));
	std::vector<u8> loaded(state.length);
	assert(DoOpen(0, 0, loaded.data()) == 0 && loaded == expected);
	// Follow the game's block layout: banner, two options copies, four saves.
	for (auto part : {std::pair<u32,u32>(0,8192), {8192,8192}, {16384,8192}, {24576,32768},
	                 {57344,32768}, {90112,32768}, {122880,32768}}) {
		auto patch = pattern(part.second, (u8)(part.first / 8192));
		assert(DoWrite(0, 0, patch.data(), patch.size(), part.first) == 0);
		std::copy(patch.begin(), patch.end(), expected.begin() + part.first);
		assert(readSave() == expected);
	}
	// A full replacement retains its file index and updates the utility list.
	expected = pattern(state.length, 91);
	assert(DoSave(0, &state, expected.data()) == 0 && listing[0].mFileNo == 0 && CardControl.mDirentCount == 1);
	assert(CARDUnmount(0) == 0); restart(); assert(readSave() == expected);
	CardControl.mDirentList = nullptr;
	puts("Game save/create, banner metadata, directory listing, all seven block writes and remount passed");
}

void emptyCardGameCheck()
{
	CARDInit(); CardControl.mChannel = -1;
	MemoryCard card;
	assert(card.getMemoryCardState(false) == CARD_RESULT_READY);
	assert(card.mCardChannel == 0 && card.mSaveFileIndex == -1 && card.mSectorSize == 8192);
	assert(card.getMemoryCardState(true) == CARD_RESULT_READY && card.mSaveFileIndex == -1);
	// Outstanding utility work must finish before the game starts a new check.
	CardControl.mChannel = 0; completeJobOnSleep = true;
	int slept = sleepCalls;
	assert(card.getMemoryCardState(false) == CARD_RESULT_READY);
	assert(CardControl.mChannel == -1 && sleepCalls > slept);
	completeJobOnSleep = false;
	puts("Production empty-card startup checks passed before any save was created");
}
void boundsAndCallbacks()
{
	CARDFileInfo info; assert(CARDOpen(0, "Pikmin dataFile", &info) == 0);
	u8 byte = 0;
	assert(CARDRead(&info, &byte, -1, 0) == CARD_RESULT_FATAL_ERROR);
	assert(CARDRead(&info, &byte, 1, info.length) == CARD_RESULT_FATAL_ERROR);
	assert(CARDWrite(&info, &byte, 1, -1) == CARD_RESULT_FATAL_ERROR);
	assert(CARDWrite(&info, &byte, INT32_MAX, INT32_MAX) == CARD_RESULT_FATAL_ERROR);
	assert(CARDFastOpen(0, 127, &info) == CARD_RESULT_FATAL_ERROR);
	assert(CARDOpen(0, "Pikmin dataFile", &info) == 0);
	int before = callbackCalls;
	assert(CARDReadAsync(&info, &byte, -1, 0, callback) < 0 && callbackCalls == before);
	assert(CARDClose(&info) == 0 && CARDRead(&info, &byte, 1, 0) == CARD_RESULT_FATAL_ERROR);
	std::string longName(33, 'n'); assert(CARDCreate(0, longName.c_str(), 8192, &info) == CARD_RESULT_NAMETOOLONG);
	assert(CARDCreate(0, "unaligned", 8193, &info) == CARD_RESULT_FATAL_ERROR);
	assert(CARDCreate(0, "Pikmin dataFile", 8192, &info) == CARD_RESULT_EXIST);
	assert(CARDCreateAsync(0, "temporary", 8192, &info, callback) == 0);
	auto data = pattern(8192, 7); shortTransfer = 13;
	assert(CARDWriteAsync(&info, data.data(), data.size(), 0, callback) == 0);
	std::vector<u8> copy(data.size()); assert(CARDReadAsync(&info, copy.data(), copy.size(), 0, callback) == 0 && copy == data);
	shortTransfer = 0;
	CARDStat state; assert(CARDGetStatus(0, info.fileNo, &state) == 0);
	strcpy(state.fileName, "should not rename"); state.length = 999;
	assert(CARDSetStatusAsync(0, info.fileNo, &state, callback) == 0);
	assert(!strcmp(state.fileName, "temporary") && state.length == 8192);
	assert(CARDRenameAsync(0, "temporary", "renamed", callback) == 0);
	assert(CARDOpen(0, "temporary", &info) == CARD_RESULT_NOFILE);
	assert(readSave(0, "renamed") == data);
	assert(CARDOpen(0, "renamed", &info) == 0);
	assert(CARDFastDeleteAsync(0, info.fileNo, callback) == 0);
	restart(); assert(CARDOpen(0, "renamed", &info) == CARD_RESULT_NOFILE);
	assert(CARDMount(1, nullptr, nullptr) == 0);
	assert(CARDOpen(1, "Pikmin dataFile", &info) == CARD_RESULT_NOFILE);
	assert(CARDCreate(1, "other channel", 8192, &info) == 0);
	assert(CARDFormat(1) == 0 && CARDMount(1, nullptr, nullptr) == 0);
	assert(CARDOpen(1, "other channel", &info) == CARD_RESULT_NOFILE);
	assert(!readSave().empty());
	puts("Bounds, short transfers, callback reentry, metadata, rename/delete and channel isolation passed");
}
void failureAndRecovery()
{
	CARDStat state = saveState(8192); strcpy(state.fileName, "transaction");
	auto oldData = pattern(8192, 3), newData = pattern(8192, 6);
	s32 number; assert(PikminPsp::saveCardFile(0, &state, oldData.data(), &number) == 0);
	for (int write = 1; write <= 10; ++write) {
		resetFaults(); failWrite = write;
		assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == CARD_RESULT_INSSPACE);
		restart(); assert(readSave(0, "transaction") == oldData);
	}
	for (int step = 1; step <= 2; ++step) {
		resetFaults(); failRename = step;
		assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == CARD_RESULT_IOERROR);
		restart(); assert(readSave(0, "transaction") == oldData);
	}
	resetFaults(); failClose = 1;
	assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == CARD_RESULT_IOERROR);
	restart(); assert(readSave(0, "transaction") == oldData);
	resetFaults(); failSync = 1;
	assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == CARD_RESULT_IOERROR);
	restart(); assert(readSave(0, "transaction") == oldData);
	resetFaults(); failedRead = true;
	assert(CARDMount(0, nullptr, nullptr) == CARD_RESULT_IOERROR);
	restart(); assert(readSave(0, "transaction") == oldData);
	// Kill a child immediately after each mutation in the commit sequence.
	// After remount, the save must be an entire old or entire new payload.
	int tested = 0;
	for (int step = 1; step <= 32; ++step) {
		resetFaults(); assert(PikminPsp::saveCardFile(0, &state, oldData.data(), &number) == 0);
		pid_t child = fork(); assert(child >= 0);
		if (!child) {
			resetFaults(); crashAt = step;
			assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == 0);
			_exit(0);
		}
		int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
		restart(); auto survived = readSave(0, "transaction");
		assert(survived == oldData || survived == newData);
		if (WEXITSTATUS(status) == 0) break;
		assert(WEXITSTATUS(status) == 77); ++tested;
	}
	assert(tested >= 15);
	// Corrupt both payload and header in turn; a valid backup must recover.
	for (int offset : {24, 128 + 512}) {
		assert(PikminPsp::saveCardFile(0, &state, oldData.data(), &number) == 0);
		assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == 0);
		char path[512]; assert(cardPath(path, sizeof(path), 0, number));
		int fd = open(path, O_RDWR); assert(fd >= 0);
		u8 value; assert(pread(fd, &value, 1, offset) == 1); value ^= 1;
		assert(pwrite(fd, &value, 1, offset) == 1); close(fd);
		restart(); assert(readSave(0, "transaction") == oldData);
	}
	// An unbacked corrupt file is reported as broken, not a fresh empty card.
	CARDFileInfo info; assert(CARDCreate(0, "broken", 8192, &info) == 0);
	char path[512]; cardPath(path, sizeof(path), 0, info.fileNo);
	assert(truncate(path, 128) == 0);
	CARDInit(); assert(CARDMount(0, nullptr, nullptr) == CARD_RESULT_BROKEN);
	assert(CARDMountAsync(0, nullptr, nullptr, nullptr) == 0 && CARDGetResultCode(0) == CARD_RESULT_BROKEN);
	assert(CARDCheckAsync(0, nullptr) == 0 && CARDGetResultCode(0) == CARD_RESULT_BROKEN);
	u32 sector; assert(CARDGetSectorSize(0, &sector) == 0 && sector == 8192);
	assert(CARDOpen(0, "transaction", &info) == CARD_RESULT_BROKEN);
	assert(unlink(path) == 0); restart();
	printf("Write/close/sync/rename failures, %d interrupted commit points, CRC recovery and corrupt-file errors passed\n", tested);
}
void capacityAndGameWait()
{
	assert(CARDMount(1, nullptr, nullptr) == 0);
	CARDFileInfo info; assert(CARDCreate(1, "broken", 8192, &info) == 0);
	char path[512]; cardPath(path, sizeof(path), 1, info.fileNo);
	assert(truncate(path, 128) == 0);
	assert(CARDMountAsync(1, nullptr, nullptr, nullptr) == 0 && CARDGetResultCode(1) == CARD_RESULT_BROKEN);
	assert(CARDFormatAsync(1, nullptr) == 0 && CARDCheck(1) == 0);
	assert(CARDCreate(1, "full", kCapacity, &info) == 0);
	assert(CARDCreate(1, "extra", 8192, &info) == CARD_RESULT_INSSPACE);
	s32 bytes, files; assert(CARDFreeBlocks(1, &bytes, &files) == 0 && bytes == 0 && files == 126);
	assert(CARDFormat(1) == 0);
	for (int i = 0; i < 127; ++i) {
		char name[32]; snprintf(name, sizeof(name), "slot%d", i);
		assert(CARDCreate(1, name, 8192, &info) == 0);
	}
	assert(CARDCreate(1, "overflow", 8192, &info) == CARD_RESULT_NOENT);
	assert(CARDFormat(1) == 0);
	MemoryCard card;
	CardControl.mChannel = -1; CardControl.mResultCode = CARD_RESULT_INSSPACE;
	assert(card.hasCardFinished() && card.mDidSaveFail); // Unmount success must not erase the error.
	card.mDidSaveFail = false; CardControl.mChannel = 0; CardControl.mResultCode = 0;
	completeJobOnSleep = true; int before = sleepCalls; card.waitPolling(); completeJobOnSleep = false;
	assert(sleepCalls > before && !card.mDidSaveFail);
	restart();
	puts("Virtual byte/file capacity and production save-result/polling paths passed");
}

void serializedSaves()
{
	Stream stream;
	const int integers[] = {0, 127, 128, 255, INT32_MIN, INT32_MAX, -1, (int)0xfedcba98U};
	const short shorts[] = {0, 127, 255, 32767, -32768, -1};
	const u32 floats[] = {0, 0x80000000U, 0x3f800000U, 0xbf800000U, 0x7f800000U, 0xff800000U, 0x7fc12345U};
	for (int value : integers) stream.writeInt(value);
	for (short value : shorts) stream.writeShort(value);
	for (u32 bits : floats) { f32 value; memcpy(&value, &bits, 4); stream.writeFloat(value); }
	const u8 firstBytes[] = {0,0,0,0, 0,0,0,127, 0,0,0,128, 0,0,0,255,
	                        128,0,0,0, 127,255,255,255, 255,255,255,255, 254,220,186,152};
	assert(!memcmp(stream.bytes.data(), firstBytes, sizeof(firstBytes)));
	stream.position = 0;
	for (int value : integers) assert(stream.readInt() == value);
	for (short value : shorts) assert(stream.readShort() == value);
	for (u32 bits : floats) { f32 value = stream.readFloat(); u32 got; memcpy(&got, &value, 4); assert(bits == got); }
	MemoryCard card;
	auto data = pattern(8192, 91);
	assert(card.calcChecksum(data.data(), 0x1ffc) == 0xd2f11337U); // Golden big-endian checksum.
	Stream checksum;
	checksum.writeInt((int)card.calcChecksum(data.data(), 0x1ffc));
	std::copy(checksum.bytes.begin(), checksum.bytes.end(), data.begin() + 0x1ffc);
	CARDStat state = saveState(8192); strcpy(state.fileName, "serialized"); s32 number;
	assert(PikminPsp::saveCardFile(0, &state, data.data(), &number) == 0);
	restart(); auto loaded = readSave(0, "serialized"); assert(loaded == data);
	checksum.bytes.assign(loaded.begin() + 0x1ffc, loaded.end()); checksum.position = 0;
	assert((u32)checksum.readInt() == card.calcChecksum(loaded.data(), 0x1ffc));
	std::vector<u8> unaligned(loaded.size() + 1); std::copy(loaded.begin(), loaded.end(), unaligned.begin() + 1);
	assert(card.calcChecksum(unaligned.data() + 1, 0x1ffc) == 0xd2f11337U);
	puts("Production integer/short/float serialization and big-endian game checksum survived save/reload");
}

void portableCommitAndWorkerFallback()
{
	CARDStat state = saveState(8192); strcpy(state.fileName, "fallback");
	auto data = pattern(8192, 19); s32 number;
	for (SyncRoute route : {SyncRoute::SdkDriver, SyncRoute::MountAlias, SyncRoute::FatDriver, SyncRoute::Unsupported,
	                       SyncRoute::KernelUnsupported, SyncRoute::KernelNoDevice, SyncRoute::KernelPermission, SyncRoute::KernelInvalid}) {
		resetFaults(); syncRoute = route;
		assert(PikminPsp::saveCardFile(0, &state, data.data(), &number) == 0);
		assert(readSave(0, "fallback") == data);
	}
	resetFaults(); corruptStaged = true;
	auto changed = pattern(8192, 41);
	assert(PikminPsp::saveCardFile(0, &state, changed.data(), &number) == CARD_RESULT_BROKEN);
	assert(readSave(0, "fallback") == data); // Failed verification cannot replace the current save.
	for (SyncRoute route : {SyncRoute::KernelIoFailure, SyncRoute::KernelNoMemory, SyncRoute::GenericUnsupported}) {
		resetFaults(); syncRoute = route;
		assert(PikminPsp::saveCardFile(0, &state, changed.data(), &number) == CARD_RESULT_IOERROR);
		assert(readSave(0, "fallback") == data);
	}
	resetFaults(); CardControl.mChannel = -1; sCardThreadReady = false;
	assert(CardUtilCommand(0, CARDCMD_Mount, 0, &CardWorkArea, 0, 0) == 0);
	assert(CardUtilCommand(0, CARDCMD_Save, reinterpret_cast<intptr_t>(&state), changed.data(), 0, 0) == 0);
	assert(!CardUtilIsCardBusy() && CardUtilResultCode() == 0);
	assert(readSave(0, "fallback") == changed);
	data = pattern(8192, 27);
	assert(CardUtilCommand(0, CARDCMD_Write, number, data.data(), 0, data.size()) == 0);
	assert(readSave(0, "fallback") == data);
	gameflow.mMemoryCard.mDidSaveFail = false; failWrite = writeCalls + 1;
	assert(CardUtilCommand(0, CARDCMD_Save, reinterpret_cast<intptr_t>(&state), changed.data(), 0, 0) == CARD_RESULT_INSSPACE);
	assert(gameflow.mMemoryCard.mDidSaveFail && !CardUtilIsCardBusy());
	resetFaults(); restart(); assert(readSave(0, "fallback") == data);
	puts("Firmware sync routes, unsupported sync, verified staged writes and missing-worker save fallback passed");
}

void actualDefaultFile()
{
	// Create the real game banner and all seven real-format blocks in a fresh
	// virtual card using the production makeDefaultFile and save/check methods.
	std::string previous = storageRoot; storageRoot += "/default-test";
	std::filesystem::create_directory(storageRoot);
	resetFaults(); syncRoute = SyncRoute::KernelUnsupported;
	CARDInit(); CardControl.mChannel = -1; CardControl.mDirentList = nullptr;
	sCardThreadReady = false; gameflow.mMemoryCard = MemoryCard{};
	assert(gameflow.mMemoryCard.getMemoryCardState(false) == 0);
	assert(gameflow.mMemoryCard.makeDefaultFile() == 0);
	assert(gameflow.mMemoryCard.hasCardFinished() && !gameflow.mMemoryCard.didSaveFail());
	CARDInit(); assert(!gameflow.mMemoryCard.isFileBroken());
	assert(gameflow.mMemoryCard.mSaveFileIndex >= 0);
	assert(gameflow.mMemoryCard.getOkSections() == 0x7f);
	assert(gameflow.mMemoryCard.mValidOptionsCount == 2 && gameflow.mMemoryCard.mValidBlockCount == 3);
	resetFaults(); storageRoot = previous; restart();
	puts("Actual default-file creation, banner/icons, seven block checksums and game corruption check passed");
}
void psplinkSave()
{
	std::string previous = storageRoot;
	storageRoot += "/psplink-test";
	std::filesystem::create_directories(storageRoot + "/host/psplink");
	std::filesystem::create_directory(storageRoot + "/ms0");
	launchRoot = "host0:/psplink"; hostWrites = 0;
	rejectStorageRootStat = true;
	resetFaults(); syncRoute = SyncRoute::GenericUnsupported;
	CARDInit(); CardControl.mChannel = -1; sCardThreadReady = false;
	gameflow.mMemoryCard = MemoryCard{};
	assert(gameflow.mMemoryCard.getMemoryCardState(false) == 0);
	assert(gameflow.mMemoryCard.makeDefaultFile() == 0);
	assert(gameflow.mMemoryCard.hasCardFinished() && !gameflow.mMemoryCard.didSaveFail());
	assert(hostWrites > 0);
	assert(std::filesystem::exists(storageRoot + "/host/psplink/saves/card_a/000.pcs"));
	assert(!syncDevices.empty() && syncDevices[0] == "host0:");
	for (const auto& device : syncDevices) assert(device == "host0:");
	CARDInit();
	assert(!gameflow.mMemoryCard.isFileBroken());
	assert(gameflow.mMemoryCard.getOkSections() == 0x7f);
	// Exercise replacement, backup recovery and deletion through HostFS too.
	assert(CARDMount(0, nullptr, nullptr) == 0);
	CARDStat state = saveState(8192); strcpy(state.fileName, "host-recovery"); s32 number;
	auto oldData = pattern(8192, 4), newData = pattern(8192, 9);
	assert(PikminPsp::saveCardFile(0, &state, oldData.data(), &number) == 0);
	assert(PikminPsp::saveCardFile(0, &state, newData.data(), &number) == 0);
	assert(readSave(0, "host-recovery") == newData);
	char path[512]; assert(cardPath(path, sizeof(path), 0, number));
	assert(unlink(nativePath(path).c_str()) == 0);
	restart(); syncRoute = SyncRoute::GenericUnsupported;
	assert(readSave(0, "host-recovery") == oldData);
	assert(CARDFastDelete(0, number) == 0);
	restart(); CARDFileInfo info; assert(CARDOpen(0, "host-recovery", &info) == CARD_RESULT_NOFILE);
	assert(!gameflow.mMemoryCard.isFileBroken());
	rejectStorageRootStat = false;
	launchRoot = "ms0:/PSP/GAME/pikmin"; storageRoot = previous; restart();
	puts("PSPLink card detection, plain -1 unsupported sync, HostFS create/reload/replace/recovery/delete passed");
}
} // namespace

int main(int argc, char** argv)
{
	assert(argc >= 2); storageRoot = argv[1]; setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc > 2) {
		restart(); assert(readSave() == pattern(0x26000, 91));
		puts("Save persisted across a separate process restart"); return 0;
	}
	emptyCardGameCheck(); basicSave(); boundsAndCallbacks(); failureAndRecovery(); capacityAndGameWait(); serializedSaves();
	portableCommitAndWorkerFallback();
	actualDefaultFile();
	psplinkSave();
	puts("PSP card checks passed with LTO, AddressSanitizer and UndefinedBehaviorSanitizer");
}
