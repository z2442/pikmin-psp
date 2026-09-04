#ifndef PIKMIN_PORT_PSP_RENDERER_H
#define PIKMIN_PORT_PSP_RENDERER_H

#include "types.h"

struct intraFont;

namespace PikminPsp {

class Renderer {
public:
	Renderer();
	bool initialise();
	void shutdown();
	void beginFrame(u32 clearColour);
	void endFrame();

	void rectangle(f32 x, f32 y, f32 width, f32 height, u32 colour);
	void triangle(f32 x0, f32 y0, f32 x1, f32 y1, f32 x2, f32 y2, u32 colour);
	void circle(f32 x, f32 y, f32 radius, u32 colour, int segments = 24);
	void text(f32 x, f32 y, f32 size, u32 colour, const char* value);
	void textCentered(f32 y, f32 size, u32 colour, const char* value);

private:
	intraFont* mFont;
	bool mInitialised;
};

} // namespace PikminPsp

#endif
