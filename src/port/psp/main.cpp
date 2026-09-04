#include "port/psp/PspPlatform.h"
#include "port/psp/PspRenderer.h"

#include "Dolphin/gx.h"

#include <pspctrl.h>
#include <pspgu.h>
#include <pspkernel.h>

#include <math.h>
#include <stdio.h>

PSP_MODULE_INFO("Pikmin PSP Port", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(-1024);

namespace {

void drawPikmin(PikminPsp::Renderer& renderer, f32 x, f32 y, f32 scale, u32 bodyColour, f32 phase)
{
	f32 sway = sinf(phase) * 3.0f * scale;
	renderer.rectangle(x - scale, y - 31.0f * scale, scale * 2.0f, scale * 17.0f, GU_RGBA(58, 145, 54, 255));
	renderer.circle(x + sway + 5.0f * scale, y - 35.0f * scale, 7.0f * scale, GU_RGBA(91, 190, 70, 255), 18);
	renderer.circle(x, y - 7.0f * scale, 12.0f * scale, bodyColour, 22);
	renderer.triangle(x - 8.0f * scale, y - 3.0f * scale, x + 8.0f * scale, y - 3.0f * scale, x, y + 23.0f * scale,
	                  bodyColour);
	renderer.circle(x - 4.0f * scale, y - 9.0f * scale, 2.2f * scale, GU_RGBA(255, 255, 255, 255), 12);
	renderer.circle(x + 4.0f * scale, y - 9.0f * scale, 2.2f * scale, GU_RGBA(255, 255, 255, 255), 12);
}

void drawGxBackendBadge()
{
	// This is intentionally submitted through Pikmin's original GX-facing API.
	// It exercises the GX-to-GU immediate path in every bootstrap frame.
	GXSetNumTexGens(0);
	GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	GXPosition2f32(438.0f, 21.0f);
	GXColor4u8(126, 210, 117, 220);
	GXPosition2f32(461.0f, 21.0f);
	GXColor4u8(126, 210, 117, 220);
	GXPosition2f32(449.5f, 41.0f);
	GXColor4u8(126, 210, 117, 220);
	GXEnd();
}

} // namespace

int main(int argc, char** argv)
{
	if (!PikminPsp::initialise(argc, argv)) {
		return 1;
	}

	PikminPsp::Renderer renderer;
	if (!renderer.initialise()) {
		PikminPsp::shutdown();
		return 1;
	}

	const bool haveData = PikminPsp::dataReady();
	const u64 startTime = PikminPsp::timeMicroseconds();
	PspInputState input = {};

	while (!PikminPsp::shouldQuit()) {
		PikminPsp::pollInput(input);
		if (input.pressed & PSP_CTRL_CIRCLE) {
			PikminPsp::requestQuit();
		}

		f32 time = (PikminPsp::timeMicroseconds() - startTime) / 1000000.0f;
		renderer.beginFrame(GU_RGBA(13, 25, 31, 255));
		renderer.rectangle(0.0f, 205.0f, 480.0f, 67.0f, GU_RGBA(29, 61, 42, 255));
		renderer.rectangle(0.0f, 221.0f, 480.0f, 51.0f, GU_RGBA(43, 82, 49, 255));
		drawGxBackendBadge();

		drawPikmin(renderer, 176.0f + input.stickX * 18.0f, 214.0f - input.stickY * 10.0f, 1.0f,
		           GU_RGBA(225, 54, 49, 255), time * 2.2f);
		drawPikmin(renderer, 240.0f, 211.0f, 1.12f, GU_RGBA(64, 111, 230, 255), time * 2.0f + 1.8f);
		drawPikmin(renderer, 306.0f, 214.0f, 0.96f, GU_RGBA(237, 213, 55, 255), time * 2.4f + 3.5f);

		renderer.textCentered(42.0f, 1.35f, GU_RGBA(236, 244, 226, 255), "PIKMIN - PSP PORT");
		renderer.textCentered(69.0f, 0.72f, GU_RGBA(167, 198, 177, 255), "GPIE01_01  |  VIDEO / INPUT BOOTSTRAP");

		if (haveData) {
			renderer.rectangle(91.0f, 91.0f, 298.0f, 37.0f, GU_RGBA(23, 91, 51, 230));
			renderer.textCentered(115.0f, 0.72f, GU_RGBA(193, 255, 197, 255), "GAME DATA FOUND");
		} else {
			renderer.rectangle(64.0f, 91.0f, 352.0f, 52.0f, GU_RGBA(105, 60, 22, 230));
			renderer.textCentered(112.0f, 0.68f, GU_RGBA(255, 222, 162, 255), "WAITING FOR EXTRACTED GAME DATA");
			renderer.textCentered(132.0f, 0.49f, GU_RGBA(225, 209, 181, 255), "COPY dataDir TO data/dataDir BESIDE EBOOT.PBP");
		}

		char status[128];
		snprintf(status, sizeof(status), "ANALOG  X %+1.2f   Y %+1.2f", input.stickX, input.stickY);
		renderer.textCentered(166.0f, 0.52f, GU_RGBA(183, 202, 209, 255), status);
		renderer.textCentered(188.0f, 0.48f, GU_RGBA(141, 160, 167, 255), "CIRCLE: EXIT");
		renderer.endFrame();
	}

	renderer.shutdown();
	PikminPsp::shutdown();
	return 0;
}
