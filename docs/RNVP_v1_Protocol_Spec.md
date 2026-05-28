# RNVP v1 Protocol Specification

RNVP, Realtime Network Video Protocol, is the UDP-based video transport used by this engine. RNVP v1 is designed for low-latency realtime video experiments where stale video should be dropped, missing chunks should be recovered only while useful, and the encoder should adapt to user-visible quality of experience.

This document describes the protocol as implemented in `network/PacketProtocol.h`, `network/NetworkManager.cpp`, `network/UdpReceiver.cpp`, `network/FrameReassembler.cpp`, `network/JitterBuffer.cpp`, and `network/AdaptiveStreamingController.cpp`.

## 1. Goals

- Transport realtime camera frames over UDP with explicit frame and chunk metadata.
- Support chunk-level selective retransmission without turning the stream into a high-latency reliable byte stream.
- Measure RTT with Ping/Pong packets.
- Report received and missing chunks with ACK payloads.
- Use deadline-based NACK behavior when a frame is incomplete before its receive deadline.
- Drop frames that can no longer meet the display deadline.
- Pace data packets at the adaptive target bitrate to reduce burst-induced queueing, jitter, and packet loss.
- Feed packet loss, jitter, RTT, decode load, display load, and deadline misses into adaptive streaming control.

## 2. Transport Model

RNVP v1 runs over UDP. Each RNVP packet is self-describing and starts with a fixed 44-byte big-endian header.

The current local experiment topology uses:

| Role | Default |
| --- | --- |
| Sender remote address | `127.0.0.1:50000` |
| Receiver listen port | `50000` |
| Transport | UDP |
| Maximum RNVP UDP payload target | `1200` bytes for the legacy fragmentation path |
| Default stream id | `1` |

RNVP is frame-oriented. A video frame is fragmented into chunks. The receiver reconstructs frames by `(streamId, frameId)` and releases complete frames through the jitter buffer.

## 3. Byte Order

All multi-byte integer fields are encoded in network byte order, big endian.

The helper functions are:

- `WriteU16BE`, `WriteU32BE`, `WriteU64BE`
- `ReadU16BE`, `ReadU32BE`, `ReadU64BE`

## 4. RNVP Header Layout

RNVP v1 header size is fixed at 44 bytes.

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `magic` | `u32` | Protocol magic. Must be `0x524E5650`, ASCII `RNVP`. |
| 4 | `version` | `u8` | RNVP version. Must be `1`. |
| 5 | `packetType` | `u8` | Packet type. See Packet Types. |
| 6 | `headerSize` | `u16` | Header byte size. Must be `44` for v1. |
| 8 | `sequence` | `u32` | RNVP packet sequence number. |
| 12 | `streamId` | `u32` | Logical stream id. Default stream is `1`. |
| 16 | `frameId` | `u32` | Video frame id for data or frame-related control. |
| 20 | `chunkIndex` | `u16` | Zero-based chunk index for data packets. |
| 22 | `chunkCount` | `u16` | Total chunk count for the frame. |
| 24 | `sendTimeUs` | `u64` | Sender timestamp in microseconds. |
| 32 | `payloadSize` | `u32` | Payload byte size after the RNVP header. |
| 36 | `flags` | `u32` | Packet flags. See Packet Flags. |
| 40 | `codecType` | `u8` | Codec identifier. See Codec Types. |
| 41 | `reserved0` | `u8` | Reserved. Must be ignored by receivers. |
| 42 | `reserved1` | `u16` | Reserved. Must be ignored by receivers. |

Validation rules:

- `magic` must match `RNVP`.
- `version` must match `1`.
- `headerSize` must match `44`.
- `headerSize + payloadSize` must not exceed the received UDP datagram size.
- For `Data` packets, `chunkCount` must be greater than `0`.
- For `Data` packets, `chunkIndex` must be less than `chunkCount`.

## 5. Packet Types

| Value | Name | Direction | Purpose |
| ---: | --- | --- | --- |
| 0 | `Data` | Sender to receiver | Carries one frame chunk. |
| 1 | `Ack` | Receiver to sender | Reports received and missing chunks. Also acts as deadline NACK when missing chunks are present. |
| 2 | `Ping` | Either side, currently sender initiated | RTT measurement request. |
| 3 | `Pong` | Ping receiver to ping sender | RTT measurement response. |
| 4 | `Control` | Either side | Quality, FPS, bitrate, keyframe, or stats command. |
| 5 | `TransportFeedback` | Receiver to sender | Batched packet arrival/loss feedback for congestion and bandwidth estimation. |

