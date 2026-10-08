# Producer/Consumer Rate Mismatch: Buffering, Bursts and Stalls

How to move frames between two stages whose rates differ by an unknown amount, when the producer can send bursts and the consumer can slow down without warning. The first sections describe the general design. Section 8 shows where each part lives in this framework. Section 9 proposes decode rate control for playback, modeled on the camera recorder, and section 10 lists what isn't built yet.

## 1. Problem

A producer (demuxer, decoder, compositor) hands items to a consumer (decoder, compositor, display, encoder).

- **Unknown mean mismatch.** The producer's long-run rate `λ` and the consumer's long-run rate `μ` are not known ahead of time and can drift (content frame rate vs. display refresh, decoder speed vs. resolution, thermal throttling).
- **Random bursts.** The producer sometimes sends many items at once, for example a decoder flushing reordered B-frames, a demuxer reading a large interleaved chunk, or a stage catching up after its thread was descheduled.
- **Random stalls.** The consumer sometimes stops for a while: a GPU hiccup, a page fault, a slow encoder call, a window being dragged.

Requirements:

1. Memory must stay bounded, with a stated worst case.
2. Latency must stay bounded. A queue that only grows turns a rate mismatch into ever-growing delay.
3. Short bursts and stalls must be absorbed with no visible effect.
4. A sustained mismatch must be resolved by a defined policy, not by accident.
5. Every drop and repeat must be observable (metrics).

## 2. Key insight: two different problems

| | Transient jitter (burst, stall) | Sustained mismatch (`λ ≠ μ` on average) |
| --- | --- | --- |
| Time scale | ms to a few hundred ms | seconds and longer |
| Mean rate | Equal; only the variance is non-zero | Not equal |
| Fix | **Buffer**: store the excess, drain it later | **Rate policy**: backpressure, drop or repeat |
| Can a buffer fix it? | Yes, if it is big enough | **No.** Any finite buffer eventually fills (`λ > μ`) or empties (`λ < μ`) |

By Little's Law, `latency = depth / rate`. The buffer depth sets the latency you pay. Its only job is to cover **variance**. The **mean** difference has to be removed somewhere else.

So the design has two layers:

```text
 Producer ──► [ bounded buffer ] ──► Consumer
                 │  absorbs jitter      │  runs on its own clock
                 │  caps memory         │  selects by timestamp
                 ▼                      ▼
        overflow policy        underflow policy
     (backpressure / drop)     (wait / repeat)
```

## 3. Data structure

### 3.1 Bounded queue capped by count, bytes and duration

A count cap alone is not enough for media. One 4K frame is about 12 MB and one compressed packet can be up to 16 MB, so "60 items" could mean 1 GB. Cap each queue by whichever limit is reached first:

```cpp
bool full() const {
  if (items.empty()) return false;                  // always accept one item
  return items.size()  >= caps.count     ||
         bytes          >= caps.bytes     ||
         back().pts - front().pts >= caps.durationUs;
}
```

- **Count** limits the number of bookkeeping slots and pool buffers.
- **Bytes** limits memory. This is the cap that gives a real worst case.
- **Duration** limits latency directly. This is the cap that matters when rates are unknown, because "300 ms buffered" means the same thing at 24 fps and 120 fps, and "8 frames" does not.
- **An empty queue always accepts one item**, so a single oversized item can't block the pipeline for good.

### 3.2 Lock-free SPSC ring for the real-time edge

Where the consumer is a real-time callback (audio render, display link), it must never take a lock or allocate. Use a single-producer/single-consumer ring:

- Preallocated storage with a power-of-two capacity.
- Monotonic 64-bit `write` and `read` indices (`index & (cap-1)` gives the slot), so wraparound is never ambiguous.
- Producer: load `read` (acquire), write the slots, store `write` (release).
- Consumer: load `write` (acquire), read the slots, store `read` (release).
- **Flush without a lock:** only the consumer moves `read`. To flush, the producer publishes a record `{generation, writeIndex}` (through a seqlock). On its next callback the consumer jumps to that index.

### 3.3 Preallocated item pool

Decoded frames come from a fixed pool of refcounted buffers (for example `CVPixelBufferPool`), never `malloc` on the hot path. Queues carry handles, not copies. An empty pool is also a natural backpressure signal on the decoder.

