#include "port/psp/PspAudio.h"
#include <stdlib.h>
#include <string.h>

namespace {
// Convert metadata only. JAM bytecode and compressed AW/STX samples retain
// their disc byte order and are decoded when consumed.
struct BankReader {
	u8* dst;
	const u8* src;
	u32 size;
	bool ok;
	bool range(u32 p, u32 n) { return ok = ok && p <= size && n <= size - p; }
	u32 word(u32 p) {
		if (!range(p, 4)) return 0;
		return (u32(src[p]) << 24) | (u32(src[p + 1]) << 16) | (u32(src[p + 2]) << 8) | src[p + 3];
	}
	u32 swap32(u32 p) { u32 v = word(p); if (ok) memcpy(dst + p, &v, 4); return v; }
	void swap16(u32 p) {
		if (!range(p, 2)) return;
		u16 v = (u16(src[p]) << 8) | src[p + 1]; memcpy(dst + p, &v, 2);
	}
	void words(u32 p, u32 n) {
		if (n > size / 4 || !range(p, n * 4)) return;
		for (u32 i = 0; i < n; ++i) swap32(p + i * 4);
	}
	u32 pointer(u32 base, u32 p) {
		u32 v = swap32(p);
		if (!v) return 0;
		if (base > size || v >= size - base) { ok = false; return 0; }
		return base + v;
	}
	u32 count(u32 p, u32 limit = 4096) {
		u32 n = swap32(p); if (n > limit) { ok = false; return 0; } return n;
	}
	void envelope(u32 p) {
		if (!p) return;
		for (unsigned i = 0; i < 1024 && ok; ++i, p += 6) {
			if (!range(p, 6)) return;
			u16 command = (u16(src[p]) << 8) | src[p + 1];
			swap16(p); swap16(p + 2); swap16(p + 4);
			if (command >= 13) return;
		}
		ok = false;
	}
	void oscillator(u32 base, u32 p) {
		if (!p) return;
		swap32(p + 4);
		envelope(pointer(base, p + 8)); envelope(pointer(base, p + 12));
		words(p + 16, 2);
	}
	void velocity(u32 base, u32 p) {
		p = pointer(base, p); if (!p) return;
		// Consumers treat WSYS and wave indices as one packed 32-bit ID.
		words(p + 4, 3);
	}
	void instrument(u32 base, u32 p) {
		const u32 tag = swap32(p);
		if (tag == 0x494e5354) { // INST
			words(p + 4, 3);
			oscillator(base, pointer(base, p + 16)); oscillator(base, pointer(base, p + 20));
			for (u32 i = 24; i < 40; i += 4) {
				u32 effect = pointer(base, p + i);
				if (effect) words(effect + 4, 2);
			}
			u32 n = count(p + 40, 128);
			for (u32 i = 0; i < n && ok; ++i) {
				u32 key = pointer(base, p + 44 + 4 * i);
				if (!key) { ok = false; break; }
				u32 m = count(key + 4, 128);
				for (u32 j = 0; j < m && ok; ++j) velocity(base, key + 8 + 4 * j);
			}
		} else if (tag == 0x50455232 || tag == 0x50455243) { // PER2/PERC
			for (u32 i = 0; i < 128 && ok; ++i) {
				u32 key = pointer(base, p + 0x88 + i * 4);
				if (!key) continue;
				words(key, 2);
				for (u32 j = 8; j < 16; j += 4) {
					u32 effect = pointer(base, key + j);
					if (effect) words(effect + 4, 2);
				}
				u32 n = count(key + 16, 128);
				for (u32 j = 0; j < n && ok; ++j) velocity(base, key + 20 + j * 4);
			}
			if (tag == 0x50455232) for (u32 i = 0; i < 128; ++i) swap16(p + 0x308 + i * 2);
		} else {
			ok = false;
		}
	}
	void bank(u32 p) {
		if (word(p) != 0x49424e4b || word(p + 32) != 0x42414e4b) { ok = false; return; }
		words(p, 4); swap32(p + 32);
		for (u32 i = 0; i < 240 && ok; ++i) {
			u32 inst = pointer(p, p + 36 + i * 4);
			if (inst) instrument(p, inst);
		}
	}
	void waveSystem(u32 p) {
		if (word(p) != 0x57535953) { ok = false; return; }
		words(p, 4);
		u32 winf = pointer(p, p + 16), wbct = pointer(p, p + 20);
		if (!winf || !wbct || word(winf) != 0x57494e46 || word(wbct) != 0x57424354) { ok = false; return; }
		swap32(winf);
		u32 n = count(winf + 4, 256);
		for (u32 i = 0; i < n && ok; ++i) {
			u32 arc = pointer(p, winf + 8 + i * 4);
			if (!arc || !range(arc, 0x74) || !memchr(src + arc, 0, 0x40)) { ok = false; break; }
			u32 m = count(arc + 0x70);
			for (u32 j = 0; j < m && ok; ++j) {
				u32 w = pointer(p, arc + 0x74 + j * 4);
				if (!w) { ok = false; break; }
				words(w + 4, 7); swap16(w + 32); swap16(w + 34); swap32(w + 36);
			}
		}
		words(wbct, 2); n = count(wbct + 8, 256);
		for (u32 i = 0; i < n && ok; ++i) {
			u32 scene = pointer(p, wbct + 12 + i * 4);
			if (!scene) { ok = false; break; }
			words(scene, 2); u32 deps = count(scene + 8, 256); words(scene + 24, deps);
			for (u32 j = 12; j < 24 && ok; j += 4) {
				u32 ctrl = pointer(p, scene + j); if (!ctrl) continue;
				swap32(ctrl); u32 m = count(ctrl + 4);
				for (u32 k = 0; k < m && ok; ++k) {
					u32 id = pointer(p, ctrl + 8 + k * 4);
					if (id) swap32(id); else ok = false;
				}
			}
		}
	}
};
struct SequenceRange { uintptr_t begin, end; } sSequences[32];
u32 sSequenceCount;
}