## 6. Packet Flags

| Flag | Value | Meaning |
| --- | ---: | --- |
| `PacketFlag_KeyFrame` | `1 << 0` | Data packet belongs to a keyframe. |
| `PacketFlag_LastChunk` | `1 << 1` | Data packet is the last chunk of the frame. |
| `PacketFlag_DroppedAllowed` | `1 << 2` | Frame may be dropped if late or incomplete. |
| `PacketFlag_Control` | `1 << 3` | Packet is a control-oriented packet. |

## 7. Codec Types

| Value | Name | Meaning |
| ---: | --- | --- |
| 0 | `Unknown` | Codec unspecified. |
| 1 | `Raw` | Raw frame payload. |
| 2 | `MJPEG` | JPEG-compressed frame payload. |
| 3 | `H264` | H.264 payload. Present in the protocol; MJPEG is the current main realtime path. |

## 8. Data Packet

A `Data` packet carries one chunk of one encoded video frame.

Required data header fields:

- `packetType = Data`
- `streamId`
- `frameId`
- `chunkIndex`
- `chunkCount`
- `sendTimeUs`
- `payloadSize`
- `flags`
- `codecType`

Receiver behavior:

1. Decode and validate the RNVP header.
2. Route `Data` packets to `FrameReassembler`.
3. Track chunks by `(streamId, frameId)`.
4. Build the completed frame only after all chunks are present.
5. Push complete frames to `JitterBuffer`.
6. Drop late frames if they exceed the display deadline.

## 9. Raw Frame Payload Header

Raw frame payloads may start with a 16-byte raw-frame header.

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `magic` | `u32` | Must be `RVF1`. |
| 4 | `width` | `u16` | Frame width. |
| 6 | `height` | `u16` | Frame height. |
| 8 | `format` | `u8` | `1` means RGBA8. |
| 9 | `reserved0` | `u8` | Reserved. |
| 10 | `reserved1` | `u16` | Reserved. |
| 12 | `payloadBytes` | `u32` | Raw pixel payload size after this header. |

## 10. ACK and Deadline NACK Payload

RNVP v1 uses a single `Ack` packet type for both positive ACK and NACK-style missing chunk feedback.

Base payload size is 16 bytes.

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `frameId` | `u32` | Frame being acknowledged. |
| 4 | `receivedChunkCount` | `u32` | Number of chunks received for this frame. |
| 8 | `missingChunkCount` | `u32` | Number of missing chunks for this frame. |
| 12 | `latestSequence` | `u32` | Latest RNVP sequence observed by the receiver for the frame. |
| 16 | `missingChunkIndices[]` | `u16[]` | Missing chunk indices, up to `512` entries. |

ACK interpretation:

- `missingChunkCount == 0`: frame is complete or no missing chunks are known.
- `missingChunkCount > 0`: receiver is explicitly requesting missing chunks. This is the RNVP deadline NACK path.

The receiver currently sends ACKs in two cases:

1. Last-chunk ACK path: when a `Data` packet with `PacketFlag_LastChunk` is received and frame state is known.
2. Deadline NACK path: when an incomplete frame exceeds the NACK deadline and missing chunk indices can be computed.

This design fixes the weak point of last-chunk-only ACK because a lost last chunk can still be detected by the receiver deadline scanner.

## 11. Transport Feedback Payload

`TransportFeedback` is a packet-level observation channel. It does not replace ACK/NACK recovery. It gives the sender enough information to estimate packet loss, arrival jitter, and queue delay trend before frame loss becomes visible.

Base payload size is 16 bytes.

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `baseSequence` | `u32` | First RNVP data sequence represented by this feedback packet. |
| 4 | `packetStatusCount` | `u16` | Number of status entries. |
| 6 | `feedbackSequence` | `u16` | Receiver-generated feedback sequence. |
| 8 | `referenceReceiveTimeUs` | `u64` | Receiver receive timestamp used as the delta base. |
| 16 | `entries[]` | `8 bytes each` | Packet status entries. |

Each entry is 8 bytes.

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `sequenceDelta` | `u16` | `packetSequence - baseSequence`. |
| 2 | `flags` | `u8` | `Received` or `Missing`. |
| 3 | `reserved0` | `u8` | Reserved. |
| 4 | `receiveDeltaUs` | `u32` | `receiveTimeUs - referenceReceiveTimeUs` for received packets. Missing packets use `0`. |

