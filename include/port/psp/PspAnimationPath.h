#pragma once

#include "types.h"
#include <stdio.h>
#include <string.h>

namespace PikminPsp {

inline bool animationBundlePath(char* output, u32 capacity, const char* modelPath, const char* bundlePath)
{
	if (!output || !capacity) return false;
	output[0] = '\0';
	if (bundlePath) {
		int length = snprintf(output, capacity, "%s", bundlePath);
		if (length >= 0 && u32(length) < capacity) return true;
		output[0] = '\0';
		return false;
	}
	if (!modelPath) return false;
	const char* name = modelPath;
	for (const char* p = modelPath; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
	const char* extension = strrchr(name, '.');
	const size_t stem = extension && extension != name ? size_t(extension - modelPath) : strlen(modelPath);
	if (!*name || capacity < 5 || stem > capacity - 5) return false;
	memcpy(output, modelPath, stem);
	memcpy(output + stem, ".anm", 5);
	return true;
}

} // namespace PikminPsp
