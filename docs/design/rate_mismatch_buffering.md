# Producer/Consumer Rate Mismatch: Buffering, Bursts and Stalls

How to move frames between two stages whose rates differ by an unknown amount, when the producer can send bursts and the consumer can slow down without warning. The first sections describe the general design. Section 8 shows where each part lives in this framework, and section 9 lists what isn't built yet.

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

The master clock is the **audio** that has actually been heard, not wall time. Video selection follows audio, so a stall in audio output pauses video selection too and A/V sync is kept.

## 9. Not built yet

- **Adaptive target depth (§6).** Queue caps are fixed today. A jitter-based target would lower the steady-state latency of live preview.
- **Sustained-mismatch detection.** There is no `λ̂`/`μ̂` tracking yet, and no signal back to the decoder to skip non-reference frames when it can't keep up.
- **Watermark hysteresis.** `onSpace` fires on every pop. Batching the wakeups would reduce scheduler churn under heavy load.

## 10. Metrics

| Metric | Meaning |
| --- | --- |
| Dropped-frame rate | late drops / (presented + late drops). Excludes rate-cap skips, seek flushes and hidden-window drops |
| Rate-capped count | Frames skipped on purpose because the content rate is above the output rate |
| Repeats / underflows | Ticks where the consumer had no new frame (producer slow or stalled) |
| Queue depth histogram (time) | Shows whether the caps are too large (wasted latency) or too small (frequent full/empty) |
| Peak memory | Checks the stated worst case from §3.1 |

## 11. Summary

1. Buffer only for **variance**. Size the buffer in **time** and cap it by count, bytes and duration.
2. Put deep buffers where items are **cheap** (compressed) and shallow ones where they are expensive (decoded).
3. Lossless edges use **non-blocking backpressure** with wake hooks. Real-time edges **drop, with latest wins**.
4. Let the consumer run on **its own clock** and **select by timestamp**. That resolves an unknown rate mismatch with no rate estimate: skip when the producer is faster, hold when it is slower.
5. Count on-purpose skips separately from failures, and measure everything.