### 3.4 Where to put the depth

Buffer **where items are cheap**. A compressed packet is about 100× smaller than the decoded frame, so a 2 s packet queue in front of the decoder costs a few MB, while 2 s of decoded 4K would cost about 1.4 GB. The usual shape:

```text
demux ──[deep: ~2 s of packets]──► decode ──[shallow: ~4 frames]──► compose ──[~4]──► present
```

The deep, cheap queue absorbs bursts and I/O stalls. The shallow decoded queues only smooth out per-frame timing.

## 4. Overflow policy (queue full)

The right action depends on whether every item has to arrive.

| Stage type | Policy when full | Why |
| --- | --- | --- |
| Lossless (demux → decode, export, file write) | **Backpressure.** `tryPush` fails, the producer stops, and it is woken when space frees up | Every item has to arrive, so time is cheaper than data |
| Real-time output (compose → display) | **Drop, latest wins.** Replace the waiting item | An old frame shown late is worse than one skipped |
| Compressed data that must be dropped | Drop **non-reference** frames first. If a reference frame is dropped, drop up to the next keyframe | Dropping a reference frame corrupts everything that depends on it |

Implement backpressure as **non-blocking push plus a wake hook**, not as a blocking push. A thread blocked inside `push` can't service its other queues, react to a seek or shut down. With `tryPush` returning false, the scheduler parks the producer, and the queue's `onSpace` hook wakes it when the consumer pops.

**Hysteresis.** If the producer is resumed the moment one slot frees up, it wakes on every pop. Use a high watermark (stop) and a low watermark (resume), for example resume at 50% full, so wakeups happen in batches.

## 5. Underflow policy and rate matching: select by timestamp

This is what handles an **unknown** mean mismatch without measuring it.

Do not let the consumer pop items in FIFO order. Run it on its **own clock** (vsync, audio device clock) and, on each tick, ask *which item belongs at time T?*

```cpp
// Called once per consumer tick for presentation time T.
const Frame* selectAt(Time T) {
  // Producer faster: skip every frame already superseded at T.
  while (queue.size() >= 2 && queue.peek(1).pts <= T)
    release(queue.pop());                 // count as rate-capped, not dropped

  if (queue.empty() || queue.peek(0).pts > T)
    return last;                          // producer slower or stalled: hold/repeat

  last = queue.peek(0);                   // latest frame at or before T
  return last;
}
```

- **Producer faster (`λ > μ`):** superseded frames are skipped automatically. Memory stays bounded because the consumer drains the queue at the producer's rate even though it shows fewer frames.
- **Producer slower (`λ < μ`):** the last frame is held. Nothing needs to be synthesized.
- **Neither side needs to know the other's rate.** The timestamps hold all the information.

Two details matter in practice:

1. **Quantize to output slots.** Compare frames by the vsync slot they would land in, `slot = round((pts − anchor) / period)`, not by raw time gaps. A naive `pts − lastPts < period` check drops about ⅓ of frames for 60 fps content with millisecond-rounded timestamps on a 60 Hz display.
2. **Separate on-purpose skips from failures.** Skipping a 120 fps frame on a 60 Hz display is correct behavior. Count it as *rate-capped*. Count only frames that missed their deadline as *dropped*, so the dropped-frame metric means something.

For a lossless consumer (export), the clock is the timeline, not wall time. The consumer waits until every input is exact at T, so the producer's speed changes only how long the export takes.

## 6. Adaptive target depth

Fixed caps must be sized for the worst case, which means paying the worst-case latency all the time. When burst and stall sizes are random, track the jitter and size the target depth from it, the way a jitter buffer does in WebRTC or VoIP.

```text
on each arrival:
    d       = actual_interarrival − expected_interarrival
    mean   ← mean + α (|d| − mean)                 // EWMA, α ≈ 1/16
    var    ← var  + α ((|d| − mean)² − var)
    target  = clamp(mean + k·sqrt(var), MIN_LATENCY, MAX_LATENCY)   // k ≈ 3–4
```

Control the queue around `target`:

