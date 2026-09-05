#include "port/psp/PspAudio.h"
#include "port/psp/PspAudioDecode.h"
#include "jaudio/oneshot.h"
#include "jaudio/audiostruct.h"
#include "jaudio/jammain_2.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

static u32 word(const u8* p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }

int main(int argc, char** argv)
{
	s32 a = 0, b = 0;
	s16 out[16];
	u8 block[9] = {0x10, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67};
	PikminPsp::audioDecodeAfc(block, out, a, b);
	for (int i = 0; i < 16; ++i) assert(out[i] == (i - 8) * 2);
	assert(a == 14 && b == 12);
	u8 hold[9] = {0x01};
	PikminPsp::audioDecodeAfc(hold, out, a, b);
	for (s16 s : out) assert(s == 14);
	u8 clamp[9] = {0xf0, 0x78};
	PikminPsp::audioDecodeAfc(clamp, out, a, b);
	assert(out[0] == 32767 && out[1] == -32768);
	u8 low[5] = {0x10, 0x1b, 0x1b, 0x1b, 0x1b};
	PikminPsp::audioDecodeAfc(low, out, a, b, true);
	const s16 expected[] = {0, 8, -16, -8};
	for (int i = 0; i < 16; ++i) assert(out[i] == expected[i % 4]);
	SOUNDID_ id; id.value = 0x12345678;
	assert(id.bytes[0] == 0x12 && id.bytes[1] == 0x34 && id.bytes[2] == 0x56 && id.bytes[3] == 0x78);
	MixConfig mix; mix.whole = 0x0152;
	assert(mix.parts.upper == 1 && mix.parts.lower0 == 5 && mix.parts.lower1 == 2);
	URegisterParam_ registers = {};
	// The resident Olimar sequence writes the low half of its dispatch-table
	// address this way before reading it as extended register 3.
	registers.reg[23] = 0x147c;
	assert(Jam_GetExtendedRegister(&registers, 3) == 0x0000147c);
	Jam_SetExtendedRegister(&registers, 2, 0x12345678);
	assert(registers.reg[20] == 0x1234 && registers.reg[21] == 0x5678);
	assert(Jam_GetExtendedRegister(&registers, 2) == 0x12345678);
	s16 native[] = {-300, 1234};
	assert(PikminPsp::audioReadEnvelope(native) == -300);
	u8 sequence[] = {0xfe, 0xd4, 0x04, 0xd2};
	PikminPsp::audioRegisterSequence(sequence, sizeof(sequence));
	assert(PikminPsp::audioReadEnvelope((s16*)sequence) == -300);
	assert(PikminPsp::audioReadEnvelope((s16*)(sequence + 2)) == 1234);
	// Validate the real asset when supplied, without committing game data.
	if (argc > 1) {
		FILE* f = fopen(argv[1], "rb"); assert(f);
		fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f);
		std::vector<u8> original(size); assert(fread(original.data(), 1, size, f) == (size_t)size); fclose(f);
		auto bank = original;
		assert(PikminPsp::audioConvertBank(bank.data(), bank.size()));
		assert(word(bank.data() + 4) == 22 && word(bank.data() + 12) == 22);
		u32 table = word(bank.data() + 8);
		u32 base = word(bank.data() + table + 16); // jungle bank
		u32 instrument = base + word(bank.data() + base + 36);
		u32 key = base + word(bank.data() + instrument + 44);
		u32 velocity = base + word(bank.data() + key + 8);
		assert(word(bank.data() + velocity + 4) == 0x00020000);
		assert(word(bank.data() + velocity + 8) == 0x3f800000); // volume = 1
		for (u32 cut : {0U, 15U, 64U, 1024U, u32(size / 2)}) {
			bank = original;
			assert(!PikminPsp::audioConvertBank(bank.data(), cut));
		}
		bank = original; bank[4] = 0xff;
		assert(!PikminPsp::audioConvertBank(bank.data(), bank.size()));
		puts("Original bank conversion and truncated-asset checks passed");
	}
	puts("AFC, predictor history, clipping, packed IDs, envelopes and routing checks passed");
}
