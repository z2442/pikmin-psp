#include "port/psp/PspPlatform.h"

#include <pspkernel.h>

PSP_MODULE_INFO("Pikmin PSP Game", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(-1024);

int pikminGameMain(int argc, char** argv);

int main(int argc, char** argv)
{
	if (!PikminPsp::initialise(argc, argv)) return 1;
	int result = pikminGameMain(argc, argv);
	PikminPsp::shutdown();
	return result;
}
