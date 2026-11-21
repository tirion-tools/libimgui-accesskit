# Third-party notices

The code in this repository is MIT-licensed (`LICENSE`, Copyright (c) 2026 Tirion) unless this file says otherwise.

## Bundled third-party code and data

None. Every file in this repository was written for it; `fuzz/corpus/` holds a synthetic accessibility tree.

## Dependencies (not bundled)

The consumer supplies these at build time; nothing from them is redistributed in this repository:

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT): compiled from the consumer's sources (`IMGUI_ACCESSKIT_IMGUI_DIR`).
- [SDL3](https://libsdl.org/) (zlib).
- [AccessKit C](https://github.com/AccessKit/accesskit-c) (MIT or Apache-2.0).
- Pango, cairo and fontconfig (LGPL-2.1+, LGPL-2.1 or MPL-1.1, MIT-style), linked dynamically. `tools/licences.allow`
  lists the licences of every shared library the gate accepts.
