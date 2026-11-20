# Flashing U0U on an FM-1

**Not yet tried on a real FM-1.** The package builds and passes the build's checks (size, RAM, no calls from RAM code, no
register access outside `hal/`), and its shared code is the browser build's (689 checks). Everything below the first boot is
untested; installing is at your own risk.

## 0. Before

- Keep the stock package: `FM-1.fwsc` (the official V16, 2026-10-21, linked from `upstream/README.md`). Felucca's installer and
  `fm1_install.py` take only the unmodified **V15** as "official" (the **Return to official V15** button, no `--force`), and the
  community advice (upstream README) is to go back to the official stock before flashing the next custom firmware. Whether the
  installer's handshake works with a V16 FM-1 has not been tried here: if it does not find the FM-1, M-VAVE's own updater is the other way.
- U0U writes only the flash areas that Felucca's store uses (`0x97000-0xDFFFF` projects and presets, `0xE5000-0xE6FFF`, `0xFC000-0xFEFFF`
  settings). Its objects have their own magics, so what Felucca left there is ignored and **overwritten by U0U's first save**
  (a few seconds after an edit, when stopped).
- Turn **MASTER down** for the first sound: the output reaches 0 dBFS (Felucca stops at -6 dB).

## 1. Build

```sh
cd drumbox
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib JIELI_TOOLCHAIN=$HOME/.jieli/toolchain \
  AC79_SDK=$HOME/fw-AC79_AIoT_SDK ~/.fm1-venv/bin/python tools/build.py        # colima must be running
```

Writes `build/u0u.fwsc` (identity `FM-1_2100004`: 210 for U0U and 0004 for 0.4, free on the Firmware Hub's identity check), `build/u0u.bin` and the
update loader. `--target felucca` builds the original firmware (`build/felucca.fwsc`) with the same tool.

## 2. Install

Web installer (Chrome or Edge, USB data cable, no other app on the FM-1's MIDI port). Published, it is `https://<user>.github.io/<repo>/install/`
(the page to try U0U in the browser is the root of the same site, with an "Install it on your FM-1" button). From a local copy:

```sh
python3 web/make_site.py build/u0u.fwsc 0.4 site          # the emulator at /, the installer at /install/, firmware/u0u-0.4.fwsc
python3 -m http.server 8793 -d site --bind 127.0.0.1      # http://localhost:8793/install/
```

or the command line (`pip3 install mido python-rtmidi`):

```sh
python3 tools/fm1_install.py build/u0u.fwsc
python3 tools/fm1_install.py --info            # who the connected FM-1 says it is
```

Do not unplug while it writes; if it is cut, install again (the FM-1 stays in update mode until it is finished).

## 3. First minutes

1. The screen shows the logo, `U0U`, `By`, `@Y_oulisse` for 0.9 s, then the lane page. White keys light as steps, a black key plays its drum.
2. **SEQ** (tap twice, the second sheet, the foot of it): which package is installed, `U0U v0.4  FM-1_2100004`.
   **SEQ** (tap three times, the third sheet): the bottom line is the audio's load, `LOAD n%  PEAK m%`, with the key scan and USB
   nested in it. Press PLAY first. The desktop estimate is 30% at the worst; the board's figure is to be read here (M5.1). Near 100% the
   audio drops out.
3. Change something, wait, power off, power on: the pattern should come back. **SAVE** held 0.6 s writes at once.
   To delete a pattern: **SAVE** (the pattern page), tap its key, then hold **EDIT** 0.6 s (header says DELETED): the next ones move down, 1 2 3 without the 2nd gives 1 2; HOME held is the full reset.
4. A flash erase may stop the audio for a moment (SAVE held while playing): to be heard and noted.
5. Screen: the oscilloscope is a ~20 ms blit every 30 ms; if the keys feel late, that is the first thing to tune.

## 4. The way out (all in the build)

- **OCT- + OCT+ held**: "UPDATE 3, 2, 1" then UBOOT. Letting go before the end cancels.
- **The installer again, with U0U running**: U0U answers its handshake (`FELUCCA_OTA`), so Install / Return to official V15 work as from Felucca.
- **Two boots in a row that die in their first 30 s** (a crash screen, a hang): the next one enters UBOOT by itself.
- **The stock soft key's SysEx** (`F0 22 24 35 7D F7`) enters UBOOT.
- Beyond that: the mask-ROM `WL82 UBOOT1.00` over USB, or a hardware `USB_KEY` pulse (README, Flashing and safety).

## 5. What to report back

The `LOAD` / `PEAK` figures with a full pattern and all effects on; whether it boots from V15 and from another firmware; whether
SAVE glitches the sound; the LED and screen behaviour; anything the crash screen (`U0U CRASH` and five hex lines) shows.
