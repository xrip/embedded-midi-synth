#pragma GCC optimize("Ofast")
#pragma once
//
// General MIDI synthesizer — single device include point.
//
// Pick the synth backend with ONE compile-time define. The public contract is
// the same either way — what mpu401.c.inl / the emulator call never changes:
//
//   * void parse_midi(const midi_command_t *)
//   * void midi_sample(int16_t out[2])           stereo render (L,R)
//   * void midi_cache_release(void)              drop RAM cache (no-op on sine)
//
// Backends:
//   (default)          GM.DLS/GUS wavetable, 16-bit PCM (best quality).
//                      Needs a v4 gm_bank.bin (dls_pack / gus_pack, no define).
//   -DWT_PCM_MULAW     wavetable, 8-bit µ-law PCM (~half the flash, ~38 dB SNR).
//                      Needs a v5 bank (pack with -DWT_PCM_MULAW).
//   -DMIDI_BACKEND_SINE  bank-free sine + LFSR-noise generator (smallest; no bank).
//
// MIDI_BACKEND_SINE takes precedence when set. WT_PCM_MULAW affects only the
// wavetable backend (the sine backend has no PCM, so it is ignored there).
//
// Provided by the includer (emulator.h): INLINE, and (sine backend)
// SOUND_FREQUENCY + __not_in_flash; optionally __not_in_flash_func to RAM-place
// the wavetable hot path (big win: the 16 KB XIP cache is shared between code
// and the flash-resident PCM). midi_selfcheck.c / sine_render.c show host stubs.

// ============================================================================
// Backend 3 — sine / noise generator (no sound bank)
// ============================================================================
#ifdef MIDI_BACKEND_SINE

#ifndef SOUND_FREQUENCY
#define SOUND_FREQUENCY 22050          // the sine pitch tables are baked for this
#endif
#ifndef __fast_mul
#define __fast_mul(a, b) ((a) * (b))   // device maps this to a single-cycle MUL
#endif

// The sine engine writes `static INLINE ...` (with a leading `static`), so it uses
// the includer's INLINE as-is (emulator.h / midi_selfcheck define INLINE = inline).
// Do NOT bridge it to `static inline` here — unlike wavetable.c.inl below, which
// writes `INLINE foo` (no leading static) and therefore needs that bridge.
#include "../../sine/general-midi.c.inl"   // parse_midi, midi_sample_stereo, midi_command_t

// No bank to embed and no RAM wave cache: keep the two public entry points so the
// consumer links unchanged under either backend.
static INLINE void midi_sample(int16_t samles[2]) {
    midi_sample_stereo(&samles[0], &samles[1]);
}
void midi_cache_release(void) { /* no-op: the sine backend has no wave cache */ }

// ============================================================================
// Backends 1 & 2 — GM.DLS/GUS wavetable (16-bit int16, or 8-bit µ-law)
// ============================================================================
#else

#include "../../gm_bank.h"

#define WT_MAX_VOICES 32
#ifndef WT_RAMFUNC
#ifdef __not_in_flash_func
#define WT_RAMFUNC(name) __not_in_flash_func(name)   // keep the render loop in RAM
#endif
#endif

// RAM wave cache (on by default): the first time a looped wave plays it is
// malloc'd into RAM and read from RAM thereafter, so the per-sample pcm[] reads
// stop hitting XIP flash (the real bottleneck at high polyphony). It caches as
// much as fits in the heap right now and frees its least-recently-used wave when
// the heap is full — RAM use scales with the live working set, no fixed
// reservation. Host A/B on Doom/omf/dott: ~90%+ of per-sample reads from RAM,
// bit-exact. Build with -DWT_NO_WAVE_CACHE to compile it out entirely (no malloc)
// on targets with no spare RAM. See wavetable.c.inl / docs/device-integration.md.

// wavetable.c.inl marks its functions `INLINE foo` expecting INLINE == `static
// inline`. The emulator's INLINE is just `inline` (its code writes `static
// INLINE`), which would give wavetable external-linkage inlines. Bridge it for
// the wavetable include only, then restore the emulator's INLINE so the rest of
// the TU (mpu401.c.inl's `static INLINE ...`) keeps compiling.
#pragma push_macro("INLINE")
#undef INLINE
#define INLINE static inline
#include "../../wavetable.c.inl"   // parse_midi, midi_sample_stereo, wt_set_bank, midi_command_t
#pragma pop_macro("INLINE")

// Embed the packed soundbank in flash via inline-asm .incbin. Generate it first
// (dls_pack/gus_pack) and make it reachable by the assembler (e.g. -Wa,-I<dir>
// on this TU, or an absolute path below). Define WT_BANK_EXTERN to skip the embed
// and supply gm_bank_blob yourself (host self-check, a separate .S/.c, etc.). The
// bank's PCM format (v4 int16 / v5 µ-law) MUST match this TU's WT_PCM_MULAW
// setting — gm_bank_view() rejects a mismatched version. See docs/device-integration.md.
#ifndef WT_BANK_EXTERN
#define IMPORT_BIN(file, sym) asm (\
    ".section .rodata." #sym "\n"           /* own rodata subsection (flash) */\
    ".balign 4\n"                           /* word alignment */\
    ".global " #sym "\n"                    /* export the object address */\
    #sym ":\n"                              /* define the object label */\
    ".incbin \"" file "\"\n"                /* import the file */\
    ".global _sizeof_" #sym "\n"            /* export the object size */\
    ".set _sizeof_" #sym ", . - " #sym "\n" /* define the object size */\
    ".balign 4\n"                           /* word alignment */\
    ".section \".text\"\n")                 /* restore section */
IMPORT_BIN("gm_bank.bin", gm_bank_blob);
#endif
extern const uint8_t gm_bank_blob[];        // size also available as _sizeof_gm_bank_blob

static int wt_bank_ready = 0;

// Bind the engine to the flash bank. Runs before main() via the C runtime init
// array on the Pico; the lazy guard in midi_sample() is a fallback for runtimes
// that do not run constructors.
static void __attribute__((constructor)) gm_wavetable_init(void) {
    wt_set_bank(gm_bank_blob);
    wt_bank_ready = 1;
}

// Stereo render. For a mono sink, mix to center: ((int32_t)l + r) >> 1.
static INLINE void midi_sample(int16_t samles[2]) {
    if (__builtin_expect(!wt_bank_ready, 0)) {
        wt_set_bank(gm_bank_blob);
        wt_bank_ready = 1;
    }
    midi_sample_stereo(&samles[0], &samles[1]);
}
// Hand all the wave cache's RAM back to the heap when another subsystem needs it.
// MIDI keeps playing (voices on a RAM copy fall back to the byte-identical flash
// PCM seamlessly; the cache re-fills on demand). No-op when the cache is compiled
// out (WT_NO_WAVE_CACHE). Exported (external linkage): a consumer in another TU
// declares `extern void midi_cache_release(void);`. Call it from the same context
// as the MIDI/audio path, not concurrently with midi_sample().
void midi_cache_release(void) {
    wt_cache_release();
}

#endif // MIDI_BACKEND_SINE
