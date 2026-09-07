#include "ID32.h"

#include "Age.h"
#include "DebugLog.h"
#include "Stream.h"
#include "sysNew.h"

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F0
 */
DEFINE_PRINT("id32");

/**
 * @brief Default constructor.
 */
ID32::ID32()
{
	setID('none');
}

/**
 * @brief Constructs an ID32.
 * @param id Initial ID value.
 */
ID32::ID32(u32 id)
{
	setID(id);
}

/**
 * @brief Sets the ID.
 * @param id ID value to store.
 */
void ID32::setID(u32 id)
{
	mId = id;
	updateString();
}

/**
 * @brief Matches an ID with a wildcard byte.
 * @param id Value to compare.
 * @param wild Wildcard byte to treat as "match any".
 * @return True if all four bytes match (or are wildcards).
 */
bool ID32::match(u32 id, char wild) immut
{
	immut char* other = reinterpret_cast<immut char*>(&id);
	immut char* self  = reinterpret_cast<immut char*>(&mId);

	for (int i = 0; i < 4; i++) {
		if (other[i] != wild && other[i] != self[i]) {
			return false;
		}
	}

	return true;
}

/**
 * @brief Updates mId from mStringID.
 */
void ID32::updateID()
{
#if defined(PLATFORM_PSP)
	mId = (u32(u8(mStringID[0])) << 24) | (u32(u8(mStringID[1])) << 16)
	    | (u32(u8(mStringID[2])) << 8) | u32(u8(mStringID[3]));
#else
	char* id = reinterpret_cast<char*>(&mId);

	for (int i = 0; i < 4; i++) {
		id[i] = this->mStringID[i];
	}
#endif
}

/**
 * @brief Updates mStringID from mId.
 */
void ID32::updateString()
{
#if defined(PLATFORM_PSP)
	sprint(mStringID);
#else
	char* id = reinterpret_cast<char*>(&mId);

	for (int i = 0; i < 4; i++) {
		mStringID[i] = id[i];
	}

	mStringID[4] = 0;
#endif
}

/**
 * @brief Assignment operator.
 * @param other ID value.
 *
 * @note UNUSED Size: 000030
 */
void ID32::operator=(u32 other)
{
	setID(other);
}

/**
 * @brief Equality operator.
 */
bool ID32::operator==(u32 other) immut
{
	return mId == other;
}

/**
 * @brief Inequality operator.
 */
bool ID32::operator!=(u32 other) immut
{
	return mId != other;
}

/**
 * @brief Writes mId as 4 bytes.
 * @param stream Stream to write to.
 */
void ID32::write(RandomAccessStream& stream) immut
{
#if defined(PLATFORM_PSP)
	// IDs use least-significant byte first on disc, unlike stream integers.
	for (int shift = 0; shift < 32; shift += 8) {
		stream.writeByte(u8(mId >> shift));
	}
#else
	immut char* id = reinterpret_cast<immut char*>(&mId);

	stream.writeByte(id[3]);
	stream.writeByte(id[2]);
	stream.writeByte(id[1]);
	stream.writeByte(id[0]);
#endif
}

/**
 * @brief Reads mId and updates mStringID.
 * @param stream Stream to read from.
 */
void ID32::read(RandomAccessStream& stream)
{
#if defined(PLATFORM_PSP)
	mId = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		mId |= u32(stream.readByte()) << shift;
	}
#else
	char* id = reinterpret_cast<char*>(&mId);
	id[3]    = stream.readByte();
	id[2]    = stream.readByte();
	id[1]    = stream.readByte();
	id[0]    = stream.readByte();
#endif

	updateString();
}

/**
 * @brief Prints the ID.
 */
void ID32::print() immut
{
	PRINT("id (%x) is %s\n", mId, mStringID);
}

/**
 * @brief Formats mId into a 4-char string.
 * @param buffer Output buffer (>= 5 bytes).
 */
void ID32::sprint(char* buffer) immut
{
	buffer[0] = (mId & 0xFF000000) >> 24;
	buffer[1] = (mId & 0xFF0000) >> 16;
	buffer[2] = (mId & 0xFF00) >> 8;
	buffer[3] = (mId & 0xFF);
	buffer[4] = 0;
}

#ifdef WIN32

// .dll exclusive function

void ID32::genAge(AgeServer& server, char* name)
{
	server.setOnChange(new Delegate<ID32>(this, ageChangeID));
	server.NewEditor(name, mStringID, 5);
	server.setOnChange((Delegate<ID32>*)nullptr);
}

#endif
