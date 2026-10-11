#include "port/psp/PspAnimationPath.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

struct Model { const char* mName; };
struct System {
	Model* mCurrentShape = nullptr;
	std::string bundle;
	u32 calls = 0;
	void loadBundle(const char* path, bool cache) { assert(!cache); bundle = path; ++calls; }
};
static System systemState;
static System* gsys = &systemState;
static u32 reports;
void OSReport(const char*, ...) { ++reports; }

struct Manager {
	Model* mModel;
	void load(const char* bundlePath, int missingAnimCount = 1) {
		char finalBundlePath[PATH_MAX];
		char finalAnimPath[PATH_MAX];
		// This is the actual loadAnims bundle block, including its PSP guard.
#include "bundle_load.inc"
	}
};

static void check(const char* modelPath, const char* bundlePath, const char* expected)
{
	Model model = {modelPath};
	Manager manager = {&model};
	u32 before = systemState.calls;
	manager.load(bundlePath);
	assert(systemState.calls == before + 1 && systemState.bundle == expected);
	assert(systemState.mCurrentShape == &model);
	manager.load(bundlePath, 0);
	assert(systemState.calls == before + 1); // Already-loaded animations skip bundles.
}

int main()
{
	check("cinemas/opening/opening.mod", nullptr, "cinemas/opening/opening.anm");
	check("cinemas/titles/logo.mod", nullptr, "cinemas/titles/logo.anm");
	check("model.mod", "explicit/path.bundle", "explicit/path.bundle");
	check("model.mod", "bundle%08x%%.anm", "bundle%08x%%.anm");
	check("dir.v1/model.mod", nullptr, "dir.v1/model.anm");
	check("dir.v1/model", nullptr, "dir.v1/model.anm");
	check("dir.v1\\model.mod", nullptr, "dir.v1\\model.anm");
	check("model", nullptr, "model.anm");
	check("model.x", nullptr, "model.anm");
	check("model.extension", nullptr, "model.anm");
	check(".model", nullptr, ".model.anm");
	check("percent%path.mod", nullptr, "percent%path.anm");
	std::string stem(PATH_MAX - 5, 'a');
	check((stem + ".mod").c_str(), nullptr, (stem + ".anm").c_str());
	std::string explicitPath(PATH_MAX - 1, 'b');
	check("model.mod", explicitPath.c_str(), explicitPath.c_str());
	for (const auto& modelPath : {stem + "a.mod", std::string(PATH_MAX * 2, 'c'), std::string("dir/")}) {
		Model model = {modelPath.c_str()}; Manager manager = {&model};
		u32 calls = systemState.calls, errors = reports;
		manager.load(nullptr);
		assert(systemState.calls == calls && reports == errors + 1);
	}
	Model model = {nullptr}; Manager manager = {&model};
	u32 calls = systemState.calls;
	manager.load(nullptr);
	assert(systemState.calls == calls);
	explicitPath += 'x';
	manager.load(explicitPath.c_str());
	assert(systemState.calls == calls);
	char output[PATH_MAX];
	assert(!PikminPsp::animationBundlePath(output, 0, "model.mod", nullptr));
	assert(!PikminPsp::animationBundlePath(nullptr, PATH_MAX, "model.mod", nullptr));
	assert(!PikminPsp::animationBundlePath(output, 4, "model.mod", nullptr));
	assert(output[0] == '\0');
	puts("Animation bundle loading: opening/logo paths, literal overrides, extension replacement, capacity and cache-hit checks passed");
}
