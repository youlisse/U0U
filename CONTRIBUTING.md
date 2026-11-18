# Contributing to U0U

Thanks for helping. U0U is a drum machine firmware for the M-VAVE FM-1, built on [Felucca](https://github.com/hugelton/Felucca)
(Leo Kuroshita, Hügelton Instruments): the parts that are Felucca's (the hardware layer, the drum voices, the DSP) are best fixed
upstream.

- **A bug you can reproduce**: open an issue, one per bug, with the release you installed, the steps, what you expected and what
  happened. A photo or a short video of the FM-1's screen helps. For a crash, the five hexadecimal lines of the `U0U CRASH` screen.
- **An idea or a question**: an issue is fine too.
- **A pull request**: keep to the style of the file you touch (comments, names), run the emulator's checks
  (`node web/emu/drumbox_test.mjs`) and say what you tried on a real FM-1.

Licence of what you send: GPL-3.0-only, as the rest (see [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md)).
