#pragma once
#include "types.h"

namespace PikminPsp {
void movieDraw();
void movieMix(s16* output, u32 frames); // Called with AudioLock held.
void movieShutdown();
void drawMovieFrame(const u16* rgb565, u32 width, u32 height, u32 stride);
}