Current batching policy:

- Send after `32` packet statuses, or
- Send after `50 ms`, whichever comes first.
- Include both received packet statuses and missing sequence gaps.
- Cap one feedback payload to `128` entries.

Sender-side interpretation:

- `feedbackLossRate` = missing statuses / all statuses.
- `feedbackArrivalJitterMs` = average absolute difference between send interval and receive interval.
- `feedbackQueueDelayTrendMs` = average positive interval expansion, which suggests queue growth.
- `BandwidthEstimator` consumes the same packet statuses and exposes `estimatedBandwidthBps`, `deliveryRateBps`, `bandwidthQueueDelayMs`, `bandwidthRttTrendMs`, `bandwidthLossTrend`, and `bandwidthJitterTrendMs`.

The sender and receiver clocks are not assumed to be synchronized. The first implementation therefore uses interval differences, not absolute one-way delay, as the congestion signal.

## 12. Ping/Pong RTT Measurement

### Ping Payload

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `clientTimeUs` | `u64` | Sender timestamp in microseconds. |

### Pong Payload

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `clientTimeUs` | `u64` | Original Ping timestamp copied from the Ping. |
| 8 | `serverTimeUs` | `u64` | Receiver timestamp when Pong was created. |

RTT calculation:

```text
rttMs = (nowUs - pong.clientTimeUs) / 1000.0
```

The sender tracks:

- last RTT
- average RTT
- max RTT
- RTT sample count

## 13. Control Payload

Control payload size is 8 bytes.

| Offset | Field | Type | Description |
| ---: | --- | --- | --- |
| 0 | `command` | `u8` | Control command id. |
| 1 | `reserved0` | `u8` | Reserved. |
| 2 | `reserved1` | `u16` | Reserved. |
| 4 | `value` | `u32` | Command value. |

### Control Commands

| Value | Name | Value Meaning |
| ---: | --- | --- |
| 0 | `None` | No operation. |
| 1 | `SetJpegQuality` | Target JPEG quality. |
| 2 | `SetTargetFps` | Target frame rate. |
| 3 | `SetBitrateKbps` | Target bitrate in kbps. |
| 10 | `RequestKeyFrame` | Request next frame to be sent as a keyframe. `value` may carry the related frame id. |
| 11 | `ResetStats` | Reset receiver-side statistics. |

`RequestKeyFrame` is used when selective retransmission is not expected to recover useful video in time, or repeated incomplete frames indicate the stream needs a clean reference point.

## 14. Selective Retransmission Policy

RNVP v1 is not a reliable byte stream. It is a low-latency video transport with bounded recovery.

Sender-side retransmission state:

| Parameter | Current Value |
| --- | ---: |
| Sent-frame history limit | `24` frames |
| Max retransmits per frame | `1` |
| Max retransmit frame lag | `2` frames |
| Max retransmit age | `180 ms` |

When an ACK has `missingChunkCount > 0`:

1. Sender searches the sent-frame history by `(streamId, frameId)`.
2. If the frame is not found, the request is stale and a keyframe is requested.
3. If the frame is too old by frame lag or age, the request is stale and a keyframe is requested.
4. If the retransmit budget is exhausted, the request is stale and a keyframe is requested.
5. Otherwise, only `missingChunkIndices` are retransmitted.
6. If no missing index list is available, the sender falls back to full-frame retransmission.
7. If the missing rate is at least `25%`, the sender also schedules a keyframe.

This keeps recovery selective and bounded. RNVP attempts to recover frames that can still arrive in time, but avoids wasting bandwidth on stale video.

## 15. Deadline NACK Policy

Receiver-side incomplete frame recovery uses these current constants:

| Parameter | Current Value | Purpose |
| --- | ---: | --- |
| Frame NACK deadline | `80 ms` | Wait this long after first chunk before sending deadline NACK. |
| NACK interval | `40 ms` | Minimum interval between NACKs for the same frame. |
| NACK recovery expiry | `140 ms` | Expire incomplete recovery before the display deadline. |
| Minimum recovery slack | `12 ms` | Do not NACK if recovery cannot plausibly arrive before expiry. |
| Max deadline NACKs per frame | `2` | Bound per-frame feedback. |
| Keyframe request cooldown | `500 ms` | Avoid excessive keyframe requests. |

