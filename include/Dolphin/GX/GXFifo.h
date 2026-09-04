#ifndef _DOLPHIN_GXFIFO_H
#define _DOLPHIN_GXFIFO_H

#include "types.h"

#include "Dolphin/GX/GXEnum.h"
#include "Dolphin/GX/GXTypes.h"

BEGIN_SCOPE_EXTERN_C

/////////////// FIFO STRUCTS ///////////////
#define GX_FIFO_MINSIZE  (64 * 1024)
#define GX_FIFO_OBJ_SIZE (128)

#define GXFIFO_ADDR 0xCC008000

// Generic struct for FIFO access (size 0x80).
typedef struct _GXFifoObj {
	u8 padding[GX_FIFO_OBJ_SIZE]; // _00
} GXFifoObj;

typedef struct __GXFifoObj {
	u8* base;
	u8* top;
	u32 size;
	u32 hiWatermark;
	u32 loWatermark;
	void* rdPtr;
	void* wrPtr;
	s32 count;
	u8 bind_cpu;
	u8 bind_gp;
} __GXFifoObj;

// Internal struct for FIFO access.
typedef struct _GXFifoObjPriv {
	void* base;        // _00
	void* end;         // _04
	u32 size;          // _08
	u32 highWatermark; // _0C
	u32 lowWatermark;  // _10
	void* readPtr;     // _14
	void* writePtr;    // _18
	s32 rwDistance;    // _1C
	u8 _20[0x60];      // _20
} GXFifoObjPriv;

typedef void (*GXBreakPtCallback)(void);

// PPC Write Gather Pipe
typedef union {
	u8 u8;
	u16 u16;
	u32 u32;
	u64 u64;
	s8 s8;
	s16 s16;
	s32 s32;
	s64 s64;
	f32 f32;
	f64 f64;
} PPCWGPipe;

extern volatile PPCWGPipe GXWGFifo AT_ADDRESS(GXFIFO_ADDR);

////////////////////////////////////////////

//////////// FIFO MACROS/INLINES ///////////
#if defined(PLATFORM_PSP)
void PspGXWriteU8(u8 value);
void PspGXWriteU16(u16 value);
void PspGXWriteS16(s16 value);
void PspGXWriteU32(u32 value);
void PspGXWriteF32(f32 value);
void PspGXPosition2f32(f32 x, f32 y);
void PspGXPosition2s16(s16 x, s16 y);
void PspGXPosition2u16(u16 x, u16 y);
void PspGXPosition3s16(s16 x, s16 y, s16 z);
void PspGXPosition3u16(u16 x, u16 y, u16 z);
void PspGXPosition3f32(f32 x, f32 y, f32 z);
void PspGXNormal3f32(f32 x, f32 y, f32 z);
void PspGXColor1u32(u32 colour);
void PspGXColor4u8(u8 r, u8 g, u8 b, u8 a);
void PspGXTexCoord2s8(s8 u, s8 v);
void PspGXTexCoord2u8(u8 u, u8 v);
void PspGXTexCoord2s16(s16 u, s16 v);
void PspGXTexCoord2u16(u16 u, u16 v);
void PspGXTexCoord2f32(f32 u, f32 v);
void PspGXEnd(void);

#define GX_WRITE_U8(val)  PspGXWriteU8((u8)(val))
#define GX_WRITE_U16(val) PspGXWriteU16((u16)(val))
#define GX_WRITE_S16(val) PspGXWriteS16((s16)(val))
#define GX_WRITE_U32(val) PspGXWriteU32((u32)(val))
#define GX_WRITE_F32(val) PspGXWriteF32((f32)(val))
#else
#define GX_WRITE_U8(val)  (GXWGFifo.u8 = val)
#define GX_WRITE_U16(val) (GXWGFifo.u16 = val)
#define GX_WRITE_S16(val) (GXWGFifo.s16 = val)
#define GX_WRITE_U32(val) (GXWGFifo.u32 = (u32)val)
#define GX_WRITE_F32(val) (GXWGFifo.f32 = (f32)val)
#endif

static inline void GXPosition2f32(const f32 x, const f32 y)
{
#if defined(PLATFORM_PSP)
	PspGXPosition2f32(x, y);
#else
	GXWGFifo.f32 = x;
	GXWGFifo.f32 = y;
#endif
}

static inline void GXPosition3s16(const s16 x, const s16 y, const s16 z)
{
#if defined(PLATFORM_PSP)
	PspGXPosition3s16(x, y, z);
#else
	GXWGFifo.s16 = x;
	GXWGFifo.s16 = y;
	GXWGFifo.s16 = z;
#endif
}

static inline void GXPosition3u16(const u16 x, const u16 y, const u16 z)
{
#if defined(PLATFORM_PSP)
	PspGXPosition3u16(x, y, z);
#else
	GXWGFifo.u16 = x;
	GXWGFifo.u16 = y;
	GXWGFifo.u16 = z;
#endif
}

static inline void GXPosition3f32(f32 x, f32 y, f32 z)
{
#if defined(PLATFORM_PSP)
	PspGXPosition3f32(x, y, z);
#else
	GXWGFifo.f32 = x;
	GXWGFifo.f32 = y;
	GXWGFifo.f32 = z;
#endif
}

