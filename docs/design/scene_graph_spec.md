# Scene graph format (v1)

A JSON description of what the player plays and the exporter renders: **tracks** of **items** (video, image, text, solid color, audio), **transitions** between items, per-item **effects**, keyframe **animation** of any number, and **compositing** of the tracks. This document defines what every field means and exactly how a frame and the audio are rendered from it. It also defines the validation rules and the mapping to OpenTimelineIO (OTIO) for exchange with other tools.

| File | What it is |
| --- | --- |
| [schema/scene_graph.schema.json](../../schema/scene_graph.schema.json) | JSON Schema (draft 2020-12): structure, types and ranges |
| [schema/examples/two_clips_logo_title.json](../../schema/examples/two_clips_logo_title.json) | Two clips joined by a push, a fading logo, an animated title, a music bed |
| [scripts/validate_scene.py](../../scripts/validate_scene.py) | Checks the schema, then the rules of §6 that a schema can't express |

Why a format of our own: OTIO is the standard for moving an edit between tools, but it deliberately leaves effects, animation and compositing undefined. MLT XML and FCPXML define them, but only as their own engines behave. So this format is what the engine renders, with exact rules, and OTIO is how it is exchanged (§8).

---

## 1. Document

```json
{
  "version": 1,
  "output": { "width": 1920, "height": 1080, "fps": 30, "sampleRate": 48000, "channels": 2, "background": "#000000" },
  "tracks": [ { "kind": "video", "items": [ ... ] }, { "kind": "audio", "items": [ ... ] } ],
  "metadata": { }
}
```

- **`output`** fixes the render size and frame rate. It is the export size. Playback aspect-fits it into the view, and draws at the display's rate with the vsync driver (mvp_spec.md §2.5).
- **`tracks`** (1 to 16): video tracks are composited **in order, the first at the bottom**. Audio tracks, and the audio of video items, are summed.
- **`metadata`**, on the document, a track or an item, is free-form. It is ignored when rendering and kept on OTIO round trips. The engine keeps the document's when it saves a scene; the editor keeps its playhead there, as `"editor": {"playhead": seconds}`.
- **Positions and sizes** are fractions of the output (0 to 1 from the top-left), so a document renders the same at any output size.

## 2. Time

- **Units:** seconds, as a number (`2.5`) or an exact rational string (`"1001/30000"`). The engine converts to microseconds, rounding to nearest. Use rationals for frame-exact NTSC rates.
- **Item placement:** an item occupies `[start, start + duration)` on the timeline.
- **Media items** (video, audio) read their file at media time `m = in + (t − start) × speed`. `speed` > 0, 1 by default. The file must cover `[in, in + duration × speed)`.
- **Duration 0** (video and audio items only) means "to the end of the file": the engine sets `duration = (file length − in) / speed` when it opens the file. Rules that need the item's end (R2, R4, R5, R6) are then checked against that length, so a violation is reported when the file is probed rather than when the document is loaded.
- **Keyframe times are relative to the item's start**, so moving an item moves its animation.
- **The timeline's duration** is the latest end of any item on an enabled track. Where nothing covers the output, it shows `background` and plays silence.

## 3. Items

| `type` | Track | Source | Natural size |
| --- | --- | --- | --- |
| `video` | video | `src` file: video track, plus its audio (the `audio` field: `mute`, `gain`, `pan`) | the decoded frame's pixel size |
| `image` | video | `src` still image (PNG, JPEG, HEIF), decoded once and held | its pixel size |
| `text` | video | `text` with `style` (§4.5), rasterized once and cached | the rasterized text box |
| `color` | video | `color` fill | the whole output |
| `audio` | audio | `src` file's audio track (AAC-LC or MP3; an `.m4a` or `.mp3` file, or a video's), with `gain` and `pan` | — |
| `transition` | either | joins the item before it to the item after it (§5.3) | — |

