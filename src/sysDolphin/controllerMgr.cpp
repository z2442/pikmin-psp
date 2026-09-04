#include "Controller.h"

#include "DebugLog.h"
#if defined(PLATFORM_PSP)
#include <pspctrl.h>
#else
#include "Dolphin/pad.h"
#endif

/**
 * @todo: Documentation
 * @note UNUSED Size: 00009C
 */
DEFINE_ERROR(__LINE__) // Never used in the DLL

/**
 * @todo: Documentation
 * @note UNUSED Size: 0000F4
 */
DEFINE_PRINT("ControllerMgr");

#if defined(PLATFORM_PSP)
static SceCtrlData sPspPad;
static u32 sPspKeyStatus;
#else
static PADStatus sControllerPad[PAD_MAX_CONTROLLERS];
static u32 padChannels[PAD_CHANMAX] = { PAD_CHAN0_BIT, PAD_CHAN1_BIT, PAD_CHAN2_BIT, PAD_CHAN3_BIT };
#endif

/**
 * @todo: Documentation
 */
void ControllerMgr::update()
{
#if defined(PLATFORM_PSP)
	sceCtrlPeekBufferPositive(&sPspPad, 1);
	sPspKeyStatus = 0;

	if (sPspPad.Buttons & PSP_CTRL_CROSS)    sPspKeyStatus |= KBBTN_A;
	if (sPspPad.Buttons & PSP_CTRL_CIRCLE)   sPspKeyStatus |= KBBTN_B;
	if (sPspPad.Buttons & PSP_CTRL_SQUARE)   sPspKeyStatus |= KBBTN_X;
	if (sPspPad.Buttons & PSP_CTRL_TRIANGLE) sPspKeyStatus |= KBBTN_Y;
	if (sPspPad.Buttons & PSP_CTRL_LTRIGGER) sPspKeyStatus |= KBBTN_L;
	if (sPspPad.Buttons & PSP_CTRL_RTRIGGER) sPspKeyStatus |= KBBTN_R;
	if (sPspPad.Buttons & PSP_CTRL_START)    sPspKeyStatus |= KBBTN_START;

	const bool cStick = (sPspPad.Buttons & PSP_CTRL_SELECT) != 0;
	if (cStick) {
		if (sPspPad.Buttons & PSP_CTRL_LEFT)  sPspKeyStatus |= KBBTN_CSTICK_LEFT;
		if (sPspPad.Buttons & PSP_CTRL_RIGHT) sPspKeyStatus |= KBBTN_CSTICK_RIGHT;
		if (sPspPad.Buttons & PSP_CTRL_UP)    sPspKeyStatus |= KBBTN_CSTICK_UP;
		if (sPspPad.Buttons & PSP_CTRL_DOWN)  sPspKeyStatus |= KBBTN_CSTICK_DOWN;
		if (sPspPad.Buttons & PSP_CTRL_RTRIGGER) sPspKeyStatus |= KBBTN_Z;
	} else {
		if (sPspPad.Buttons & PSP_CTRL_LEFT)  sPspKeyStatus |= KBBTN_DPAD_LEFT;
		if (sPspPad.Buttons & PSP_CTRL_RIGHT) sPspKeyStatus |= KBBTN_DPAD_RIGHT;
		if (sPspPad.Buttons & PSP_CTRL_UP)    sPspKeyStatus |= KBBTN_DPAD_UP;
		if (sPspPad.Buttons & PSP_CTRL_DOWN)  sPspKeyStatus |= KBBTN_DPAD_DOWN;
	}

	s8 stickX = static_cast<s8>(static_cast<int>(sPspPad.Lx) - 128);
	s8 stickY = static_cast<s8>(127 - static_cast<int>(sPspPad.Ly));
	if (stickX < -32) sPspKeyStatus |= KBBTN_MSTICK_LEFT;
	if (stickX > 32)  sPspKeyStatus |= KBBTN_MSTICK_RIGHT;
	if (stickY < -32) sPspKeyStatus |= KBBTN_MSTICK_DOWN;
	if (stickY > 32)  sPspKeyStatus |= KBBTN_MSTICK_UP;
#else
	PADRead(sControllerPad);
	PADClamp(sControllerPad);
	u32 padMask = 0;
	for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) {
		if (sControllerPad[i].err == PAD_ERR_NO_CONTROLLER) {
			padMask |= padChannels[i];
		}
	}
	if (padMask) {
		PADReset(padMask);
	}
#endif
}

/**
 * @todo: Documentation
 */
bool ControllerMgr::keyDown(int btn)
{
#if defined(PLATFORM_PSP)
	return (sPspKeyStatus & btn) != 0;
#else
	return (sControllerPad[0].button & btn);
#endif
}

/**
 * @todo: Documentation
 */
void ControllerMgr::init()
{
#if defined(PLATFORM_PSP)
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	sPspKeyStatus = 0;
#else
	PADSetSpec(PAD_SPEC_5);
	PADInit();
#endif
}

/**
 * @todo: Documentation
 */
