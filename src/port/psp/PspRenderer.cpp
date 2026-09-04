#include "port/psp/PspRenderer.h"

#include <intraFont.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>

#include <math.h>

namespace {

const int kBufferWidth  = 512;
const int kScreenWidth  = 480;
const int kScreenHeight = 272;
const int kFrameBytes   = kBufferWidth * kScreenHeight * 4;

u32 sDisplayList[262144] __attribute__((aligned(64)));

struct ColourVertex {
	u32 colour;
	f32 x;
	f32 y;
	f32 z;
};

void set2DState()
{
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

} // namespace

namespace PikminPsp {

Renderer::Renderer()
    : mFont(nullptr)
    , mInitialised(false)
{
}

bool Renderer::initialise()
{
	if (mInitialised) {
		return true;
	}

	sceGuInit();
	sceGuStart(GU_DIRECT, sDisplayList);
	sceGuDrawBuffer(GU_PSM_8888, (void*)0, kBufferWidth);
	sceGuDispBuffer(kScreenWidth, kScreenHeight, (void*)kFrameBytes, kBufferWidth);
	sceGuDepthBuffer((void*)(kFrameBytes * 2), kBufferWidth);
	sceGuOffset(2048 - (kScreenWidth / 2), 2048 - (kScreenHeight / 2));
	sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuShadeModel(GU_SMOOTH);
	sceGuFrontFace(GU_CW);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);

	if (intraFontInit()) {
		mFont = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
	}
	mInitialised = true;
	return true;
}

void Renderer::shutdown()
{
	if (!mInitialised) {
		return;
	}
	if (mFont) {
		intraFontUnload(mFont);
		mFont = nullptr;
	}
	intraFontShutdown();
	sceGuTerm();
	mInitialised = false;
}

void Renderer::beginFrame(u32 clearColour)
{
	sceGuStart(GU_DIRECT, sDisplayList);
	sceGuClearColor(clearColour);
	sceGuClearDepth(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	set2DState();
}

void Renderer::endFrame()
{
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sceDisplayWaitVblankStart();
	sceGuSwapBuffers();
}

void Renderer::rectangle(f32 x, f32 y, f32 width, f32 height, u32 colour)
{
	ColourVertex* vertices = static_cast<ColourVertex*>(sceGuGetMemory(sizeof(ColourVertex) * 2));
	vertices[0].colour = colour;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0.0f;
	vertices[1].colour = colour;
	vertices[1].x = x + width;
	vertices[1].y = y + height;
	vertices[1].z = 0.0f;
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr, vertices);
}

void Renderer::triangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, u32 colour)
{
	ColourVertex* vertices = static_cast<ColourVertex*>(sceGuGetMemory(sizeof(ColourVertex) * 3));
	vertices[0] = ColourVertex { colour, x0, y0, 0.0f };
	vertices[1] = ColourVertex { colour, x1, y1, 0.0f };
	vertices[2] = ColourVertex { colour, x2, y2, 0.0f };
	sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 3, nullptr, vertices);
}

void Renderer::circle(f32 x, f32 y, f32 radius, u32 colour, int segments)
{
	if (segments < 3) {
		segments = 3;
	}
	ColourVertex* vertices = static_cast<ColourVertex*>(sceGuGetMemory(sizeof(ColourVertex) * (segments + 2)));
	vertices[0] = ColourVertex { colour, x, y, 0.0f };
	for (int i = 0; i <= segments; i++) {
		f32 angle = (6.28318530718f * i) / segments;
		vertices[i + 1] = ColourVertex { colour, x + cosf(angle) * radius, y + sinf(angle) * radius, 0.0f };
	}
	sceGuDrawArray(GU_TRIANGLE_FAN, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, segments + 2, nullptr, vertices);
}

void Renderer::text(f32 x, f32 y, f32 size, u32 colour, const char* value)
{
	if (!mFont || !value) {
		return;
	}
	intraFontSetStyle(mFont, size, colour, GU_RGBA(0, 0, 0, 180), 0.0f, INTRAFONT_ALIGN_LEFT);
	intraFontPrint(mFont, x, y, value);
}

void Renderer::textCentered(f32 y, f32 size, u32 colour, const char* value)
{
	if (!mFont || !value) {
		return;
	}
	intraFontSetStyle(mFont, size, colour, GU_RGBA(0, 0, 0, 180), 0.0f, INTRAFONT_ALIGN_CENTER);
	intraFontPrint(mFont, kScreenWidth * 0.5f, y, value);
}

} // namespace PikminPsp
