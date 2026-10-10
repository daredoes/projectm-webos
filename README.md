# projectM for LG webOS

A native webOS app that shows [projectM](https://github.com/projectM-visualizer/projectm)
(MilkDrop-compatible) visuals for whatever the TV is playing. It reads audio from the
TV's pulseaudio mixer monitor (`pcm_output.monitor`), so it reacts to any app, including
[Sendspin Cinema](https://github.com/daredoes/sendspin-webos).

- Back exits; Left / Right switch presets. Presets change on their own every 25 s.
- Renders at half the panel resolution and stretches it to full screen. Set
  `PROJECTM_SCALE=1` for full resolution, `PROJECTM_SOURCE` for another pulse source.
- Log: `/tmp/projectm-webos.log` (GL renderer, preset count, fps every 300 frames).

Built for 32-bit ARM webOS (`arm-webos-linux-gnueabi`) in GitHub Actions; the IPK is a
workflow artifact. Presets come from
[presets-cream-of-the-crop](https://github.com/projectM-visualizer/presets-cream-of-the-crop)
at build time.

projectM is LGPL-2.1 and linked statically; this app's source is here to satisfy that.