Receiver behavior:

1. Incomplete frames are tracked by `FrameReassembler`.
2. Once an incomplete frame exceeds `80 ms`, missing chunks are calculated.
3. Receiver sends an ACK payload with `missingChunkCount > 0` and `missingChunkIndices`.
4. If the frame remains incomplete beyond recovery expiry, it is dropped.
5. If repeated incomplete or expired frames occur, receiver sends `RequestKeyFrame`.

Keyframe request triggers include:

- missing chunks on last-chunk ACK with at least `2` missing chunks
- at least `2` consecutive incomplete frames on last-chunk ACK path
- expired-after-NACK recovery
- at least `2` expired frames
- at least `3` consecutive incomplete frames

## 16. Display Deadline Policy

RNVP is optimized for interactive video. The display deadline is currently:

```text
maxDisplayLatency = 150 ms
```

Deadline behavior:

- Frames older than `150 ms` from `sendTimeUs` are dropped.
- If `sendTimeUs` is unavailable or invalid, receiver falls back to `receiveTimeUs`.
- Jitter-buffered frames are checked before release.
- Released frames are checked again before entering the output queue.
- The output queue prefers the latest frame and drops older queued frames when renderer lag or jitter burst release creates backlog.

The policy is intentional: for realtime video, a fresh frame is usually more valuable than a late complete frame.

## 17. Jitter Buffer Policy

The jitter buffer stores completed frames in `(streamId, frameId)` order.

Default parameters:

| Parameter | Current Value |
| --- | ---: |
| Default target delay | `30 ms` |
| Default max buffered frames | `8` |
| Runtime output queue limit | `4` frames |
| Hard target-delay clamp | `200 ms` |

Rules:

- Duplicate frames are dropped.
- Frames older than the last released frame for the same stream are dropped.
- Overflow drops the oldest frame.
- A frame becomes ready after `receiveTimeUs + targetDelayMs`.
- Expired frames are dropped using the `150 ms` display deadline.

Auto jitter-buffer mode:

| Parameter | Current Value |
| --- | ---: |
| Update interval | `500 ms` |
| Min auto delay | `5 ms` |
| Max auto delay | `80 ms` |
| Max step up | `5 ms` per update |
| Max step down | `3 ms` per update |

Auto delay formula:

```text
calculatedDelayMs = 5 + averageJitterMs * 3

if currentJitterMs > averageJitterMs * 2:
    calculatedDelayMs += 5

if maxJitterMs >= 25:
    calculatedDelayMs = max(calculatedDelayMs, averageJitterMs * 2 + 15)
```

The result is clamped and smoothed to avoid sudden playback rhythm changes.

## 18. Adaptive Streaming Policy

Adaptive streaming consumes network and pipeline telemetry:

- ACK missing rate
- packet loss rate
- RTT
- current latency
- jitter
- receive FPS
- decode FPS
- display FPS
- displayed frame count
- deadline drops
- output queue drops
- deadline NACK count
- deadline NACK missing chunk count
- output queue drop reason

Controller modes:

| Mode | Purpose |
| --- | --- |
| `Fixed Quality` | Stable baseline. Uses quality `85`, FPS `30`, bitrate `9500 kbps`, resolution `320x180`. |
| `Loss Reactive` | Reacts mainly to packet loss and NACK pressure. |
| `QoE/Deadline Adaptive` | Uses QoE, deadline, display FPS, decode/display load, RTT, jitter, and loss causes. |

Adaptive target bounds:

| Target | Min | Max |
| --- | ---: | ---: |
| JPEG quality | `35` | `95` |
| FPS | `8` | `30` |
| Bitrate | `500 kbps` | `12000 kbps` |
| Width | `160` | `320` |
| Height | `90` | `180` |

Cause-specific degradation:

| Cause | Detection Examples | Response |
| --- | --- | --- |
| PacketLoss | ACK missing rate, packet loss rate, deadline NACKs | Reduce bitrate/quality. |
| Jitter | high jitter, jitter burst output drops | Mild bitrate reduction, preserve latency. |
| RTT | RTT or latency close to deadline | Stronger bitrate reduction and FPS reduction. |
| Bandwidth | target bitrate exceeds estimated bandwidth ceiling and feedback trends show pressure | Cap recovery and reduce target bitrate toward the estimated ceiling. |
| DecodeLoad | decode FPS below receive FPS | Reduce bitrate and FPS. |
| DisplayLoad | display FPS below decode FPS or renderer-lag output drops | Reduce bitrate and FPS. |

