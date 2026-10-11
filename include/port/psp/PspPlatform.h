#ifndef PIKMIN_PORT_PSP_PLATFORM_H
#define PIKMIN_PORT_PSP_PLATFORM_H

#include "types.h"

struct PspInputState {
	u32 buttons;
	u32 pressed;
	u32 released;
	f32 stickX;
	f32 stickY;
};

namespace PikminPsp {

bool initialise(int argc, char** argv);
void shutdown();
void pollInput(PspInputState& input);
bool shouldQuit();
void requestQuit();
u64 timeMicroseconds();

const char* executableRoot();
bool resolvePath(char* output, u32 outputSize, const char* relativePath);
bool dataReady();

// Presentation uses three color buffers. A missed frame need not wait for
// another vblank, but a fast frame must let the prior display swap latch.
void setPresentationInterval(u32 refreshes);
u32 waitForFrameRetrace(u32 previousRetrace, u32 refreshes);

} // namespace PikminPsp

#endif
