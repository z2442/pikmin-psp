#include "jaudio/bankloader.h"

#include "jaudio/aictrl.h"
#include "jaudio/bankread.h"
#include "jaudio/dvdthread.h"
#include "jaudio/waveread.h"
#if defined(PLATFORM_PSP)
#include "port/psp/PspAudio.h"
#endif

typedef struct BXHeader BXHeader;
typedef struct AddrSize AddrSize;

/**
 * @brief This is an invented type of an unknown name. It represents a file structure.
 *
 * @note Size: 0x10. Based on https://github.com/XAYRGA/JaiSeqX/blob/sxlja-newenv/JaiSeqXLJA/libJAudio/Loaders/JA_BXLoader.cs
 */
struct BXHeader {
	u32 wsysPointerOffset;
	u32 wsysPointerCount;
	u32 ibnkPointerOffset;
	u32 ibnkPointerCount;
};

/**
 * @brief This is an invented type of an unknown name. It represents a file structure.
 *
 * @note Size: 0x8.
 */
struct AddrSize {
	u32 addr;
	u32 size;
};

/**
 * @TODO: Documentation
 */
void Bank_Setup(immut char* filename)
{
	u32 i;

	u8* mem;
	BXHeader* header;
	AddrSize* pairs;
	u32 pointerCount;

	Bank_Init();

#if defined(PLATFORM_PSP)
	mem = (u8*)PikminPsp::audioBankData();
#else
	mem = (u8*)OSAlloc2(DVDT_CheckFile(filename));
	DVDT_LoadFile(filename, mem);
#endif
	header = (BXHeader*)mem;

#if defined(PLATFORM_PSP)
	PikminPsp::audioLog("bank setup: data=%p wsysTable=0x%x wsysCount=%u ibnkTable=0x%x ibnkCount=%u", mem,
	                     (unsigned)header->wsysPointerOffset, (unsigned)header->wsysPointerCount,
	                     (unsigned)header->ibnkPointerOffset, (unsigned)header->ibnkPointerCount);
#endif

	Wavegroup_Init();

	pointerCount = header->wsysPointerCount;
	pairs        = (AddrSize*)(mem + header->wsysPointerOffset);

	for (i = 0; i < pointerCount; ++i) {
		if (pairs[i].size != 0) {
#if defined(PLATFORM_PSP)
			PikminPsp::audioLog("bank setup: WSYS %u begin offset=0x%x size=%u", (unsigned)i,
			                     (unsigned)pairs[i].addr, (unsigned)pairs[i].size);
			BOOL registered = Wavegroup_Regist((void*)(mem + pairs[i].addr), i);
			PikminPsp::audioLog("bank setup: WSYS %u result=%d", (unsigned)i, (int)registered);
#else
			Wavegroup_Regist((void*)(mem + pairs[i].addr), i);
#endif
		}
	}

	pointerCount = header->ibnkPointerCount;
	pairs        = (AddrSize*)(mem + header->ibnkPointerOffset);

	for (i = 0; i < pointerCount; ++i) {
		if (pairs[i].size != 0) {
#if defined(PLATFORM_PSP)
			PikminPsp::audioLog("bank setup: IBNK %u begin offset=0x%x size=%u", (unsigned)i,
			                     (unsigned)pairs[i].addr, (unsigned)pairs[i].size);
			BOOL registered = Bank_Regist((void*)(mem + pairs[i].addr), i);
			PikminPsp::audioLog("bank setup: IBNK %u result=%d", (unsigned)i, (int)registered);
#else
			Bank_Regist((void*)(mem + pairs[i].addr), i);
#endif
		}
	}
}
