# projectM for LG webOS

A native webOS app that shows [projectM](https://github.com/projectM-visualizer/projectm)
(MilkDrop-compatible) visuals for whatever the TV is playing. It reads audio from the
TV's pulseaudio mixer monitor (`pcm_output.monitor`), so it reacts to any app, including
[Sendspin Cinema](https://github.com/daredoes/sendspin-webos).

- Left / Right switch presets. Back exits.
- OK adds the current preset to favorites, or removes it. Favorites are listed in
  `/media/internal/projectm-webos-favorites`, one path per line relative to `presets/`.
- Up toggles automatic preset changes (every 25 s). Down toggles favorites-only, which limits
  both auto-change and Left / Right to the favorites (all presets while there are none). Both
  are off until turned on, then remembered in `/media/internal/projectm-webos-autochange` and
  `/media/internal/projectm-webos-favorites-only`.
- The preset name shows in the top-left corner for 4 s after each change, with `* ` in front
  of a favorite.
- A preset that renders below 30 fps after its 3 s blend-in is skipped, unless it's a favorite.
- Launch parameters `{"screensaver": true}` make it a screensaver: any other key or a
  pointer click exits back to whatever was on screen.
- Renders at half the panel resolution and stretches it to full screen. Set
  `PROJECTM_SCALE=1` for full resolution, `PROJECTM_SOURCE` for another pulse source.
- Log: `/tmp/projectm-webos.log` (GL renderer, preset count, fps every 300 frames, and the
  codes of keys it doesn't use).
- Launch parameters `{"benchmark": true}` run a benchmark: every preset, in path order, gets a
  generated 120 bpm drum loop instead of the TV's audio, 1 s to settle and 2 s of counted
  frames, with vsync off where the driver allows. Each result is appended to
  `/media/internal/projectm-webos-bench.tsv` as `fps<TAB>load ms<TAB>preset` (a preset that
  fails to load gets 0 fps). A new run resumes after the last result; delete the file to start
  over. It exits when done, or on Back. The full set takes about 9 hours.
  The **Benchmark presets** workflow runs it on each TV in turn from a self-hosted runner on
  the home network (label `lan`) using the `TV_PASSWORD` secret, and uploads one table per TV;
  `scripts/benchmark-tv.sh` does the same from any machine on the network.

Built for 32-bit ARM webOS (`arm-webos-linux-gnueabi`) in GitHub Actions; the IPK is a
workflow artifact. Presets come from
[presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)
at a pinned commit (`PRESETS_SHA` in the workflow) at build time.

projectM is LGPL-2.1 and linked statically; this app's source is here to satisfy that.
