// Host compile/link check for the device GM glue (examples/rp2040/general-midi.c.inl).
// Verifies the selected backend wires up and the parse_midi / midi_sample /
// midi_cache_release contract that mpu401.c.inl depends on still resolves, with
// INLINE supplied by the includer (as emulator.h does on device). It is NOT meant
// to run (the wavetable backend needs a real bank); a clean compile + link is the
// test.
//
//   wavetable (16-bit):   powershell -File build.ps1 -Target midi_selfcheck
//   wavetable (µ-law):    powershell -File build.ps1 -Target midi_selfcheck -Define WT_PCM_MULAW
//   sine backend:         powershell -File build.ps1 -Target midi_selfcheck -Define MIDI_BACKEND_SINE
#include <stdint.h>

#define INLINE inline           // emulator convention: code writes `static INLINE`
#define SOUND_FREQUENCY 22050
#define __not_in_flash(x)       // sine-backend table attribute; no-op on host
#ifndef MIDI_BACKEND_SINE
#define WT_BANK_EXTERN          // skip the .incbin embed; supply a stub bank below
#endif
#include "../examples/rp2040/general-midi.c.inl"

#ifndef MIDI_BACKEND_SINE
// Stand-in for the embedded bank so the link resolves without the multi-MB file.
const uint8_t gm_bank_blob[64] = {0};
#endif

int main(void) {
    midi_command_t c = {0x90, 60, 100, 0};   // note-on
    parse_midi(&c);
    int16_t s[2];
    midi_sample(s);                          // stereo render -> out[0]=L, out[1]=R
    volatile int16_t mono = (int16_t) (((int32_t) s[0] + s[1]) >> 1);
    (void) mono;
    midi_cache_release();                     // public cache-drop entry point (no-op on sine)
    return 0;
}
