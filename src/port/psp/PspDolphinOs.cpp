#include "Dolphin/os.h"

#include "port/psp/PspPlatform.h"

#include <pspkernel.h>
#include <pspthreadman.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

const u32 kArenaSize = 24 * 1024 * 1024;
u8* sArena;
void* sArenaLo;
void* sArenaHi;
OSThread sMainThread;

struct ThreadSlot {
	OSThread* thread;
	OSThreadStartFunction function;
	void* parameter;
	SceUID id;
	bool started;
};

ThreadSlot sThreads[8];

ThreadSlot* findThreadSlot(OSThread* thread)
{
	for (u32 i = 0; i < ARRAY_SIZE(sThreads); i++) {
		if (sThreads[i].thread == thread) return &sThreads[i];
	}
	return nullptr;
}

int threadEntry(SceSize args, void* argument)
{
	if (args != sizeof(ThreadSlot*) || !argument) return -1;
	ThreadSlot* slot = *static_cast<ThreadSlot**>(argument);
	if (!slot || !slot->function) return -1;
	void* result = slot->function(slot->parameter);
	if (slot->thread) {
		slot->thread->val = result;
		slot->thread->state = OS_THREAD_STATE_MORIBUND;
	}
	return 0;
}

void ensureArena()
{
	if (sArena) return;
	sArena = static_cast<u8*>(memalign(64, kArenaSize));
	if (!sArena) return;
	sArenaLo = sArena;
	sArenaHi = sArena + kArenaSize;
}

} // namespace

extern "C" {

u32 __OSBusClock = 4000000;
u32 __OSCoreClock = 333000000;
volatile OSHeapHandle __OSCurrHeap = -1;
volatile OSContext* __OSCurrentContext = nullptr;
volatile OSContext* __OSFPUContext = nullptr;
OSThreadQueue __OSActiveThreadQueue = {};
OSThread* __OSCurrentThread = &sMainThread;
OSErrorHandler __OSErrorTable[OS_ERROR_MAX] = {};

void OSInit(void)
{
	ensureArena();
	memset(&sMainThread, 0, sizeof(sMainThread));
	sMainThread.state = OS_THREAD_STATE_RUNNING;
}

void* OSGetArenaLo(void) { ensureArena(); return sArenaLo; }
void* OSGetArenaHi(void) { ensureArena(); return sArenaHi; }
void OSSetArenaLo(void* address) { sArenaLo = address; }
void OSSetArenaHi(void* address) { sArenaHi = address; }
void* OSInitAlloc(void* arenaStart, void*, int) { return arenaStart; }

OSTick OSGetTick(void) { return sceKernelGetSystemTimeLow(); }
OSTime OSGetTime(void) { return (OSTime)PikminPsp::timeMicroseconds(); }
OSTime __OSGetSystemTime(void) { return OSGetTime(); }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* output)
{
	if (!output) return;
	memset(output, 0, sizeof(*output));
	output->year = 2000;
	output->mday = 1;
	output->usec = (int)(ticks % 1000);
	output->msec = (int)((ticks / 1000) % 1000);
	output->sec = (int)((ticks / 1000000) % 60);
	output->min = (int)((ticks / 60000000) % 60);
	output->hour = (int)((ticks / 3600000000LL) % 24);
}