static inline void GXNormal3f32(const f32 x, const f32 y, const f32 z)
{
#if defined(PLATFORM_PSP)
	PspGXNormal3f32(x, y, z);
#else
	GXWGFifo.f32 = x;
	GXWGFifo.f32 = y;
	GXWGFifo.f32 = z;
#endif
}

static inline void GXColor1u32(u32 c)
{
#if defined(PLATFORM_PSP)
	PspGXColor1u32(c);
#else
	GXWGFifo.u32 = c;
#endif
}

static inline void GXColor4u8(const u8 r, const u8 g, const u8 b, const u8 a)
{
#if defined(PLATFORM_PSP)
	PspGXColor4u8(r, g, b, a);
#else
	GXWGFifo.u8 = r;
	GXWGFifo.u8 = g;
	GXWGFifo.u8 = b;
	GXWGFifo.u8 = a;
#endif
}

static inline void GXTexCoord2s8(const s8 u, const s8 v)
{
#if defined(PLATFORM_PSP)
	PspGXTexCoord2s8(u, v);
#else
	GXWGFifo.s8 = u;
	GXWGFifo.s8 = v;
#endif
}

static inline void GXTexCoord2u8(u8 s, u8 t)
{
#if defined(PLATFORM_PSP)
	PspGXTexCoord2u8(s, t);
#else
	GXWGFifo.u8 = s;
	GXWGFifo.u8 = t;
#endif
}

static inline void GXPosition2u16(u16 x, u16 y)
{
#if defined(PLATFORM_PSP)
	PspGXPosition2u16(x, y);
#else
	GXWGFifo.u16 = x;
	GXWGFifo.u16 = y;
#endif
}

static inline void GXPosition2s16(s16 x, s16 y)
{
#if defined(PLATFORM_PSP)
	PspGXPosition2s16(x, y);
#else
	GXWGFifo.s16 = x;
	GXWGFifo.s16 = y;
#endif
}

static inline void GXTexCoord2s16(const s16 u, const s16 v)
{
#if defined(PLATFORM_PSP)
	PspGXTexCoord2s16(u, v);
#else
	GXWGFifo.s16 = u;
	GXWGFifo.s16 = v;
#endif
}

static inline void GXTexCoord2u16(const u16 u, const u16 v)
{
#if defined(PLATFORM_PSP)
	PspGXTexCoord2u16(u, v);
#else
	GXWGFifo.u16 = u;
	GXWGFifo.u16 = v;
#endif
}

static inline void GXTexCoord2f32(const f32 u, const f32 v)
{
#if defined(PLATFORM_PSP)
	PspGXTexCoord2f32(u, v);
#else
	GXWGFifo.f32 = u;
	GXWGFifo.f32 = v;
#endif
}

static inline void GXEnd(void)
{
#if defined(PLATFORM_PSP)
	PspGXEnd();
#endif
}

////////////////////////////////////////////

//////////// FIFO INIT/SET/SAVE ////////////
// Init.
extern void __GXFifoInit();
extern void GXInitFifoBase(GXFifoObj* obj, void* base, u32 size);
extern void GXInitFifoPtrs(GXFifoObj* obj, void* readPtr, void* writePtr);
extern void GXInitFifoLimits(GXFifoObj* obj, u32 hiWaterMark, u32 loWaterMark);

// Set.
extern void GXSetCPUFifo(GXFifoObj* obj);
extern void GXSetGPFifo(GXFifoObj* obj);
extern void GXSaveCPUFifo(GXFifoObj* obj);

////////////////////////////////////////////

/////////////// FIFO GETTERS ///////////////
extern void GXGetGPStatus(GXBool* isOverHi, GXBool* isUnderLo, GXBool* isReadIdle, GXBool* isCmdIdle, GXBool* isHitBrkPt);
extern GXFifoObj* GXGetCPUFifo();
extern GXFifoObj* GXGetGPFifo();

////////////////////////////////////////////

//////////// DISPLAY LIST FUNCS ////////////
extern void GXBeginDisplayList(void* list, u32 size);
extern u32 GXEndDisplayList();
extern void GXCallDisplayList(void* list, u32 numBytes);

////////////////////////////////////////////

///////////// BREAKPOINT FUNCS /////////////
extern GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback callback);

////////////////////////////////////////////

/////////////// OTHER FUNCS ////////////////
void __GXSaveCPUFifoAux(__GXFifoObj* obj);
void __GXFifoReadEnable();
void __GXFifoReadDisable();
void __GXFifoLink(u8);
void __GXWriteFifoIntEnable(u8, u8);
void __GXWriteFifoIntReset(u8, u8);
void __GXCleanGPFifo(void);

// Unused/inlined in P2.
extern void GXSaveGPFifo(GXFifoObj* obj);

extern void GXGetFifoStatus(GXFifoObj* obj, GXBool* isOverHi, GXBool* isUnderLo, u32* fifoCount, GXBool* isCpuWrite, GXBool* isGPRead,
                            GXBool* isFifoWrap);
extern void GXGetFifoPtrs(GXFifoObj* obj, void** readPtr, void** writePtr);
extern void* GXGetFifoBase(GXFifoObj* obj);
extern u32 GXGetFifoSize(GXFifoObj* obj);
extern void GXGetFifoLimits(GXFifoObj* obj, u32* hi, u32* lo);

extern void GXEnableBreakPt(void* breakPtr);
extern void GXDisableBreakPt();

////////////////////////////////////////////

END_SCOPE_EXTERN_C

#endif
