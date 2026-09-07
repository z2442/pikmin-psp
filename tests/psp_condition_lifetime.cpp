// Test the production Palm startup condition construction without a game world.
#include <math.h>
namespace std { using ::sqrtf; }
#include "TekiConditions.h"
#include <assert.h>
#include <stdio.h>

struct Creature {
	int type;
	float distance;
};
struct Teki : Creature {
	int mTekiType;
};

// Fake leaf queries; the constructors and AND/NOT virtual dispatch are real.
static int pointerCalls, typeCalls, distanceCalls;
bool TekiCreaturePointerCondition::satisfy(Creature* target) const
{
	++pointerCalls;
	return target == mCreature;
}
bool TekiTypeCondition::satisfy(Creature* target) const
{
	++typeCalls;
	return target->type == mTekiType;
}
bool TekiDistanceCondition::satisfy(Creature* target) const
{
	++distanceCalls;
	return target->distance < mDistance;
}

__attribute__((noinline)) static bool evaluate(const Condition& cond, Creature* target)
{
	return cond.satisfy(target);
}

// Retained for testing the previous source's nonstandard temporary expression.
#define stack_new(type) &type

int main()
{
	Teki teki;
	teki.mTekiType = teki.type = 7;
	teki.distance = 0.0f;
	float rad = 20.0f;
#include "palm_conditions.inc"

	// Use the tree after its declaration statement, as findClosest does.
	Creature near = {7, 5.0f}, far = {7, 25.0f}, differentType = {8, 5.0f};
	assert(evaluate(cond, &near));
	assert(!evaluate(cond, &far));
	assert(!evaluate(cond, &differentType));
	assert(!evaluate(cond, &teki));
	assert(pointerCalls == 4 && typeCalls == 3 && distanceCalls == 2);
	puts("PSP AI condition lifetime and short-circuit checks passed");
}