Bitrate is mapped to resolution, FPS, and JPEG quality. Lower bitrate tiers reduce resolution and FPS first, then quality.

## 19. RNVP Congestion Control Policy

RNVP separates media transport, control, feedback, and congestion control.

| Layer | Packet / Component | Role |
| --- | --- | --- |
| Data | RNVP `Data` | Carries frame chunks. |
| Control | RNVP `Control`, Ping/Pong | Carries keyframe requests, bitrate/FPS/quality commands, and RTT measurement. |
| Feedback | RNVP `Ack`, `TransportFeedback` | Reports frame chunk recovery state and packet-level delivery timing/loss. |
| Congestion Control | `AdaptiveStreamingController` + `BandwidthEstimator` | Chooses target bitrate/FPS/resolution using AIMD and bandwidth ceiling logic. |

Congestion control modes:

| Mode | Main Signal | Behavior |
| --- | --- | --- |
| `Loss Based` | ACK missing rate, receiver packet loss, feedback loss trend, deadline NACKs | Reduces bitrate when loss rises. |
| `Delay Based` | queue delay trend, RTT trend, jitter trend, latency | Reduces bitrate before heavy loss when queue growth appears. |
| `Hybrid` | loss, delay, jitter, QoE, decode load, display load, deadline drops | Combines transport and user-visible pipeline signals. |

AIMD policy:

| Network State | Action |
| --- | --- |
| Stable | Additive increase: `targetBitrateKbps += 100`. |
| Loss > 5% | Multiplicative decrease: `targetBitrateKbps *= 0.85`. |
| Loss > 10% | Multiplicative decrease: `targetBitrateKbps *= 0.70`. |
| Loss > 20% | Multiplicative decrease: `targetBitrateKbps *= 0.50`. |
| Queue delay > 30 ms | Multiplicative decrease: `targetBitrateKbps *= 0.85`. |
| Queue delay > 80 ms | Multiplicative decrease: `targetBitrateKbps *= 0.70`. |

The active bitrate is still clamped by the BandwidthEstimator ceiling. This prevents additive recovery from immediately overshooting the estimated path capacity.

`Fixed Quality` mode remains a fixed A/B baseline. `Loss Reactive` maps to `Loss Based` congestion control. `QoE/Deadline Adaptive` can use `Loss Based`, `Delay Based`, or `Hybrid`; the default is `Hybrid`.

## 20. Packet Pacing Policy

RNVP data packets are paced before they enter the network condition simulator or UDP `sendto` path. The pacer smooths a frame's chunk burst into packet-spaced transmission based on the current adaptive target bitrate.

Pacing target:

```text
packetIntervalSec = packetBytes * 8 / targetBitrateBps
```

Example:

```text
targetBitrate = 4 Mbps
packetSize = 1200 bytes
packetBits = 9600 bits
packetInterval = 9600 / 4,000,000 = 2.4 ms
```

Packet priority:

| Packet Category | Pacing Behavior | Reason |
| --- | --- | --- |
| RNVP `Data` | Normal paced queue | Smooth frame chunk bursts. |
| Selective retransmit chunks | High-priority paced queue | Recovery should not sit behind new video for too long. |
| Ping/Pong | Immediate path | RTT feedback should stay fresh. |
| ACK / Deadline NACK | Immediate receiver path | Missing chunk feedback is latency-sensitive. |
| TransportFeedback | Immediate receiver path | Congestion observation should not be delayed by media backlog. |
| Control / RequestKeyFrame | Immediate receiver path | Control feedback should not be delayed by media backlog. |

Current pacer behavior:

- Default target bitrate is `6 Mbps`.
- Adaptive controller updates the target bitrate in kbps.
- Queue capacity is `512` packets.
- If the queue overflows, older normal-priority packets are dropped first.
- Each queued media packet carries a send deadline.
- Packets that miss their send deadline while waiting in the pacer are dropped rather than sent late.

Pacing telemetry:

- enabled state
- target bitrate
- total queued packets
- high-priority and normal queue sizes
- enqueued packets
- sent packets and sent bytes
- dropped packets
- deadline drops
- overflow drops
- current and max queue delay