namespace PikminPsp {
bool audioConvertBank(void* data, u32 size)
{
	if (!data || size < 16 || size > 1024 * 1024) return false;
	u8* original = (u8*)malloc(size);
	if (!original) return false;
	memcpy(original, data, size);
	BankReader r = { (u8*)data, original, size, true };
	for (u32 t = 0; t < 2 && r.ok; ++t) {
		u32 table = r.swap32(t * 8), n = r.count(t * 8 + 4, 256);
		for (u32 i = 0; i < n && r.ok; ++i) {
			u32 p = r.swap32(table + i * 8), length = r.swap32(table + i * 8 + 4);
			if (!length) continue;
			if (!r.range(p, length)) break;
			if (t == 0) r.waveSystem(p); else r.bank(p);
		}
	}
	free(original);
	return r.ok;
}

void audioConvertSequenceHeader(void* data)
{
	// The embedded header is shared by every sequence and converted once.
	static bool converted;
	if (converted) return;
	converted = true;
	u8* b = (u8*)data;
	u32 n = (u32(b[12]) << 24) | (u32(b[13]) << 16) | (u32(b[14]) << 8) | b[15];
	if (n > 256) return;
	memcpy(b + 12, &n, 4);
	for (u32 i = 0; i < n; ++i) {
		u8* e = b + 32 + i * 32;
		u16 dummy = (u16(e[14]) << 8) | e[15]; memcpy(e + 14, &dummy, 2);
		for (u32 j = 16; j < 32; j += 4) {
			u32 v = (u32(e[j]) << 24) | (u32(e[j + 1]) << 16) | (u32(e[j + 2]) << 8) | e[j + 3];
			memcpy(e + j, &v, 4);
		}
	}
}

void audioRegisterSequence(const void* data, u32 size)
{
	uintptr_t p = (uintptr_t)data;
	for (u32 i = 0; i < sSequenceCount; ++i) {
		if (sSequences[i].begin == p) { sSequences[i].end = p + size; return; }
	}
	if (sSequenceCount < 32) sSequences[sSequenceCount++] = { p, p + size };
}

s16 audioReadEnvelope(const s16* data)
{
	uintptr_t p = (uintptr_t)data;
	for (u32 i = 0; i < sSequenceCount; ++i) {
		if (p >= sSequences[i].begin && p + 2 <= sSequences[i].end) {
			const u8* b = (const u8*)data;
			return s16((u16(b[0]) << 8) | b[1]);
		}
	}
	return *data;
}
}
