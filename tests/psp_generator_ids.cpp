// Exercise the original ID and generator helpers on a little-endian host.
#include <math.h>
namespace std { using ::sqrtf; }
#include "../src/plugPikiKando/generator.cpp"
#define _Print ID32_Print
#define _Error ID32_Error
#include "../src/sysCommon/id32.cpp"
#undef _Print
#undef _Error
#define _Print Route_Print
#define _Error Route_Error
#include "../src/sysCommon/shpRoutes.cpp"
#undef _Print
#undef _Error
#define _Print RouteMgr_Print
#define _Error RouteMgr_Error
#include "../src/plugPikiKando/routeMgr.cpp"
#undef _Print
#undef _Error
#define _Print Node_Print
#define _Error Node_Error
#include "../src/sysCommon/node.cpp"
#undef _Print
#undef _Error
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void* System::alloc(size_t size) { return malloc(size); }
// Rendering and asynchronous path search are outside this lookup regression.
System* gsys = nullptr;
f32 qdist2(f32, f32, f32, f32) { assert(false); return 0.0f; }
#include "../src/sysCommon/stream.cpp"
#include "../src/sysCommon/string.cpp"

class TestStream : public RandomAccessStream {
public:
	u8 bytes[64] = {};
	size_t length = 0;
	size_t position = 0;
	void read(void* dst, int size) override
	{
		assert(size >= 0 && position + size <= length);
		memcpy(dst, bytes + position, size);
		position += size;
	}
	void write(const void* src, int size) override
	{
		const u8* p = static_cast<const u8*>(src);
		assert(length + size <= sizeof(bytes));
		memcpy(bytes + length, p, size);
		length += size;
	}
};

class TestRouteMgr : public RouteMgr {
public:
	void setGroup(u32* id, Group* group)
	{
		mRouteCount = 1;
		mRouteGroupIDs = id;
		mGroupList = group;
	}
};

int main(int argc, char** argv)
{
	// The Onion looks up 'test' after RouteGroup::loadini copies its name.
	RouteGroup route;
	route.setID('test');
	assert(strcmp(route.mStringID, "test") == 0);
	memcpy(route.mStringID, "test", 5);
	route.updateID();
	assert(route.mIntID == 'test');
	WayPoint points[2];
	for (int i = 0; i < 2; ++i) {
		points[i].mPosition.set(i * 100.0f, 0.0f, 0.0f);
		points[i].mIndex = i;
		points[i].mIsOpen = true;
		points[i].mFlags = 0;
	}
	RouteMgr::Group group;
	group.mWayPoints = points;
	group.mNumPoints = 2;
	TestRouteMgr routes;
	routes.setGroup(&route.mIntID, &group);
	WayPoint* nearest = routes.findNearestWayPoint('test', Vector3f(90.0f, 0.0f, 0.0f), false);
	assert(nearest == &points[1]);
	assert(nearest->mIndex == 1);

	ID32 id('v0.1');
	assert(strcmp(id.mStringID, "v0.1") == 0);
	memcpy(id.mStringID, "item", 5);
	id.updateID();
	assert(id == 'item');
	assert(id.match('it**'));
	assert(!id.match('te**'));
	TestStream stream;
	id.write(stream);
	assert(stream.length == 4 && memcmp(stream.bytes, "meti", 4) == 0);
	ID32 loaded;
	loaded.read(stream);
	assert(loaded == 'item');
	assert(strcmp(loaded.mStringID, "item") == 0);

	TestStream helper;
	writeID(helper, 'v0.1');
	assert(helper.length == 4 && memcmp(helper.bytes, "1.0v", 4) == 0);
	assert(readID(helper) == 'v0.1');

	// GeneratorMgr::read conditionally consumes the direction for v0.1.
	// Before the fix it reads 180.0f's bits (1127481344) as the count.
	TestStream header;
	const u8 fixture[] = {'1', '.', '0', 'v', 0, 0, 0, 0, 0, 0, 0, 0,
	                0, 0, 0, 0, 0x43, 0x34, 0, 0, 0, 0, 0, 4};
	header.write(fixture, sizeof(fixture));
	for (int i = 0; i < argc; ++i) {
		if (i) {
			FILE* file = fopen(argv[i], "rb");
			assert(file);
			header.length = 24;
			assert(fread(header.bytes, 1, 24, file) == 24);
			fclose(file);
		}
		header.position = 0;
		ID32 version;
		version.read(header);
		assert(version == 'v0.1');
		for (int axis = 0; axis < 3; ++axis) assert(isfinite(header.readFloat()));
		if (version == 'v0.1') assert(header.readFloat() == 180.0f);
		assert(header.readInt() == 4);
		assert(header.position == 24);
	}
	puts("PSP route lookup, generator ID and first-area header checks passed");
}
