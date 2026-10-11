#ifndef PIKMIN_PORT_PSP_CARD_H
#define PIKMIN_PORT_PSP_CARD_H

#include "Dolphin/card.h"

namespace PikminPsp {
// Replace a complete game file in one transaction. Unlike the GC utility's
// delete-then-rename sequence, the previous save survives an interrupted write.
s32 saveCardFile(s32 channel, CARDStat* state, const void* data, s32* fileNo);
}

#endif
