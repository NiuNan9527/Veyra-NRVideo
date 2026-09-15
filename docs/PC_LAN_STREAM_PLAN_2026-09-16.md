# PC LAN low-latency input plan — 2026-09-16

## Goal

Add an experimental PC-to-PC LAN video input for the dual-PC use case:

```
RX 9070 gaming PC -> low-latency HEVC/AMF -> wired LAN -> Veyra/RTX PC
                   -> D3D12VA decode -> existing EnhanceGraph -> NR/SR/FG -> display
```

Keyboard and mouse remain connected directly to the gaming PC.

## Latency contract

- Decoded ingress mailbox capacity is **1**.
- If enhancement is behind, an unread decoded frame is overwritten by the newest frame.
- An overwrite marks the next delivered packet with `FrameFlagBits::Drop` so temporal history resets instead of silently mixing discontinuous frames.
- The network decoder runs independently from NR/SR/FG, preventing the enhancement loop from growing a decoded-frame queue.
- FFmpeg network demux uses bounded probe/buffer/read-timeout settings.
- With FG disabled, LAN input is not intentionally delayed to follow source PTS.
- No GPU-to-CPU pixel readback is added to the live path.
- D3D12VA is preferred on the receiving RTX PC and shares Veyra's D3D12 device.

## Phase 1 transport

The first hardware-test transport is MPEG-TS over UDP.

Receiver URL:

```
udp://0.0.0.0:5000?fifo_size=262144&overrun_nonfatal=1&buffer_size=262144
```

Sender target:

- HEVC through AMD AMF
- 3840x2160 / 60 fps when the capture path sustains it
- ultra-low-latency usage
- no B-frames
- async depth 1
- initial 120 Mbps
- one-second GOP
- MPEG-TS packets sized for UDP

## Evidence boundary

This branch does not yet prove button-to-photon latency. Software timestamps can measure receive/decode/enhance/present-return segments, but monitor scanout still needs the user's RX 9070 + RTX 5060 Ti machines.

## First user acceptance

1. 4K60 HEVC UDP opens in Veyra.
2. D3D12VA decode reports active.
3. If NR falls below 60 fps, input freshness remains bounded by dropping stale decoded frames rather than accumulating delay.
4. Stop/restart does not leave an unbounded queue.
5. Compare hand-feel and image quality against GC553Pro 1440p60 NV12.