**A track shows one item at a time.** Items on a track are in start order and don't overlap, except the two items joined by a transition. Use another track to put things on top of each other, e.g. a logo over video.

**Video frames** are selected like the engine's drivers do: for media time `m`, the latest decoded frame with `pts ≤ m`. After the file's last frame, but still within the item, the last frame is held. Before the first frame, the item draws nothing.

## 4. Properties

### 4.1 Geometry: fit, then transform

1. **Fit** the item's natural size (after `crop`, §4.4) into the output:
   - `contain` (default): aspect-fit, whole item visible;
   - `cover`: aspect-fill, cropped by the output edges;
   - `fill`: stretched to the output;
   - `none`: natural pixel size.

   This gives the item's box.
2. **Transform** the box:
   - `anchor` `[ax, ay]` (default `[0.5, 0.5]`) is a point of the box, in fractions of the box;
   - `flipX` (default false) mirrors the item's image left to right within its box (after `crop`), as for a front camera: the box itself doesn't move;
   - `scale` (default 1) and `rotation` (degrees, clockwise, default 0) apply about the anchor;
   - the anchor is then placed at `(x · width, y · height)` of the output (defaults 0.5, 0.5, so centered).

With every default, a `contain` item is centered and aspect-fit, which is exactly what the engine draws today.

### 4.2 Opacity and blending

- **Opacity:** `opacity` (0 to 1, default 1) multiplies the item's alpha. A track's `opacity` multiplies the track's combined image once (§5.1), not each item.
- **Blend modes** (`blend`) apply where a track is drawn onto the tracks below it (§5.1), using the blend of the track's top item (the incoming one during a transition). Inside a track, the two items of a transition are combined by the transition, not by their blend modes. With source `S` and destination `D` both in premultiplied alpha:

| `blend` | Result |
| --- | --- |
| `normal` (default) | `S + D·(1 − αS)` |
| `add` | `S + D` |
| `multiply` | `S·D + S·(1 − αD) + D·(1 − αS)` |
| `screen` | `S + D − S·D` |

Compositing happens in gamma-encoded sRGB, like the current Metal compositor. Video is decoded from BT.709. Linear-light compositing would be a later option (a `colorSpace` field in `output`).

### 4.3 Animation

Any field typed *animatable* in the schema is either a number or `{ "keys": [[t, v], [t, v, easing], ...], "easing": default }`:

- **Before the first key** the value is the first key's. **After the last key** it is the last key's.
- **Between key i and key i+1:** `u = (t − tᵢ) / (tᵢ₊₁ − tᵢ)`, and `v = vᵢ + (vᵢ₊₁ − vᵢ) · ease(u)`. The easing comes from key i's third element, else the property's `easing`, else `linear`.
- **Easings:**
  - `linear`;
  - `hold`: keeps vᵢ until tᵢ₊₁;
  - `easeIn` = `cubicBezier(0.42, 0, 1, 1)`;
  - `easeOut` = `(0, 0, 0.58, 1)`;
  - `easeInOut` = `(0.42, 0, 0.58, 1)`;
  - `{"cubicBezier": [x1, y1, x2, y2]}`, the same definition as CSS: `x1` and `x2` are in [0, 1], and the curve is solved for `x = u`.
- **Values are evaluated at the output time `t`**, never at a source frame's time. So animation is smooth at the output rate even over a 24 fps clip. This is the pull model of mvp_spec.md §2.5.

Animatable fields and their ranges (checked on every key):

| Field | Range |
| --- | --- |
| `opacity` | 0 – 1 |
| `transform.x`, `transform.y` | any (outside 0–1 is off-screen) |
| `transform.scale` | > 0, at most 100 |
| `transform.rotation` | any (degrees) |
| audio `gain` | 0 – 4, linear |
| `pan` | −1 – 1 |
| `colorAdjust.brightness` / `contrast` / `saturation` | −1 – 1 / 0 – 2 / 0 – 2 |
| `blur.radius` | 0 – 0.1 (of the output height) |
| `crop.left` / `top` / `right` / `bottom` | 0 – 1 |

