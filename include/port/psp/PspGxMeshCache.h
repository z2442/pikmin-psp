#pragma once
#include "Dolphin/gx.h"

namespace PikminPsp {

// Model arrays are read-only between initMesh and the end of this draw scope.
// Validate their contents once at binding, rather than for every material.
struct MeshValidationScope {
	MeshValidationScope();
	~MeshValidationScope();
	MeshValidationScope(const MeshValidationScope&) = delete;
	MeshValidationScope& operator=(const MeshValidationScope&) = delete;
	MeshValidationScope* previous;
	void* arrays[GX_VA_MAX_ATTR];
	u64 modelRevision;
};
void validateMeshArray(GXAttr attribute, const void* source, u32 bytes);
// A revision is unique across model lifetimes. Callers must replace it after
// editing source arrays or display-list bytes; matrix/material changes stay live.
u64 nextMeshRevision();
void bindMeshRevision(u64 revision);

} // namespace PikminPsp