void OSReport(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

void OSPanic(const char* file, int line, const char* format, ...)
{
	printf("Pikmin panic at %s:%d: ", file ? file : "?", line);
	va_list arguments;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
	PikminPsp::requestQuit();
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
	if (error >= OS_ERROR_MAX) return nullptr;
	OSErrorHandler previous = __OSErrorTable[error];
	__OSErrorTable[error] = handler;
	return previous;
}

BOOL OSDisableInterrupts(void) { return FALSE; }
BOOL OSRestoreInterrupts(BOOL previous) { return previous; }
BOOL OSEnableInterrupts(void) { return TRUE; }

void DCInvalidateRange(void* address, u32 size) { sceKernelDcacheInvalidateRange(address, size); }
void DCFlushRange(void* address, u32 size) { sceKernelDcacheWritebackInvalidateRange(address, size); }
void DCStoreRange(void* address, u32 size) { sceKernelDcacheWritebackRange(address, size); }
void DCFlushRangeNoSync(void* address, u32 size) { DCFlushRange(address, size); }
void DCStoreRangeNoSync(void* address, u32 size) { DCStoreRange(address, size); }
void DCZeroRange(void* address, u32 size) { memset(address, 0, size); DCStoreRange(address, size); }
void ICInvalidateRange(void* address, u32 size) { sceKernelIcacheInvalidateRange(address, size); }

u32 OSGetStackPointer(void)
{
	u32 stack;
	__asm__ volatile("move %0, $sp" : "=r"(stack));
	return stack;
}

OSThread* OSGetCurrentThread(void)
{
	SceUID id = sceKernelGetThreadId();
	for (u32 i = 0; i < ARRAY_SIZE(sThreads); i++) {
		if (sThreads[i].thread && sThreads[i].id == id) return sThreads[i].thread;
	}
	return &sMainThread;
}

BOOL OSCreateThread(OSThread* thread, OSThreadStartFunction function, void* parameter, void*, u32 stackSize, OSPriority priority, u16 attr)
{
	if (!thread || !function) return FALSE;
	for (u32 i = 0; i < ARRAY_SIZE(sThreads); i++) {
		if (sThreads[i].thread) continue;
		ThreadSlot& slot = sThreads[i];
		slot.thread = thread;
		slot.function = function;
		slot.parameter = parameter;
		slot.started = false;
		memset(thread, 0, sizeof(*thread));
		thread->state = OS_THREAD_STATE_READY;
		thread->priority = priority;
		thread->attr = attr;
		int pspPriority = 0x18 + MIN(MAX(priority, 0), 31);
		slot.id = sceKernelCreateThread("Pikmin worker", threadEntry, pspPriority, MAX(stackSize, 0x4000U), PSP_THREAD_ATTR_USER, nullptr);
		if (slot.id >= 0) return TRUE;
		slot = ThreadSlot {};
		return FALSE;
	}
	return FALSE;
}

s32 OSResumeThread(OSThread* thread)
{
	ThreadSlot* slot = findThreadSlot(thread);
	if (!slot || slot->started) return -1;
	ThreadSlot* argument = slot;
	int result = sceKernelStartThread(slot->id, sizeof(argument), &argument);
	if (result >= 0) {
		slot->started = true;
		thread->state = OS_THREAD_STATE_RUNNING;
	}
	return result;
}

void OSCancelThread(OSThread* thread)
{
	ThreadSlot* slot = findThreadSlot(thread);
	if (!slot) return;
	sceKernelTerminateDeleteThread(slot->id);
	thread->state = OS_THREAD_STATE_MORIBUND;
	*slot = ThreadSlot {};
}

BOOL OSJoinThread(OSThread* thread, void** value)
{
	ThreadSlot* slot = findThreadSlot(thread);
	if (!slot) return FALSE;
	sceKernelWaitThreadEnd(slot->id, nullptr);
	if (value) *value = thread->val;
	sceKernelDeleteThread(slot->id);
	*slot = ThreadSlot {};
	return TRUE;
}

void OSYieldThread(void) { sceKernelDelayThread(0); }
s32 OSCheckActiveThreads(void) { return 0; }

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* messages, s32 count)
{
	if (!queue) return;
	memset(queue, 0, sizeof(*queue));
	queue->msgArray = messages;
	queue->msgCount = count;
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage message, s32 flags)
{
	if (!queue || !queue->msgArray || queue->msgCount <= 0) return FALSE;
	while (queue->usedCount >= queue->msgCount) {
		if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
		sceKernelDelayThread(100);
	}
	int index = (queue->firstIndex + queue->usedCount) % queue->msgCount;
	queue->msgArray[index] = message;
	__sync_synchronize();
	queue->usedCount++;
	return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* queue, OSMessage message, s32 flags)
{
	if (!queue || !queue->msgArray || queue->msgCount <= 0) return FALSE;
	while (queue->usedCount >= queue->msgCount) {
		if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
		sceKernelDelayThread(100);
	}
	queue->firstIndex = (queue->firstIndex + queue->msgCount - 1) % queue->msgCount;
	queue->msgArray[queue->firstIndex] = message;
	__sync_synchronize();
	queue->usedCount++;
	return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* output, s32 flags)
{
	if (!queue || !queue->msgArray || queue->msgCount <= 0) return FALSE;
	while (queue->usedCount <= 0) {
		if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
		sceKernelDelayThread(100);
	}
	if (output) *output = queue->msgArray[queue->firstIndex];
	queue->firstIndex = (queue->firstIndex + 1) % queue->msgCount;
	__sync_synchronize();
	queue->usedCount--;
	return TRUE;
}

void OSInitMutex(OSMutex* mutex)
{
	if (mutex) memset(mutex, 0, sizeof(*mutex));
}

void OSLockMutex(OSMutex* mutex)
{
	if (!mutex) return;
	OSThread* current = OSGetCurrentThread();
	if (mutex->thread == current) {
		mutex->count++;
		return;
	}
	while (!__sync_bool_compare_and_swap(&mutex->thread, nullptr, current)) sceKernelDelayThread(100);
	mutex->count = 1;
}

void OSUnlockMutex(OSMutex* mutex)
{
	if (!mutex || mutex->thread != OSGetCurrentThread()) return;
	if (--mutex->count <= 0) {
		mutex->count = 0;
		__sync_synchronize();
		mutex->thread = nullptr;
	}
}

BOOL OSTryLockMutex(OSMutex* mutex)
{
	if (!mutex) return FALSE;
	OSThread* current = OSGetCurrentThread();
	if (mutex->thread == current || __sync_bool_compare_and_swap(&mutex->thread, nullptr, current)) {
		mutex->count++;
		return TRUE;
	}
	return FALSE;
}

void OSInitCond(OSCond* condition) { if (condition) memset(condition, 0, sizeof(*condition)); }
void OSWaitCond(OSCond*, OSMutex* mutex) { OSUnlockMutex(mutex); sceKernelDelayThread(1000); OSLockMutex(mutex); }
void OSSignalCond(OSCond*) { }

u8 OSGetLanguage(void) { return 0; }
u32 OSGetSoundMode(void) { return 0; }
void OSSetSoundMode(u32) { }
u32 OSGetProgressiveMode(void) { return 1; }
void OSSetProgressiveMode(u32) { }
BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSGetResetButtonState(void) { return FALSE; }
void OSResetSystem(int, u32, BOOL) { PikminPsp::requestQuit(); }
void OSDumpContext(OSContext*) { }

} // extern "C"
