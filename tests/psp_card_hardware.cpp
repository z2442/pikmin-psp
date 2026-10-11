// PSPLink hardware check of the production card backend. Results stay in
// memory for PSPLink inspection; this does not add logging to the game.
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include "port/psp/PspCard.h"
#include "port/psp/PspPlatform.h"
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("Pikmin card test", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(64);
PSP_HEAP_SIZE_KB(64);

extern "C" {
volatile s32 cardHardwareResults[16];
u32 __OSBusClock = 4000000;
void OSInitMutex(OSMutex* mutex) { memset(mutex, 0, sizeof(*mutex)); }
void OSLockMutex(OSMutex*) {}
void OSUnlockMutex(OSMutex*) {}
OSTime OSGetTime() { return sceKernelGetSystemTimeWide(); }
DVDDiskID* DVDGetCurrentDiskID()
{
	static DVDDiskID disk = {{'G','P','I','E'}, {'0','1'}, 0, 1, 0, 0, {}};
	return &disk;
}
}
namespace PikminPsp {
const char* executableRoot() { return "host0:/build/psp/checks"; }
bool resolvePath(char* output, u32 size, const char* relative)
{
	int length = snprintf(output, size, "%s/%s", executableRoot(), relative);
	return length >= 0 && (u32)length < size;
}
}
#include "../src/port/psp/PspDolphinCard.cpp"

static u8 payload[0x26000], loaded[0x26000];
int main()
{
	cardHardwareResults[0] = 0x50434152;
	for (u32 i = 0; i < sizeof(payload); ++i) payload[i] = (i * 37 + 91) & 255;
	CARDInit();
	cardHardwareResults[2] = CARDProbe(0);
	cardHardwareResults[3] = CARDMount(0, nullptr, nullptr);
	cardHardwareResults[4] = sceIoSync("host0:", 0);
	CARDStat state = {};
	strcpy(state.fileName, "Pikmin card self-test");
	state.length = sizeof(payload); state.iconAddr = 64; state.commentAddr = 0;
	state.bannerFormat = CARD_STAT_BANNER_C8; state.iconFormat = 5; state.iconSpeed = 15;
	s32 number = -1;
	cardHardwareResults[5] = PikminPsp::saveCardFile(0, &state, payload, &number);
	cardHardwareResults[6] = number;
	if (cardHardwareResults[5] == 0) {
		CARDFileInfo info;
		cardHardwareResults[7] = CARDFastOpen(0, number, &info);
		cardHardwareResults[8] = CARDRead(&info, loaded, sizeof(loaded), 0);
		cardHardwareResults[9] = memcmp(payload, loaded, sizeof(payload));
		CARDClose(&info); CARDUnmount(0); CARDInit();
		cardHardwareResults[10] = CARDMount(0, nullptr, nullptr);
		cardHardwareResults[11] = CARDOpen(0, state.fileName, &info);
		cardHardwareResults[12] = CARDRead(&info, loaded, sizeof(loaded), 0);
		cardHardwareResults[13] = memcmp(payload, loaded, sizeof(payload));
		CARDClose(&info);
		cardHardwareResults[14] = CARDFastDelete(0, number);
	}
	cardHardwareResults[1] = 1;
	// Leave results inspectable without exiting the PSPLink session.
	sceKernelExitDeleteThread(0);
	return 0;
}