- **Grow quickly.** On an underflow, raise `target` right away (for example by the length of the stall just seen). An underflow is visible, so react to it at once.
- **Shrink slowly.** If depth stays above `target` for a long window (seconds), remove the extra by skipping one frame now and then, or by playing audio slightly fast with time-stretching. Do it gradually, so the correction itself isn't visible.
- **Hard caps still apply.** `MAX_LATENCY` and the byte cap from §3.1 are the worst case. The adaptive target only decides how much of that budget to use.

**Telling a burst from a real mismatch.** Track `λ̂` and `μ̂` as EWMAs over a long window (5–10 s). If `λ̂ > μ̂` for that long, it is a mismatch, not a burst. Escalate: skip on a fixed pattern (every Nth frame), or tell the producer to reduce its work (lower resolution, skip non-reference frames when decoding, lower the encoder rate).

## 7. Sizing example

Content at 60 fps, decoder bursts of up to 8 frames, consumer stalls of up to 50 ms, latency budget of 100 ms on the decoded side.

- Burst: 8 frames ≈ 133 ms of content arriving at once. Most of it should be absorbed **upstream** (the packet queue simply isn't read for a while). The decoded queue only needs to hold what the decoder emits before backpressure stops it.
- Stall: 50 ms × 60 fps = 3 frames arrive while the consumer is stopped.
- Decoded queue: 3 frames for the stall + 1 in flight = **4 frames**, about 50 MB at 4K NV12.
- Packet queue: 2 s, which covers I/O hiccups and decoder catch-up. Capped at 32 MB for video.

Worst-case memory is the sum of each queue's byte cap plus the pool. State that number and check it with a peak-memory metric.

## 8. How this framework implements it

| Concept | Where |
| --- | --- |
| Count/bytes/duration-capped queue, non-blocking, with wake hooks | `BoundedQueue<T>` in [bounded_queue.h](../../core/src/bounded_queue.h) |
| Queue sizing: deep cheap packets, shallow decoded frames | [pipeline.h](../../core/src/pipeline.h): `videoPackets {60, 32 MB, 2 s}`, `audioPackets {120, 1 MB, 2 s}`, `frames {4}`, `composed {4}` |
| Lock-free SPSC ring, consumer-only read index, seqlock flush | `AudioRing` in [audio_ring.h](../../core/include/mf/audio_ring.h) |
| Consumer runs on its own clock and picks the latest frame at or before `t` | `VsyncDriver::step` in [vsync_driver.cpp](../../core/src/vsync_driver.cpp), using the `FrameSampler` |
| Hold instead of showing a stale frame after a missed deadline | `holdUntilExact_` in [vsync_driver.cpp](../../core/src/vsync_driver.cpp) |
| No duplicate output when nothing changed (30 fps on 60 Hz → 30 presents) | `VsyncDriver::unchanged` |
| Lossless consumer waits for exact inputs | [export_driver.cpp](../../core/src/export_driver.cpp) (`allExactAt(t)`) |
| Slot quantization; rate-capped vs. late drops counted separately | [mvp_spec.md](mvp_spec.md) §4, assumptions A9 and A19 |
| Latest-wins at the presenter | [mvp_spec.md](mvp_spec.md) (presenter busy → replace the waiting frame) |
| Stale items after a seek are dropped by serial | [implementation_skeleton.md](implementation_skeleton.md) |
| Real-time producer into a slower lossless consumer: keep frames on a rate grid, thin evenly, step the rate down and up | `SegmentRecorder` in [segment_recorder.cpp](../../core/src/segment_recorder.cpp) (`admitLocked`, `thinLocked`, `adaptRateLocked`) |

The master clock is the **audio** that has actually been heard, not wall time. Video selection follows audio, so a stall in audio output pauses video selection too and A/V sync is kept.

## 9. Proposal: decode rate control for playback

**Status: built** for both live drivers. The controller is `DecodeRate` in [decode_rate.h](../../core/src/decode_rate.h). `DecodeControl` in [drivers.h](../../core/src/drivers.h) runs it for the Vsync and LeadingClip drivers, and the video decode stage applies the steps. This section describes how playback responds when a lane's decoder is slower than real time on average, reusing the control logic of the camera recorder. It fills the "sustained mismatch" gap listed in §10.

### 9.1 The camera recorder solves the mirror image

`SegmentRecorder` takes frames from a camera, which can't be paused, and gives them to an encoder, which must write every frame it gets but can fall behind. It uses three mechanisms:

| Mechanism | What it does | Code |
| --- | --- | --- |
| Rate grid | Keeps a frame only when it is due on the current rate's grid, with a quarter frame of leeway. Bursts and frames from a faster camera are skipped, not dropped. After a stall, the grid restarts at the next frame | `admitLocked` |
| Even thinning | When too many frames wait, drops the one whose loss leaves the smallest gap, so losses spread out instead of freezing the picture. Never the first or the newest | `thinLocked` |
| Rate steps | If the queue stays at least half full for 1 s, steps down to the next lower rate (30, 20, 15, 10 fps for a 30 fps camera), at most one step per second. If it stays nearly empty for 3 s, steps back up. If a step up doesn't hold, the wait before the next step up doubles, up to 30 s | `adaptRateLocked` |

All its timing is measured in **capture time**, so a burst of frames delivered at once never looks like a slow encoder.

### 9.2 Playback has the opposite shape

| | Camera recorder | Playback (decode → compose) |
| --- | --- | --- |
| Producer | Camera: real time, can't be paused | Decoder: reads a file, can be paused for free |
| Consumer | Encoder: lossless, no deadline | Compositor: real time, on a vsync deadline |
| A full queue means | The consumer is behind | The decoder is comfortably ahead (healthy) |
| The failure | Overflow: frames must be dropped | Underflow: the compositor finds no frame for `t` |
| Today's response | Thin, then step the rate down | Hold the picture (`holdUntilExact_`), or show late frames and count `lateLayers` |

Playback already handles **transient** jitter. The compositor selects by timestamp (§5), backpressure pauses the decoder at no cost, the 2 s packet queue absorbs I/O stalls, and the 4-frame queue absorbs decoder bursts such as a reorder flush.

It does not handle a **sustained** shortfall, for example two 4K streams on hardware that can decode only one in real time. Nothing reduces the decode load, so the lane stays late. Today the vsync driver can then hold the whole picture for as long as the item stays behind.

### 9.3 Design: a decode ladder stepped evenly across lanes

Keep the recorder's controller and timing rules, but change what a step does and which signals drive it, and apply each step evenly across lanes.

**Steps.** A step lowers the decode load, not the output frame rate:

| Step | Decodes | Cost and effect |
| --- | --- | --- |
| 0 | Every frame | Normal |
| 1 | Every frame except disposable ones (frames nothing else references, usually B-frames) | About half the load with 2 B-frames per anchor. Never corrupts a later frame |
| 2 | Keyframes only | A slideshow at the GOP interval. Last resort |

A further step that decodes at reduced resolution could sit between 1 and 2, if VideoToolbox supports it for the stream (see §9.8).

**Signals.** An *active* lane is one decoding a video item, from its preroll start until its end of stream, or showing one. The meaning of a lead is inverted compared with the recorder:

| Signal | Measured as | Recorder equivalent |
| --- | --- | --- |
| **Behind:** step down | Any active lane's visible item is late. Vsync driver: not exact at the compositor (`!exactAt(i, t)`). LeadingClip driver: the frame it is about to hand over has a time the master clock has already passed, so T3 will drop it | Queue at least half full |
| **Ahead:** step up | No lane is behind, and every lane at the highest step has decoded at least two frame periods past `t`, or to its item's end | Queue holding at most one frame |

The lead is measured as the timeline time of the newest frame the decode stage handed over (`Lane::decodedToUs`). An earlier version used "the frame queue is full", but the compositor samples it right after taking a frame out, so it was almost never full at that moment and the lanes never stepped back up.

**Stepping evenly.** Lanes usually share one hardware decoder, so a late lane is usually late because of the load of all of them. Stepping down only that lane would put all the quality loss on one picture while the others stay perfect. Like the recorder's even thinning, the controller spreads the loss instead:

- There is one controller for all lanes, not one per lane.
- A step down lowers every active lane at the **lowest** step by one. A step up raises every lane at the **highest** step by one.
- Active lanes are therefore never more than one step apart. When all are level, they move together.

For example, with three active lanes:

| Event | Steps |
| --- | --- |
| Start | 0, 0, 0 |
| Behind | 1, 1, 1 |
| A new item joins at step 0, because the step-down was too recent to build momentum (below) | 1, 1, 0 |
| Behind again | 1, 1, 1 (only the lane at the lowest step moves) |
| Ahead for the calm period | 0, 0, 0 |

**Momentum at a new item.** When a lane starts its next item, the item starts at the lanes' recent level, neither at 0 nor at whatever step the previous item happened to end on:

- Starting at 0 would make a cut into another expensive item late again, and the cut is where lateness shows most.
- Copying the previous item's step would carry a brief excursion over to an item that may be cheaper.

The controller keeps a momentum value `m`, an exponential moving average of the mean step of the active lanes, measured in timeline time with a time constant `kMomentumUs` of 5 s. A new item starts at `round(m)`, clamped to stay within one step of the other active lanes:

- After about 3.5 s at a step (`τ · ln 2`), `m` passes the halfway mark, and new items start at that step.
- A shorter excursion fades. After 1 s at step 1, `m` is about 0.18, so the next item starts at 0.
- Recovery decays the same way. About 3.5 s after the lanes return to 0, new items start at 0 again.
- The average uses rounding, not rounding down, because an average only approaches the current level and never reaches it. Rounding down would never carry a step over.
- Starting one step too low costs little: if the item is still too expensive, the bounded hold (§9.4) steps the lanes down within about 100 ms. Starting too high costs at least the 3 s calm period, which is why only a sustained level carries over.

**Timing.** The recorder's rules, measured in **timeline time** at the compositor, so that a reorder flush or a single stall doesn't trigger a step. These starting values should be tuned with the metrics in §9.6:

| Rule | Recorder | Playback (starting value) |
| --- | --- | --- |
| Step down after being behind for | 1 s | 500 ms, or at once when a hold hits its limit (§9.4) |
| Minimum gap between steps down | 1 s | 1 s |
| Step up after being ahead for | 3 s | 3 s |
| A failed step up doubles that wait, up to | 30 s | 30 s |
| Momentum time constant | — | 5 s |

```cpp
// Each composition tick at timeline time t, over the active video lanes. Live drivers only (Vsync, LeadingClip).
void adaptDecode(DecodeRate& r, std::vector<LaneStep>& lanes, bool behind, bool ahead, bool holdExpired, int64_t t) {
  r.behindSince = behind ? (r.behindSince < 0 ? t : r.behindSince) : -1;
  r.aheadSince  = ahead  ? (r.aheadSince  < 0 ? t : r.aheadSince)  : -1;
  bool slow = holdExpired || (r.behindSince >= 0 && t - r.behindSince >= kBehindForUs);
  int lo = minStep(lanes), hi = maxStep(lanes);
  if (slow && lo < kMaxStep && (r.lastStep < 0 || t - r.lastStep >= kStepGapUs)) {
    if (r.lastStepUp >= 0 && t - r.lastStepUp < 2 * r.stepUpAfter) r.stepUpAfter = std::min(2 * r.stepUpAfter, kMaxStepUpAfterUs);
    for (LaneStep& l : lanes) l.step += l.step == lo;  // the least reduced lanes: never more than one step apart
    r.lastStep = t;  r.behindSince = r.aheadSince = -1;
  } else if (hi > 0 && r.aheadSince >= 0 && t - r.aheadSince >= r.stepUpAfter && t - r.lastStep >= r.stepUpAfter) {
    for (LaneStep& l : lanes) l.step -= l.step == hi;  // the most reduced lanes
    r.lastStep = r.lastStepUp = t;  r.aheadSince = -1;
  }
  r.momentum += (1 - std::exp(-double(t - r.lastTick) / kMomentumUs)) * (meanStep(lanes) - r.momentum);
  r.lastTick = t;
}

// The step a lane's next item starts at: the recent level, within one step of the other active lanes.
int startStep(const DecodeRate& r, const std::vector<LaneStep>& others) {
  int s = int(std::lround(r.momentum));
  return others.empty() ? s : std::clamp(s, maxStep(others) - 1, minStep(others) + 1);
}
```

**Where it runs.**

- The composition stage owns the controller, because it is the only stage that knows whether a lane is late. It publishes each lane's step, and the start step for new items, as atomics.
- The video decode stage (T2) reads its lane's step and skips packets before handing them to the decoder. Skipping before decoding is the point: a frame dropped after decoding has already cost the most expensive work.
- When T2 starts a lane's next item, which happens during that item's preroll, before the compositor sees it, it takes the published start step. The controller then counts the lane as active at that step.
- Step 2 must start dropping at a keyframe boundary, following the reference-frame rule in §4. On the way down it drops the rest of the current GOP. On the way up it resumes at once if nothing has been skipped since the last keyframe it decoded, because every reference is still intact. Otherwise it resumes at the next keyframe.
- **Step 2 is paced by the playhead.** Skipping costs nothing, so a keyframes-only lane would race through the rest of its item to the end of stream and leave no frames for a step back up to apply to: the rest of the item played as a slideshow. The decode stage therefore skips on to the next keyframe only once the decoder is empty and the lane has decoded less than 1 s (`kKeyframeLeadUs`) past the playhead (`Context::playheadUs`). It re-checks on a timer, at least every 100 ms. The lane stays 1 to 2 s ahead, and it usually waits right after a keyframe, where a step up can take effect at once.
- An earlier version paced on the lane's frame queue emptying. That held for the Vsync driver, but the LeadingClip driver moves frames on into its output queue at once, so the lane still raced to the end.

**Disposable packets.** `Packet` gains a `disposable` flag, false by default, so demuxers that don't set it (and the test fakes) behave as today. The macOS demuxer sets it when the sample's `kCMSampleAttachmentKey_IsDependedOnByOthers` attachment is false, or from `nal_ref_idc == 0` in the H.264 NAL header. Skipping disposable frames keeps the decoder's reorder buffer correct, because removing frames never increases how many must be held back.

**When it is off.**

- **Export.** It is lossless, so it never skips a frame; a slow decoder only makes the export take longer.
- **Seeks.** The exact frame for a seek target must be decoded, so the controller pauses while seeking, and a seek resets every lane to step 0. It keeps the momentum, so the items started after the seek still begin at the recent level.

### 9.4 A bounded hold

The hold in `VsyncDriver` stays, because it still keeps a late first frame at a cut off screen. But it gets a limit, `kMaxHoldUs`, of a few frame periods (about 100 ms):

- Within the limit, the picture is held as today.
- Past the limit, the driver shows the late frames instead of freezing, and tells the controller the hold expired. The controller then steps the lanes down at once, without waiting for `kBehindForUs`.

This is the "react to an underflow at once, recover slowly" rule from §6. The hold covers the short glitch, the ladder removes the cause, and the output can no longer freeze indefinitely.

### 9.5 What is not carried over

- **Thinning decoded frames.** Playback never overflows, because backpressure is free. Dropping a decoded frame wastes the work that was the problem.
- **The rate grid** (`nextDueUs`). The compositor's timestamp selection (§5) already chooses which frames to show, for any mix of content and display rates.
- **A full queue as the alarm.** It means the opposite here. The alarm is lateness at the compositor.

### 9.6 Metrics

As in the recorder, which counts `dropped`, `skipped` and `rateReductions` separately, new counters stay separate from late drops, so the dropped-frame rate in §11 keeps its meaning:

| Metric | Meaning |
| --- | --- |
| Decode skips (`decodeSkips`) | Packets skipped by the ladder |
| Step changes (`decodeStepDowns`, `decodeStepUps`) | Steps down and up |
| Time reduced (`decodeReducedUs`) | Timeline time with some lane above step 0 |
| Hold expiries (`holdExpiries`) | Holds that reached `kMaxHoldUs` |

The counters are totals in `MetricsReport`, not per lane. A per-lane breakdown and the step each new item started at are not recorded yet.

### 9.7 Testing

`fake::Clip::decodeUs` gives the fake decoder a per-frame decode time on the harness clock, with one decode engine shared by all lanes, like a hardware decoder. `fake::Clip::disposable` marks odd non-key frames disposable. The tests in [core_tests.cpp](../../core/tests/core_tests.cpp):

- **Controller (`decode_rate_*`):** even stepping when only one lane is late, lowest and highest lanes first, the 1 s gap between steps, a step up that doesn't hold doubling the wait, an expired hold stepping down at once, and momentum carrying only a sustained level, kept across a seek.
- **LeadingClip playback:** a decoder that keeps up never steps. A single clip needing 150% of the decoder, with or without disposable frames, plays to its end, steps back up, and shows full-rate frames again. Without the ladder it got no further than 0.2 s in 9 s.
- **Vsync playback:** a decoder that keeps up never steps. Two stacked clips needing 150% of the decoder step down, catch up within a frame of the playhead, try a step up, and undo it. Without disposable frames the lanes reach keyframes only, and still step back up to full-rate frames within the clip. Cuts without preroll on a busy decoder are held at most `kMaxHoldUs`.
- **Seek:** after stepping down, a seek to a disposable frame shows that exact frame.
- **Export:** a slow decoder decodes every frame, and every exported frame is exact.
- **macOS adapter:** on real files, skipping the frames the demuxer marks disposable corrupts none of the others, and the rest still come out in order.

Not covered by a test yet: the decode stage giving a new item the published start step (the controller's `startStep` is unit-tested).

### 9.8 Open questions

- **Momentum across very different items.** A cut from 4K to 720p carries a step the cheaper item doesn't need, until the 3 s calm period lifts it. Weighting momentum by each item's pixel rate would avoid that.
- **Lanes of very different cost.** Even stepping degrades a cheap 720p lane as much as the 4K lane that caused the lateness. If the metrics show this matters, the evenness could be weighted by each lane's decode cost.
- **Step 2 with long GOPs.** A 4 s GOP makes keyframes-only a 0.25 fps slideshow. It may be better to stop at step 1 plus reduced resolution.
- **Reduced-resolution decode.** Not yet verified for H.264 on VideoToolbox.
- **Catching up in order (LeadingClip).** The LeadingClip driver hands every frame over in order, so after it falls behind, the late frames already decoded and queued must still be decoded and dropped. In the 150% single-clip test that backlog keeps the lane behind past step 1, down to keyframes only, even though step 1 alone would keep up. Dropping late packets before decoding would let it stop at step 1.
- **Step 1 on streams without disposable frames.** It skips nothing, so the lanes spend at least `kBehindForUs` plus the gap on a step that can't help before reaching keyframes only. The decode stage could report that a lane has no disposable frames, so the controller passes over step 1 for it.
- **The cost of probing upward.** In the 150% test, the step up that doesn't hold leaves the layers up to about 250 ms behind for a moment before the step down. The doubling backoff makes this rarer, but each try is still visible.
- **Adaptive preroll.** Like the recorder's step-up backoff, the 2 s preroll lead could follow each lane's measured time to first frame. It interacts with lane assignment, which uses a fixed `kPrerollUs`, so it waits until the ladder exists and has data.

## 10. Not built yet

- **Adaptive target depth (§6).** Queue caps are fixed today. A jitter-based target would lower the steady-state latency of live preview.
- **Sustained-mismatch detection.** There is no `λ̂`/`μ̂` tracking. For live playback, §9's decode ladder skips non-reference frames when the decoder can't keep up.
- **Watermark hysteresis.** `onSpace` fires on every pop. Batching the wakeups would reduce scheduler churn under heavy load.

## 11. Metrics

| Metric | Meaning |
| --- | --- |
| Dropped-frame rate | late drops / (presented + late drops). Excludes rate-cap skips, seek flushes and hidden-window drops |
| Rate-capped count | Frames skipped on purpose because the content rate is above the output rate |
| Repeats / underflows | Ticks where the consumer had no new frame (producer slow or stalled) |
| Queue depth histogram (time) | Shows whether the caps are too large (wasted latency) or too small (frequent full/empty) |
| Peak memory | Checks the stated worst case from §3.1 |

## 12. Summary

1. Buffer only for **variance**. Size the buffer in **time** and cap it by count, bytes and duration.
2. Put deep buffers where items are **cheap** (compressed) and shallow ones where they are expensive (decoded).
3. Lossless edges use **non-blocking backpressure** with wake hooks. Real-time edges **drop, with latest wins**.
4. Let the consumer run on **its own clock** and **select by timestamp**. That resolves an unknown rate mismatch with no rate estimate: skip when the producer is faster, hold when it is slower.
5. Count on-purpose skips separately from failures, and measure everything.