The pacing design complements adaptive streaming: the controller chooses an appropriate bitrate, and the pacer makes packet emission match that bitrate smoothly.

## 21. Bandwidth Estimation Policy

The sender maintains a `BandwidthEstimator` fed by packet-level TransportFeedback and Ping/Pong RTT samples.

Estimator inputs:

- received and missing packet statuses
- RNVP sequence number
- sender-side packet send timestamp
- receiver-side packet receive timestamp delta
- packet byte size
- RTT trend from Pong samples

Estimator outputs:

| Field | Meaning |
| --- | --- |
| `estimatedBandwidthBps` | Conservative current bandwidth estimate used for evaluation and future congestion control. |
| `deliveryRateBps` | Smoothed delivered packet byte rate measured from received packet timing. |
| `bandwidthQueueDelayMs` | Smoothed positive receive-interval expansion, used as a queue growth signal. |
| `bandwidthRttTrendMs` | Smoothed RTT delta; positive values indicate RTT growth. |
| `bandwidthLossTrend` | Smoothed packet loss ratio from feedback statuses. |
| `bandwidthJitterTrendMs` | Smoothed packet arrival jitter from send/receive interval differences. |

Current behavior:

- Severe congestion, such as high loss, queue growth, RTT growth, or high jitter, multiplicatively reduces the estimate.
- Moderate congestion reduces the estimate more gently.
- Stable feedback allows gradual probing upward.
- Adaptive streaming uses the estimate as a bitrate ceiling for non-fixed controller modes.
- Recovery is allowed only when the estimated ceiling has headroom above the current target and TransportFeedback trends are stable.
- When the target bitrate is above the estimated ceiling and delivery rate or queue/loss/jitter trends show pressure, the controller marks the cause as `Bandwidth` and reduces the target bitrate.

## 22. Telemetry and Evaluation

RNVP exports telemetry to CSV and Markdown reports. Important fields include:

- RTT: last, average, max
- packet loss and jitter
- received, completed, displayed, dropped frames
- deadline drops
- output queue drop reason
- ACK count and missing chunk counts
- retransmitted frames and chunks
- stale retransmit drops
- keyframe requests
- adaptive control mode
- congestion control mode
- degradation cause
- QoE score
- target JPEG quality, FPS, bitrate, resolution
- pacing queue size, target bitrate, queue delay, sent packets, and pacing drops
- transport feedback packet count, packet statuses, feedback loss, arrival jitter, and queue delay trend
- bandwidth estimate, delivery rate, queue delay trend, RTT trend, loss trend, and jitter trend

The experiment reporter can compare Fixed Quality, Loss Reactive, and QoE/Deadline Adaptive under the same network scenario.

## 23. Current Limitations

- RNVP v1 has no encryption or authentication.
- There is no congestion-control interoperability with TCP-friendly algorithms.
- ACK/NACK packets are not themselves retransmitted.
- Selective retransmission is intentionally limited to one retransmit per frame.
- `H264` is represented in the protocol but MJPEG is the main active realtime path.
- Packet sequence tracking is used for diagnostics; it is not a full reorder/recovery protocol.
- Packet pacing is local sender-side smoothing; it is not yet a full congestion-control algorithm.
- BandwidthEstimator caps adaptive recovery, but it is not yet a full congestion-control algorithm with probing state, fairness, or congestion window modeling.
- The current implementation targets local and controlled-network experiments, not internet-scale NAT traversal.

## 24. Future Extensions

- Add protocol capability negotiation.
- Add sender and receiver session ids.
- Add explicit NACK packet type while keeping ACK payload compatibility.
- Add FEC for small burst losses.
- Add congestion window or pacing model.
- Add explicit probe-up/probe-down states for BandwidthEstimator-driven congestion control.
- Add authenticated control packets.
- Add codec-specific metadata extension headers.
- Add multi-stream synchronization for audio/video.

## 25. Interview Summary

RNVP v1 is a UDP-based realtime video protocol with explicit frame/chunk headers, deadline-based NACK, bounded selective retransmission, packet-level TransportFeedback, packet pacing, jitter buffering, display-deadline dropping, RTT measurement, control commands, and QoE-driven adaptive streaming.

The core design choice is that RNVP does not try to recover every byte or burst every chunk immediately. It paces useful media packets, recovers only missing chunks that can still contribute to a useful frame, and drops stale video to protect interactive latency.