### 4.4 Effects

An item has **at most one effect of each type**, and at most 8 in all. They always apply **in this order**, whatever the list order: `crop` → `chromaKey` → `colorAdjust` → plugin effects (in list order) → `blur`. They act on the item's own image, before fit and transform.

A **video track** can have `effects` too, with the same types, order and limits. They act on the track's combined image (§5.1), in output coordinates: `crop` removes those fractions of the output, and `blur`'s σ is relative to the output height. Their keyframe times are scene time, not item time. A fixed order keeps every effect a single shader pass, except blur, which needs two. RGB values are in 0–1:

| `type` | Parameters | Definition |
| --- | --- | --- |
| `colorAdjust` | `brightness` (0), `contrast` (1), `saturation` (1) | `c = (rgb − 0.5)·contrast + 0.5 + brightness`; `L = dot(c, (0.2126, 0.7152, 0.0722))`; `rgb' = clamp(L + (c − L)·saturation)`. With saturation 1 this is exactly the engine's current `VideoFilter` |
| `blur` | `radius` | Gaussian blur with σ = `radius × output height` pixels, edges clamped |
| `crop` | `left`, `top`, `right`, `bottom` (0) | Removes those fractions of the image. The cropped image is what gets fitted, so it changes the natural size |
| `chromaKey` | `color`, `tolerance` (0.15), `softness` (0.1) | `d` = distance in the CbCr plane from `color`; `α' = α · smoothstep(tolerance, tolerance + softness, d)` |

An effect type the renderer doesn't know is a validation error, not silently skipped, so a document never renders differently from what was authored.

**Plugin effects.** Any other `type` names an effect from an effect plugin, which must be loaded before the document is parsed or opened. A plugin declares its type and its parameters (name, default, minimum, maximum). In a document, each parameter is a key of the effect, animatable like any other number. Parameters that are left out take their defaults. An unknown key, a value outside the plugin's range (R7) or a type no loaded plugin has is a validation error, as for the built-in effects. For example, with the `beauty` plugin (in `plugins/beauty`):

```json
"effects": [{ "type": "beauty", "smooth": 0.6, "whiten": { "keys": [[0, 0], [1, 0.3]] } }]
```

| Plugin `type` | Parameters | Definition |
| --- | --- | --- |
| `beauty` | `smooth` (0.5), `whiten` (0.2), `sharpen` (0.2), all 0–1 | Skin mask `k` from a feathered CbCr box. Bilateral blur (σ = (0.002 + 0.004·smooth) × output height). `rgb' = mix(rgb, blurred, smooth·k)`, then a log lift `log(c·4w + 1) / log(1 + 4w)` by `k`, where w = whiten, then `+ sharpen·(1 − k)·(rgb − blurred)` |

On macOS, a plugin is a `.dylib` that implements [effect_plugin.h](../../platform/macos/include/mf/effect_plugin.h): it gets the image with the effects before it applied, as an RGBA16F texture the size of the item on screen, and writes its result to another. `mf::macos::loadEffectPlugins()` loads plugins from `$MF_EFFECT_PLUGIN_PATH`, `plugins/` next to the executable, an app bundle's `PlugIns`, `~/Library/Application Support/Media Framework/Plugins` and the build tree. The core registry is [effects.h](../../core/include/mf/effects.h).

### 4.5 Text

`style`:

- `font`: `system` (default), `system-bold`, or a family name;
- `size`: line height as a fraction of the output height, default 0.05;
- `color`: default white;
- `align`: `left`, `center` (default) or `right`;
- `box`: a background color, none if absent;
- `maxWidth`: default 0.9 of the output width.

Text wraps at `maxWidth`. It is rasterized at the output resolution, with a box padding of 0.3 × line height. The resulting box is the item's natural size, placed with `fit: none` semantics whatever `fit` says. Today's caption (white, dark box, bottom center) is:

