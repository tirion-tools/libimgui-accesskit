# libimgui-accesskit

Accessibility and international text for [Dear ImGui](https://github.com/ocornut/imgui) applications on Linux (C++20).
Three independent modules:

- **`imgui_accesskit::bridge`** (`bridge.h`): an immediate-mode bridge to [AccessKit](https://accesskit.dev) and through
  it to AT-SPI. The UI declares accessibility nodes every frame next to the widgets they describe (role, label, value,
  states, relations, text runs and selections), nested with push/pop; `end_frame()` publishes the tree to AccessKit only
  when it changed. AccessKit actions (click, focus) arrive on AccessKit's thread and are handed to the UI thread as an
  SDL user event.
- **`imgui_accesskit::text`** (`run_cache.h`, `line_edit.h`): text shaped by [Pango](https://pango.gnome.org) and
  rasterised by cairo instead of ImGui's font atlas: complex scripts, bidi, font fallback and colour emoji. `RunCache`
  caches runs as A8 alpha masks drawn as tinted ImGui images, with a per-frame budget for cold rasterisation, LRU
  eviction and hit-testing for selection. `LineEdit` is a single-line editor on Pango: grapheme-cluster and visual
  (bidi) caret movement, split carets, word moves, IME composition through SDL text events, masked passphrase fields.
- **`imgui_accesskit::cef`** (`cef_tree.h`): a validating mirror of a [CEF](https://bitbucket.org/chromiumembedded/cef)
  accessibility tree (the JSON of `OnAccessibilityTreeChange` / `OnAccessibilityLocationChange`), with its own bounded
  JSON parser, that always exports a well-formed tree. It depends on the standard library only.

## Builds on

- **[AccessKit C](https://github.com/AccessKit/accesskit-c)** (MIT or Apache-2.0): the accessibility tree model and the
  Unix (AT-SPI) adapter. The gate uses the upstream 0.23.2 release.
- **[SDL3](https://libsdl.org)** (zlib): events, IME text input and the clipboard. The gate uses 3.4.2.
- **[Dear ImGui](https://github.com/ocornut/imgui)** (MIT): drawing and item state. The gate uses v1.93.0-docking.
  `LineEdit` uses `imgui_internal.h` (`ItemAdd`, `SetActiveID`, `PlatformImeData`), which can change between ImGui
  releases.
- **Pango, cairo, fontconfig** (LGPL-2.1+, LGPL-2.1/MPL-1.1, MIT-style), linked dynamically.

Fixes to those projects go upstream, not into this repository.

## Use

```cmake
add_subdirectory(path/to/libimgui-accesskit ${CMAKE_BINARY_DIR}/imgui-accesskit)
target_link_libraries(app PRIVATE imgui_accesskit::bridge imgui_accesskit::text imgui_accesskit::cef)
```

The library uses the consumer's targets when they exist: `imgui`, `SDL3::SDL3-static` (else `SDL3::SDL3`),
`accesskit` and `PkgConfig::PANGOCAIRO` (pkg-config `pangocairo fontconfig`). Otherwise it calls
`find_package(SDL3 CONFIG)`, `find_package(ACCESSKIT)` (an unpacked accesskit-c release on `CMAKE_PREFIX_PATH`) and
pkg-config, and compiles Dear ImGui's core from `IMGUI_ACCESSKIT_IMGUI_DIR`. `text` also links `OpenGL::GL`.

| Option | Default | Module |
|---|---|---|
| `IMGUI_ACCESSKIT_BRIDGE` | `ON` | `imgui_accesskit_bridge` / `imgui_accesskit::bridge` (SDL3, AccessKit) |
| `IMGUI_ACCESSKIT_TEXT` | `ON` | `imgui_accesskit_text` / `imgui_accesskit::text` (Dear ImGui, SDL3, OpenGL, Pango) |
| `IMGUI_ACCESSKIT_CEF` | `ON` | `imgui_accesskit_cef` / `imgui_accesskit::cef` (nothing) |

A build that only needs the CEF mirror (for example a fuzzing build) sets the other two options `OFF` and needs none of
their dependencies. Headers are included as `"imgui_accesskit/<name>.h"`.

## API

- **Bridge** (namespace `imgui_accesskit`): `Bridge` with `start(window, title)` / `stop()`, the frame protocol
  `begin_frame(scale, width, height)`, `add`, `push`, `pop`, `top`, `set_rect`, `focus`, `end_frame(window_focused)`;
  actions (`kClick`, `kFocus`, `kSetValue`) with `take_click(id)`, `take_focus(id)` and `take_set_value(id, &text)`
  (an AT's SetValue with a string: AT-SPI `EditableText.SetTextContents` on a text input with a text run), handed out
  from the `begin_frame()` after they arrive, while `handle_event(SDL_Event)` consumes the wake-up event; `id_of(key)` (stable FNV-1a ids),
  `last_frame()`, `duplicate_ids()`, `active()`, `at_active()`. A `Node` carries `Role`, label, description, value, URL,
  bounds, selected/expanded/toggled/checked, live region, level, popup kind, orientation, modal, disabled, actions,
  invalid (a refused entry), numeric value and range, table row index/count, active descendant, `controls` / `described_by` / `labelled_by`, and
  text runs (`character_lengths`, `TextPosition` selection). Rules enforced in `end_frame()`: relations to nodes missing
  from the frame are dropped, text selections must name `kTextRun` descendants, and a repeated id gets a derived id
  (counted in `duplicate_ids()`) so AccessKit never sees a malformed update. Levels are one-based.
- **Text** (namespace `imgui_accesskit::text`): `RunStyle` (family, size, weight, wrap or ellipsis width, direction,
  alignment, fit-to-slot, scale), `TextRun::draw`, `RunCache` (`get`, budgeted `try_get`, `measure`, `metrics`,
  `new_frame(cold_budget_ms)`, `byte_budget`, `index_at`, `range_boxes`, `word_at`, `make_layout`), `LineEdit`
  (`handle_event`, `draw`, text, cursor and anchor in bytes, `set_masked`, `set_font`, `revision`), `LineEditColors`.
  Textures go through `TextureOps` (upload A8, upload RGBA, destroy), given to the `RunCache` constructor; the default is
  OpenGL (`upload_alpha_texture`: `GL_R8` with an alpha swizzle; `upload_rgba_texture`; `delete_texture`) and needs a
  current GL context. Another renderer, or a headless test, passes its own.
- **CEF mirror** (namespace `imgui_accesskit::cef`): `CefTreeMirror` (`reset(generation)`, `apply(generation, json,
  location_only)`, `valid`, `focus`, `last_reject`, `export_tree`), `CefTreeLimits`, `CefKind`, `CefExport` (with the
  content's `language`, the HTML `lang`, for `Node::language`). Mapping the exported nodes onto bridge nodes is the
  caller's choice (the bridge has the roles: link, image, caption, list, table, heading, paragraph).

## Security model

- **CEF accessibility JSON is hostile input** (renderer-controlled). `CefTreeMirror` bounds it with `CefTreeLimits`:
  1.5 MiB of JSON, nesting 64, 64 updates, 20,000 nodes, 4,096 children per node, depth 128, 16 KiB strings cut at a
  UTF-8 boundary with invalid UTF-8 replaced, 2 MiB of exported text, URLs only `http`, `https` and `mailto` up to
  2,048 bytes, languages only BCP 47-shaped tags up to 35 characters, finite and bounded coordinates. After every update a reachability pass rebuilds a tree: unreachable nodes
  are dropped, second parents and cycles ignored, depth cut. A rejected update invalidates the mirror until the next
  `reset()`. It is fuzzed (`fuzz/fuzz_cef_tree.cc`).
- **AccessKit's consumer panics on malformed trees**; the bridge filters dangling relations and invalid text selections
  and derives unique ids before anything reaches it.

## Known limits

- Linux only: the bridge creates the AccessKit Unix (AT-SPI) adapter. `Bridge::start()` takes the SDL window for
  adapters that attach to a native window (Windows, macOS); the Unix adapter does not use it.
- `at_active()` turns true when an assistive technology first asks for the tree and never turns false again: the
  adapter's deactivation callback is a no-op.
- Actions travel as an SDL user event (one `SDL_RegisterEvents` per `Bridge`); only click and focus are handled.
- The default texture backend is OpenGL; `LineEdit::draw` uses ImGui internals (see above).
- Font defaults are `Noto Sans` (`Noto Sans Mono` for monospace runs) at 14 px for `LineEdit`; fontconfig substitutes
  its own match when they are not installed. Pass a family to choose another font.

## Tests

- `tests/bridge_test.cc` (GoogleTest, no D-Bus needed): rects in physical pixels, container bounds, duplicate ids,
  stable ids, relation filtering, the AccessKit tree built from a frame (through `accesskit_tree_update_debug`), form
  roles, the invalid state and node languages, text selections, foreign events, actions and text SetValue handed out
  by the next frame.
- `tests/cef_tree_test.cc`: export of a well-formed tree, generations, malformed and over-limit input, cycle and
  second-parent repair, depth and string limits, location updates, content languages.
- `tests/run_cache_test.cc` and `tests/line_edit_test.cc` (headless: a recording `TextureOps`, SDL's dummy video driver):
  rasterising and caching, measuring and metrics, the cold budget, LRU eviction, wrapping, ellipsis and fit, base
  direction, colour emoji layers, `index_at`, `range_boxes` (line breaks, bidi), `word_at`; grapheme, word, visual
  (bidi) and RTL caret moves, Indic backspace, selection, IME composition, paste and copy, masked fields.
- Fuzz: `fuzz/fuzz_cef_tree.cc` (libFuzzer: the export is a tree and every URL is allowed), seeds in
  `fuzz/corpus/cef_tree/`.

## Gates

Every check runs locally, in a pinned toolchain container (`tools/container/Dockerfile`: Debian trixie by digest,
CMake, Ninja, GCC 14, clang 19, GoogleTest, Pango, cairo, fontconfig, libGL and the Noto fonts at exact versions from a
dated Debian snapshot, and SDL3 3.4.2, Dear ImGui v1.93.0-docking and the accesskit-c 0.23.2 release checked against
SHA-256 pins):

```sh
tools/container.sh tools/gate.sh            # format, build, asan, fuzz, licence
tools/container.sh tools/gate.sh asan       # one job
FUZZ_SECONDS=600 tools/container.sh tools/gate.sh fuzz
```

| Job | What |
|---|---|
| `format` | tracked text files: no tabs, trailing blanks or CRs, final newline (no clang-format style is adopted) |
| `build` | GCC 14, every module and test with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, then `ctest` |
| `asan` | clang 19, the same under AddressSanitizer (with leak checking) and UndefinedBehaviorSanitizer |
| `fuzz` | libFuzzer with ASan and UBSan on the CEF mirror for `FUZZ_SECONDS` (default 60), in a cef-only configuration |
| `licence` | `LICENSE` is MIT; Dear ImGui, SDL3 and AccessKit licences as declared; every shared library the tests load comes from a Debian package with an allowed licence (`tools/licences.allow`) |

The library ships no binaries. Consumers pin a commit. `.github/workflows/` mirrors the gate for when the repository is
hosted.

## Licence

MIT, see `LICENSE`.
