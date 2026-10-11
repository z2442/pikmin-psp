#include "jaudio/jammain_2.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

// Compiled directly from the production function by check_psp_audio.py.
#include "interrupt.inc"

static BOOL referenceInterrupt(seqp_* track)
{
	if (track->interruptActive) return FALSE;
	for (int i = 0; i < 8; ++i) {
		u32 mask = 1U << i;
		if ((track->interruptEnable & mask) && (track->pendingInterrupts & mask)) {
			track->savedProgramCounter = track->programCounter;
			track->programCounter = track->interruptAddresses[i];
			track->interruptActive = mask;
			track->savedTimer = track->waitTimer;
			track->waitTimer = 0;
			track->pendingInterrupts ^= mask;
			return TRUE;
		}
	}
	return FALSE;
}

int main()
{
	for (unsigned active : {0U, 1U, 255U})
		for (unsigned enabled = 0; enabled < 256; ++enabled)
			for (unsigned pending = 0; pending < 256; ++pending) {
				seqp_ actual = {};
				actual.interruptActive = active;
				actual.interruptEnable = enabled;
				actual.pendingInterrupts = pending;
				actual.programCounter = 0x12345678;
				actual.savedProgramCounter = 0xabcdef01;
				actual.waitTimer = int(pending) - 128;
				actual.savedTimer = 0x87654321;
				for (unsigned i = 0; i < 8; ++i) actual.interruptAddresses[i] = 1000 + i * 100;
				seqp_ expected = actual;
				assert(Jam_TryInterrupt(&actual) == referenceInterrupt(&expected));
				assert(!memcmp(&actual, &expected, sizeof(actual)));
			}
	puts("Audio interrupt: all 65,536 enabled/pending pairs, active nesting, priority and timer state match the original");
}
