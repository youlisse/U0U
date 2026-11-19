# U0U

A drum machine firmware for the **M-VAVE FM-1**: a step sequencer coupled with a step FX engine.

The 16 white keys are the 16 steps, the 11 black keys are the 11 drums. On the master bus every effect has its own 16 steps, so each
one lands on the beats you choose. Synthesized drums (9 kits), per-step timing, roll, chance and pitch, 16 patterns (each 16 or 32 steps) that chain into a
song, two LFOs, Euclidean rhythms with mutation, and the computer's MIDI clock over USB. It runs from the FM-1's own audio output;
nothing is sampled.

- **Try it in your browser** (no FM-1 needed; the same code the instrument runs): <https://youlisse.github.io/U0U/>
- **Install it on your FM-1**: <https://youlisse.github.io/U0U/install/> in Chrome or Edge, USB data cable, press Install. It
  can be replaced again at any time (the FM-1 stays in update mode until an install is finished; OCT- + OCT+ held 5 s enters the
  bootloader). More in [docs/FLASHING.md](docs/FLASHING.md).

> U0U is young: read [docs/FLASHING.md](docs/FLASHING.md) before the first install, and keep the official `FM-1.fwsc` to return to it.
> The first save overwrites what another custom firmware left in the flash areas U0U uses (patterns, effects, settings).

## Play

| | |
|---|---|
| **White keys** | the 16 steps of the selected drum (a tap places or removes a hit) |
| **Black keys** | pick a drum; with REC on, they play and write live |
| **EDIT** | per step: timing, roll, chance, pitch, decay, tone, pan. Held: clear the drum's steps |
| **FX** | one effect per black key: tap the steps it hits on, four knobs per page |
| **SCL / ENV / LFO / ARP** | master (low cut, high cut, OTT, clip) / duck the mix from one drum / two LFOs on volume and pan / Euclid and mutate |
| **GLO** | tempo, volume, MIDI clock (follow the computer), swing, effects mix, kit, reset |
| **SAVE** | the 16 patterns (hold one, tap another: copy; EDIT held: delete, the next ones move down), tap twice for the song. Held: save now |
| **SEQ** | tap: help (3 sheets). Held while playing: loop the last beat |
| **OCT- / OCT+** | the two sections of the pattern (a track of 16 or 32 steps): OCT+ twice doubles it, OCT+ / OCT- go from one section to the other, OCT- twice in the first takes the second away. Both held 5 s: leave U0U |

The screen shows a battery at the top (a bolt when USB powers the FM-1). Following the computer: GLO page 1, KNOB 3 to the right; the
SYNC row says `PC LOCKED` when the clock is steady. In Ableton Live: Preferences, Link/Tempo/MIDI, switch **Sync** on for the FM-1's
output port, then press play.

## Build

Needs the JieLi `pi32v2` toolchain (Linux x86-64, run in a container on a Mac) and the AC79 SDK; see [BUILDING.md](BUILDING.md) for
the one-time setup.

```sh
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib JIELI_TOOLCHAIN=$HOME/.jieli/toolchain \
  AC79_SDK=$HOME/fw-AC79_AIoT_SDK python3 tools/build.py        # -> build/u0u.fwsc
sh web/emu/build_drumbox.sh                                    # the browser build (needs Emscripten)
node web/emu/drumbox_test.mjs                                  # the emulator's checks
python3 web/make_site.py build/u0u.fwsc 0.2 site              # the site: emulator, installer, package
```

`firmware/src/drumbox.c` (with `dbx_main.c`) is the root of the build, `firmware/drumbox.ld` its memory map. The design notes, with
what was measured and what is still to read on the FM-1, are in [docs/DESIGN.md](docs/DESIGN.md).

## How it was made

U0U was written with **Claude** (Anthropic's AI model, through Claude Code) as the coding assistant, directed by [@youlisse](https://github.com/youlisse),
who chose what it does, tried it on a real FM-1 and decided what to keep. The code, the tests and the notes in `docs/` were largely
written by the assistant, so read them as such: they are checked by the emulator's tests and by use on one FM-1, not by a long
history of users. Problems are welcome as issues.

## Credits and licence

U0U is **GPL-3.0-only**, built on [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita ([@kurogedelic](https://github.com/kurogedelic),
[Hügelton Instruments](https://hugelton.com)): the hardware layer, the drum voices, the DSP and the package tools are his and his
contributors'. The browser build follows [X0X](https://github.com/charlesvestal/fm1-x0x) by charlesvestal. Every licence that
applies is in [LICENSE](LICENSE), [LICENSING.md](LICENSING.md), [LICENSES/](LICENSES/) and [CREDITS.md](CREDITS.md). M-VAVE and FM-1 are
trademarks of their owners; U0U is not affiliated with or endorsed by them. No warranty: a custom firmware is flashed at your own risk.
