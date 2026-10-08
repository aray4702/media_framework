# Docs

Engineering documentation for the media framework. For building, running and using the apps, see the [top-level README](../README.md).

## Design docs ([design/](design/))

Read them roughly in this order. Each one builds on the ones before it.

| Doc | What it covers |
| --- | --- |
| [reqs.md](design/reqs.md) | The original requirements and the scoping decisions made against them: target platforms, media types, sync and performance goals, and what v1 leaves out. |
| [mvp_spec.md](design/mvp_spec.md) | The MVP spec: scope (macOS first), architecture (portable C++17 core plus platform adapters), threads and queues, API and state machine, the sync, render and seek algorithms, metrics and acceptance criteria, milestones, and the issues found in the requirements, each with a proposal (A1–A23). |
| [implementation_skeleton.md](design/implementation_skeleton.md) | A map of the code as built: layers and components, ownership and lifetime, state transitions, startup and shutdown, concurrency, timing and ordering, and the performance-critical path. Start here before changing `core/` or `platform/macos/`. |
| [scene_graph_spec.md](design/scene_graph_spec.md) | The scene document format (v1): tracks, items, transitions, effects, keyframes and compositing, the rules for how a frame and the audio are rendered, validation, and the mapping to OpenTimelineIO. Goes with [schema/](../schema/) and [validate_scene.py](../scripts/validate_scene.py). |
| [rate_mismatch_buffering.md](design/rate_mismatch_buffering.md) | How stages with different, unknown rates exchange frames when there are random bursts and stalls: bounded queues, backpressure vs. dropping, consumer-clock frame selection, adaptive depth, sizing and metrics. Includes where each part lives in the code, a proposal for decode rate control in playback modeled on the camera recorder, and what isn't built yet. |

## Other

- [images/](images/): the logo and the demo screenshot used by the top-level README.