void ControllerMgr::updateController(Controller* controller)
{
#if defined(PLATFORM_PSP)
	// Pikmin only creates player one in normal play.  Extra GameCube ports have
	// no PSP equivalent and remain disconnected.
	if (controller->mPlayerNum != 1) {
		controller->mMainStickX = controller->mMainStickY = 0;
		controller->mSubStickX = controller->mSubStickY = 0;
		controller->mAnalogA = controller->mAnalogB = 0;
		controller->mTriggerL = controller->mTriggerR = 0;
		controller->updateCont(0);
		return;
	}

	controller->mMainStickX = static_cast<s8>(static_cast<int>(sPspPad.Lx) - 128);
	controller->mMainStickY = static_cast<s8>(127 - static_cast<int>(sPspPad.Ly));
	controller->mSubStickX  = 0;
	controller->mSubStickY  = 0;
	if (sPspPad.Buttons & PSP_CTRL_SELECT) {
		controller->mSubStickX = (sPspPad.Buttons & PSP_CTRL_LEFT) ? -74 : (sPspPad.Buttons & PSP_CTRL_RIGHT) ? 74 : 0;
		controller->mSubStickY = (sPspPad.Buttons & PSP_CTRL_DOWN) ? -74 : (sPspPad.Buttons & PSP_CTRL_UP) ? 74 : 0;
	}
	controller->mAnalogA  = (sPspPad.Buttons & PSP_CTRL_CROSS) ? 255 : 0;
	controller->mAnalogB  = (sPspPad.Buttons & PSP_CTRL_CIRCLE) ? 255 : 0;
	controller->mTriggerL = (sPspPad.Buttons & PSP_CTRL_LTRIGGER) ? 255 : 0;
	controller->mTriggerR = (sPspPad.Buttons & PSP_CTRL_RTRIGGER) ? 255 : 0;
	controller->updateCont(sPspKeyStatus);
#else
	// import stick values from controller
	controller->mMainStickX = sControllerPad[controller->mPlayerNum - 1].stickX;
	controller->mMainStickY = sControllerPad[controller->mPlayerNum - 1].stickY;
	controller->mSubStickX  = sControllerPad[controller->mPlayerNum - 1].substickX;
	controller->mSubStickY  = sControllerPad[controller->mPlayerNum - 1].substickY;

	u32 keyStatus = 0;

	// A B X Y
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_A) {
		keyStatus |= KBBTN_A;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_B) {
		keyStatus |= KBBTN_B;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_X) {
		keyStatus |= KBBTN_X;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_Y) {
		keyStatus |= KBBTN_Y;
	}

	// TRIGGERS (Z, R, L)
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_TRIGGER_Z) {
		keyStatus |= KBBTN_Z;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_TRIGGER_R) {
		keyStatus |= KBBTN_R;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_TRIGGER_L) {
		keyStatus |= KBBTN_L;
	}

	// STICKS (THRESHOLD IS +/- 32, RANGE IS [-128, 127])
	if (sControllerPad[controller->mPlayerNum - 1].stickX < -PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_MSTICK_LEFT;
	}
	if (sControllerPad[controller->mPlayerNum - 1].stickX > PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_MSTICK_RIGHT;
	}
	if (sControllerPad[controller->mPlayerNum - 1].stickY < -PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_MSTICK_DOWN;
	}
	if (sControllerPad[controller->mPlayerNum - 1].stickY > PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_MSTICK_UP;
	}
	if (sControllerPad[controller->mPlayerNum - 1].substickX < -PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_CSTICK_LEFT;
	}
	if (sControllerPad[controller->mPlayerNum - 1].substickX > PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_CSTICK_RIGHT;
	}
	if (sControllerPad[controller->mPlayerNum - 1].substickY < -PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_CSTICK_DOWN;
	}
	if (sControllerPad[controller->mPlayerNum - 1].substickY > PAD_MIN_STICK_READ) {
		keyStatus |= KBBTN_CSTICK_UP;
	}

	// DPAD BUTTONS
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_LEFT) {
		keyStatus |= KBBTN_DPAD_LEFT;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_RIGHT) {
		keyStatus |= KBBTN_DPAD_RIGHT;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_UP) {
		keyStatus |= KBBTN_DPAD_UP;
	}
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_DOWN) {
		keyStatus |= KBBTN_DPAD_DOWN;
	}

	// START
	if (sControllerPad[controller->mPlayerNum - 1].button & PAD_BUTTON_START) {
		keyStatus |= KBBTN_START;
	}

	// import analog and trigger values
	controller->mAnalogA  = sControllerPad[controller->mPlayerNum - 1].analogA;
	controller->mAnalogB  = sControllerPad[controller->mPlayerNum - 1].analogB;
	controller->mTriggerL = sControllerPad[controller->mPlayerNum - 1].triggerLeft;
	controller->mTriggerR = sControllerPad[controller->mPlayerNum - 1].triggerRight;

	// process pressed buttons
	controller->updateCont(keyStatus);
#endif
}