```json
{ "type": "text", "text": "…", "style": { "box": "#0000008c" }, "transform": { "y": 0.96, "anchor": [0.5, 1] } }
```

## 5. Rendering

### 5.1 One video frame at time `t`

1. Start from `background`.
2. For each enabled video track, bottom to top:
   1. For each item active at `t` (one, or two inside a transition), evaluate its properties at `t`, take its frame (§3), apply its effects (§4.4), crop, fit and transform, and multiply by its opacity. The player's global filter (`setFilter`) applies to video and image items here, after their own effects.
   2. Combine them into the track's image, starting from transparent: the item alone, or the pair through the transition (§5.3). Nothing below the track takes part in this step.
   3. Apply the track's effects (§4.4) to that image, multiply by the track's opacity, and blend it onto the result with the top item's blend mode (§4.2).

A track with one item and no effects gives the same result drawn straight onto the result, which is what the engine does; it combines a track on its own only during a transition or when the track has effects.

The output is a pure function of `t` and the decoded frames, which is what makes the leading-clip, vsync and export drivers render identically at the same `t`.

### 5.2 Audio

For each output sample time `t`, sum over every active audio item, and every active video item whose audio isn't muted:

```text
sample(t) = Σ trackGain · itemGain(t) · pan(t) · transitionGain(t) · source(m(t))
```

- **Pan** is a balance: left × `min(1, 1 − pan)`, right × `min(1, 1 + pan)`. Mono sources play on both channels.
- **Mixing precision:** the sum is computed in floating point and clamped at full scale. There is no limiter in v1.
- **Formats:** sources are converted to `output.sampleRate` and `output.channels`. The engine today requires matching formats and plays mismatched clips silent (mvp_spec.md §2.4); resampling is part of implementing this format.

### 5.3 Transitions

A transition item between A and B requires `B.start = A.end − duration`, so the two overlap for its whole length. The progress is `p = ease((t − B.start) / duration)`, from 0 to 1, where `ease` is the transition's `easing`.

The `direction` (`left`, `right`, `up`, `down`) is the way the content moves. Offsets are in output widths (heights for up and down). Let `dir` be −1 for left or up and +1 for right or down:

| `kind` | Video |
| --- | --- |
| `cut` | Duration 0: A, then B |
| `crossfade` | Both drawn, then mixed as `(1 − p)·A + p·B` (premultiplied), so A fades out as B fades in |
| `push` | A offset by `dir·p`, B by `dir·(p − 1)`: both move, and B pushes A out. **This is the engine's current `SlideLeft`/`SlideRight`** |
| `slide` | A stays; B moves in over it from offset `dir·(p − 1)` |
| `wipe` | A stays; B is revealed by an edge moving in `direction`, covering fraction `p` of the output |

Audio across the transition (`audio`), using the linear progress `q = (t − B.start) / duration`:

| `audio` | A gain | B gain | Notes |
| --- | --- | --- | --- |
| `equalGain` (default) | `1 − q` | `q` | The level stays constant for correlated audio. It's the engine's current crossfade |
| `equalPower` | `cos(q·π/2)` | `sin(q·π/2)` | Loudness stays constant for unrelated audio, e.g. music into speech |
| `cut` | 1 until `q = 0.5` | 1 after | |

**Limit:** a transition is at most half of each item it joins. So on one track, at most two items are ever active, which is the engine's two-lane model.

## 6. Validation

The **schema** checks structure, types, enumerations, required fields and fixed ranges. After it passes, these **rules** must also hold. `validate_scene.py` checks all of them except the runtime ones:

| # | Rule |
| --- | --- |
| R1 | Every `id` is unique across the document |
| R2 | A track's non-transition items are in start order and don't overlap, unless a transition sits between them |
| R3 | A transition sits between two items of its track. It never starts or ends a track, and never follows another transition |
| R4 | The items around a transition overlap by exactly its `duration` (to 1 µs): `B.start = A.end − duration` |
| R5 | A transition is at most half of each item it joins. A `cut` has duration 0 |
| R6 | Keyframe times strictly increase and lie within `[0, duration]` of their item |
| R7 | Every animated value, including every key, is within its range (§4.3). A `crop` never removes the whole width or height |
| R8 | At least one enabled track has an item |
| R9 *(runtime)* | Each `src` opens and has the needed track: video for `video`, audio for `audio`, a decodable image for `image` |
| R10 *(runtime)* | A media item's `in` lies inside its file (for duration 0 as well). If the file ends before `in + duration × speed`, the item holds its last frame (video) or goes silent (audio) |
| R11 *(runtime)* | At most 8 video and audio items play at once. Preroll doesn't count toward the limit: an item's decoder starts 2 s before its start when a lane is free for it, and later otherwise. Each item playing needs its own decoder, and the platform's hardware limits still apply (e.g. two 4K H.264 streams at once on Apple silicon) |
| R12 | An item or a track has at most one effect of each type and 8 in all, each built in or from a loaded effect plugin (§4.4); only video tracks have effects |

A document that fails R1–R8 or R12 is rejected by `open()` or `Exporter::start()` with `InvalidArgument`, before anything is decoded. R9–R11 fail asynchronously through `onError`, like media errors today.

Compatibility: `version` changes only for changes that alter rendering. New optional fields, and new `effect`, `kind` or `blend` values, are added within v1. A renderer that meets an unknown value rejects the document instead of guessing.

## 7. Implementation

