# Embedded GM.DLS Wavetable Synth

Fixed-point General MIDI wavetable synthesis for embedded targets.

This repository contains a small C wavetable engine that plays a pre-packed RIFF
DLS General MIDI bank, or a user-supplied Gravis UltraSound `.pat` set packed
from a TiMidity config. The runtime audio path is integer/fixed-point only: no
floating point, no DLS parsing, and no dynamic instrument metadata work during
rendering. The intended flow is:

```text
GM-compatible .dls bank -> dls_pack -> compact bank blob -> embedded synth
GUS .pat set + timidity.cfg -> gus_pack -> same compact bank blob -> embedded synth
```

The current integration target is RP2040-class hardware, but the engine itself is
plain single-translation-unit C and only needs a pointer to a packed bank blob.

## Why It Exists

- Use a real wavetable bank instead of simple generated waveforms.
- Keep the real-time path small enough for microcontrollers.
- Store the bank in flash/ROM and render directly from it.
- Optionally cache looped waves in RAM to reduce XIP flash pressure.
- Validate output on a host before flashing firmware.

## Repository Layout

| Path | Purpose |
|------|---------|
| `wavetable.c.inl` | Fixed-point MIDI wavetable engine. Include this in one C translation unit. |
| `gm_bank.h` | Packed bank format and validation helper. |
| `mulaw.h` | Shared G.711 µ-law codec (used when built with `-DWT_PCM_MULAW`). |
| `sine/` | Bank-free sine + LFSR-noise generator engine (selected with `-DMIDI_BACKEND_SINE`). |
| `wt_luts.h` | Baked lookup tables used by the fixed-point engine. |
| `tools/dls_pack.c` | Host tool: converts a RIFF DLS bank into the packed runtime blob. |
| `tools/gus_pack.c` | Host tool: converts a simple TiMidity/GUS patch set into the same packed runtime blob. |
| `examples/wt_render.c` | Host renderer for validating the fixed-point engine against a packed bank. |
| `examples/sine_render.c` | Host renderer for the bank-free sine generator (no sound bank needed). |
| `examples/rp2040/` | Optional glue for an existing RP2040/emulator integration. |
| `docs/usage.md` | Integration guide and public API notes. |
| `docs/device-integration.md` | RP2040-oriented integration and profiling notes. |
| `tools/` | Validation and analysis helpers. |

## Sound Bank Licensing

This project does not include `GM.DLS`, `gm.dls`, `gm_bank.bin`, Microsoft sound
data, or any GUS `.pat` set. Users must provide their own legally usable RIFF DLS
General MIDI bank or GUS patch set and build the packed blob locally.

Buying or owning Windows should not be treated as permission to redistribute
Microsoft's `gm.dls` or to use it outside the rights granted by the applicable
Windows license. Microsoft's Windows license terms describe Windows as licensed,
not sold, apply to included sound files, and reserve rights not expressly granted.
See the current Microsoft license terms page:
https://www.microsoft.com/en-us/useterms

Practical rule for this repo: do not commit or publish `GM.DLS`, `gm.dls`, a
third-party patch directory such as `dgguspat/`, or a packed `gm_bank.bin`
derived from a bank you cannot redistribute.

## Synth Backends (pick one at build time)

One source tree, three build configurations selected by a single compile-time
define. The public API (`parse_midi` / `midi_sample_stereo`) is identical across
all three, so the integrator code does not change between modes.

| Mode | Define | Engine | PCM | Footprint | Use when |
|------|--------|--------|-----|-----------|----------|
| **Wavetable 16-bit** (default) | *(none)* | `wavetable.c.inl` | int16 | full bank | best quality; flash is plentiful |
| **Wavetable µ-law** | `-DWT_PCM_MULAW` | `wavetable.c.inl` | 8-bit µ-law | ~half the PCM | flash-tight; ~38 dB SNR |
| **Sine generator** | `-DMIDI_BACKEND_SINE` | `sine/general-midi.c.inl` | none (no bank) | smallest | no bank at all; pure sines + noise |

`MIDI_BACKEND_SINE` takes precedence if set; `WT_PCM_MULAW` affects only the
wavetable backend (the sine backend has no PCM). **Build the packer and the
engine with the same `WT_PCM_MULAW` setting** — the bank's version tag
(v4 int16 / v5 µ-law) is checked by `gm_bank_view()`, so a mismatched bank is
rejected rather than producing noise.

Host validation can build all three in parallel as distinct binaries:

```powershell
./build.ps1 -Target wt_render                                  # wavetable, 16-bit
./build.ps1 -Target wt_render   -Define WT_PCM_MULAW           # wavetable, µ-law
./build.ps1 -Target sine_render                               # sine generator (no bank)
./build.ps1 -Target midi_selfcheck                             # device glue: 16-bit
./build.ps1 -Target midi_selfcheck -Define WT_PCM_MULAW        # device glue: µ-law
./build.ps1 -Target midi_selfcheck -Define MIDI_BACKEND_SINE   # device glue: sine
```

For embedded integration, the single include point `examples/rp2040/general-midi.c.inl`
dispatches on `MIDI_BACKEND_SINE` for you — see "Embedded Integration" below and
[docs/usage.md](docs/usage.md).

## Build Host Tools

The PowerShell build script expects a C compiler. Put `gcc` in `PATH`, or set
`CC` to a compiler path.

```powershell
./build.ps1 -Target dls_pack
./build.ps1 -Target gus_pack
./build.ps1 -Target wt_render
./build.ps1 -Target midi_selfcheck
```

Pack a bank for your target sample rate (build the packer with the same PCM mode
as the engine — no define for 16-bit, `-DWT_PCM_MULAW` for the half-size µ-law bank):

```powershell
./build.ps1 -Target dls_pack                      # 16-bit packer -> v4 gm_bank.bin
./build/dls_pack.exe path/to/gm.dls build/gm_bank.bin 22050
# ./build.ps1 -Target dls_pack -Define WT_PCM_MULAW   # µ-law packer -> v5 bank
```

Alternatively, pack a user-provided GUS patch set using a simple TiMidity config:

```powershell
./build/gus_pack.exe dgguspat/timidity.cfg build/gm_gus.bin 22050
```

See [docs/usage.md](docs/usage.md) for the full GUS patch-set notes and supported
`gus_pack` input format.

Render a MIDI file through the fixed-point engine on the host:

```powershell
./build.ps1 -Target wt_render
./build/wt_render.exe song.mid build/song.wav build/gm_bank.bin
```

The repository also ships a tiny bank-free generator synth in `sine/` (sine
table + noise LFSR, GM envelopes, drum map). Render it to a 16-bit stereo WAV with
no sound bank at all:

```powershell
./build.ps1 -Target sine_render
./build/sine_render.exe song.mid build/song.wav
```

## Embedded Integration

At runtime, bind the packed bank once and feed MIDI channel-voice messages:

```c
#include <stdint.h>
#include "gm_bank.h"

#define INLINE static inline
#define SOUND_FREQUENCY 22050
#include "wavetable.c.inl"

void synth_init(const void *bank_blob) {
    wt_set_bank(bank_blob);
}

void synth_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    midi_command_t m = { status, data1, data2, 0 };
    parse_midi(&m);
}

void synth_render(int16_t *stereo, int frames) {
    for (int i = 0; i < frames; ++i) {
        midi_sample_stereo(&stereo[0], &stereo[1]);
        stereo += 2;
    }
}
```

See `docs/usage.md` for the complete API and configuration macros.
