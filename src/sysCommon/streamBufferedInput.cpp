#include "Stream.h"

#include "DebugLog.h"
#include "Dolphin/os.h"
#include "sysNew.h"
#include "system.h"
#include <string.h>

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("streamBufferedInput");

/**
 * @todo: Documentation
 */
void BufferedInputStream::init(Stream* stream, u8* buffer, int bufferSize)
{
	mPath             = StdSystem::stringDup(stream->mPath);
	mBufferSize       = bufferSize;
	mBuffer           = buffer ? buffer : new (0x20) u8[mBufferSize];
	mPosition         = 0;
	mCurrentBufferPos = 0;
	mRemainingBytes   = 0;
	mStream           = stream;
	fillBuffer();
}

/**
 * @todo: Documentation
 */
BufferedInputStream::BufferedInputStream(Stream* stream, u8* buffer, int bufferSize)
{
	init(stream, buffer, bufferSize);
}

/**
 * @todo: Documentation
 */
void BufferedInputStream::fillBuffer()
{
	if (mRemainingBytes - mCurrentBufferPos != 0) {
		return;
	}

	mRemainingBytes = getPending();

	if (mRemainingBytes > mBufferSize) {
		mRemainingBytes = mBufferSize;
	}

	mStream->read(mBuffer, mRemainingBytes);
	mCurrentBufferPos = 0;
}

/**
 * @todo: Documentation
 */
void BufferedInputStream::read(void* input, int size)
{
	u8* buf = static_cast<u8*>(input);
	while (size != 0) {
		fillBuffer();
		int diff     = mRemainingBytes - mCurrentBufferPos;
		if (diff <= 0) {
#if defined(PLATFORM_PSP)
			// Some disc resources rely on the zero padding following their
			// logical EOF.  The original DVD layer could satisfy that aligned
			// read, while a normal PSP file ends exactly at its reported size.
			// Returning padding also guarantees forward progress instead of
			// repeatedly issuing a zero-byte sceIoRead forever.
			OSReport("Pikmin PSP: zero-padding read past EOF in %s (%d bytes)\n", mPath ? mPath : "<unknown>", size);
			memset(buf, 0, size);
			mPosition += size;
			return;
#else
			return;
#endif
		}
		int copySize = size;
		if (copySize > diff) {
			copySize = diff;
		}
		memcpy(buf, mBuffer + mCurrentBufferPos, copySize);
		buf += copySize;
		size -= copySize;
		mCurrentBufferPos += copySize;
		mPosition += copySize;
	}
}