The engine plays and exports scenes: `Player::open(const Scene&, ...)`, `Exporter::start(const Scene&, ...)`, and `mf::macos::loadScene(path, ...)`, which reads a document and resolves each `src` relative to it. `serializeScene(scene, srcFor)` writes one: the inverse of `parseScene`, with fields at their defaults left out and each `src` mapped by `srcFor` (the editor writes paths inside the document's folder relative to it). `Player::open(const MediaSource&, ...)` plays one file as a scene of one video item with duration 0 and an output taken from the file.

| Part | Where |
| --- | --- |
| JSON reader (strict RFC 8259, key order kept, depth limit) | [core/src/json.cpp](../../core/src/json.cpp) |
| Parsing, the schema's checks, rules R1–R8 and R12, keyframes and easing; writing documents | [core/src/scene.cpp](../../core/src/scene.cpp) |
| Lanes and "what is visible at `t`" (transition offsets, clips, fades; audio fades) | [core/src/layout.cpp](../../core/src/layout.cpp) |
| Per-item frame selection; `composeAt(t)` evaluates every visible item into a `ComposedLayer`, and a track that is combined on its own into a `ComposedGroup` (its effects, opacity, blend) | [core/src/composition.cpp](../../core/src/composition.cpp) |
| Audio mix: every sounding item, resampled and sped up by reading at its media time | `AudioStage` in [core/src/pipeline.cpp](../../core/src/pipeline.cpp) |
| Drawing: fit, transform, blend modes, effects, blur passes, wipe clips, styled text; a grouped track is drawn into its own texture first, then onto the canvas | [platform/macos/src/metal_compositor.mm](../../platform/macos/src/metal_compositor.mm) |
| Effect plugins: the core's registry of types and parameters; the macOS plugin interface and loader; the beauty plugin | [core/src/effects.cpp](../../core/src/effects.cpp), [platform/macos/include/mf/effect_plugin.h](../../platform/macos/include/mf/effect_plugin.h), [platform/macos/src/effect_plugins.mm](../../platform/macos/src/effect_plugins.mm), [plugins/beauty/beauty.mm](../../plugins/beauty/beauty.mm) |
| Image items | [platform/macos/src/image_loader.mm](../../platform/macos/src/image_loader.mm) (ImageIO, EXIF orientation) |

- **Lanes.** Each video or audio item gets a lane: its own decoders and queues. Lanes are assigned greedily in start order, and an item can reuse a lane once the lane's previous item has ended, counting from 1 s before the new item starts. That's the fewest lanes possible. For clips joined by transitions this gives the two alternating lanes the timeline engine used.
- **Drivers.** `Auto` uses the leading-clip driver for a scene with a single video item and nothing else visible, and the vsync driver otherwise. Export always uses the fixed grid at `output.fps`.
- **Canvas.** The output size is the canvas. Playback letterboxes it into the view, and export writes it at exactly that size.
- **Global filter.** `Player::setFilter` still applies on top of every video and image layer, after the item's own effects.
- **Not implemented:** linear-light compositing, and the OTIO adapter (§8 is the mapping to build it from).

## 8. OpenTimelineIO mapping

OTIO carries the edit: tracks, clips, gaps, transitions and time ranges. Everything OTIO can't express goes into `metadata["mf"]`, so a document survives a round trip through OTIO (and through tools that keep metadata) unchanged. Tools that don't read it still see the right cuts, dissolves and timing.

| Scene graph | OTIO |
| --- | --- |
| Document | `Timeline`, `global_start_time` 0. `metadata.mf = {version, output}` |
| Video / audio track | `Track(kind = Video / Audio)` in `timeline.tracks` (a `Stack`: later tracks on top, as here). `enabled`, `opacity`, `gain`, `effects` go to `metadata.mf` |
| `video`, `audio` item | `Clip` with `ExternalReference(target_url = src)`, `source_range = (in, duration × speed)`. Speed ≠ 1 is a `LinearTimeWarp` effect |
| `image` item | `Clip` with `ExternalReference` to the image, `source_range = (0, duration)` |
| `color` item | `Clip` with `GeneratorReference(generator_kind = "SolidColor", parameters = {color})` |
| `text` item | `Clip` with `GeneratorReference(generator_kind = "mf.text", parameters = {text, style})` |
| Space between items | `Gap`: OTIO tracks are sequential, so gaps are explicit |
| `transition` | `Transition`: `crossfade` → `SMPTE_Dissolve`, others → `Custom` with `metadata.mf = {kind, direction, easing, audio}`. `in_offset = out_offset = duration / 2` |
| `effects` | `Effect(effect_name = "mf.colorAdjust", …)` with the parameters in its metadata |
| `transform`, `opacity`, `blend`, `fit`, keyframes, `id` | `clip.metadata.mf`. `id` is also the clip `name` |
| Times | `RationalTime(value, rate)`, using `output.fps` as the rate when frame-exact, otherwise rate 1 000 000 |

**Transition geometry.** Here, B starts `d` before A ends. In OTIO, clips sit end to end with the transition centered on the cut:

- **Export:**
  - the cut is at `c = A.end − d/2`;
  - A's OTIO range ends at `c`, so its duration is `A.duration − d/2`;
  - B's starts at `c`, with its `source_range.start_time = B.in + (d/2)·speed` and duration `B.duration − d/2`;
  - the transition's `in_offset` (A's media past the cut) and `out_offset` (B's media before it) are both `d/2`.
- **Import** reverses this, and accepts unequal offsets:
  - `d = in_offset + out_offset`;
  - A is extended by `in_offset`;
  - B starts `out_offset` earlier, both on the timeline and in its media.
- **Handles:** if a clip's `available_range` has too little media for its offset, the import shortens the transition and warns.

**Other tools.** Adapters for FCPXML, AAF, CMX 3600 EDL and Premiere XML produce the cuts, dissolves and clip timing. Generator clips (text, color) and `Custom` transitions usually arrive as placeholders or as cuts, and keyframes and effects in `metadata.mf` are only preserved by tools that keep OTIO metadata. For a lossless exchange between our own tools, OTIO's `.otio` JSON is enough on its own.
