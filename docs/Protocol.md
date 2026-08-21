# AutoLink Wire Protocol

The source of truth for how two boards find each other's baud rate, lock onto it,
carry data reliably, and recover from a disruption. Read alongside
`src/al/link/LinkWire.h` (the vocabulary) and `src/al/link/sweep/LinkDecision.h`
(the decision functions).

Two peers are wire-compatible when their `AUTOLINK_VERSION` major numbers match.

## States

There are exactly two link states: `SWP` and `OK`.

```
  begin()
     |
     v
  +-------+   3-phase sweep succeeds   +------+
  |  SWP  | -------------------------> |  OK  |
  +-------+                            +------+
     ^                                     |
     +-------------------------------------+
        error threshold / rate window /
        idle or dead-peer watchdog / BREAK
```

`SWP` carries an internal three-phase sweep (`SweepPhase::PHASE1/2/3`). It is not
a link state and is not visible on the API.

## The sweep

**Phase 1 — connect at the slowest baud.** Both ends drop to
`allowedBauds[count-1]`. The master sends `PING`; the slave answers `PONG` on the
first valid one; the master promotes on the first `PONG`. The slowest baud has the
longest bit period and is the most tolerant of cable capacitance, clock mismatch,
and a peer that has not finished booting — so if anything works, this does. If
nothing is heard after ~11 ticks the link logs a wiring hint (TX↔RX crossover,
shared ground).

**Phase 2 — walk top-down for the fastest baud that works.** The master jumps to
`allowedBauds[0]` (fastest) and sends one `PING` per baud, walking down until a
`PONG` arrives inside the dwell. That baud is promoted to Phase 3. Running out of
bauds falls back to locking at the slowest.

**Phase 3 — confirm the baud is reliable, not lucky.** The master pings the chosen
baud until `PHASE3_ACKS_NEEDED` (2) `PONG`s arrive, then sends `LOCK_CMD + index`
and both ends lock. A wire that produces one lucky decode and then fails would
pass any single-ack gate; a 2-ack gate filters it. Failing the gate walks down to
the next baud.

On lock, the baud index is recorded as `preferredBaud_`.

### Dwells

Computed once at `begin()` from the baud list (`LinkSweep::computeDwells`):

| Dwell | Value |
|---|---|
| Phase 1 (master, per PING) | `cfg.delayMs`, jittered ±1/6 on device to break lockstep between two peers |
| Phase 1 (slave) | `phase2[0] + 200 ms` |
| Phase 2 (master, per baud) | `250 ms * allowedBaudsCount * 1.1` — long enough to outlast one full slave walk |
| Phase 2 (slave, per baud) | flat `250 ms` |
| Phase 3 (master) | `max(roundTripMs(baud), 50) * (acksRemaining + 1) + 100`, floored at 200 ms |

The master's Phase 2 dwell exceeds the slave's whole sweep, so the slave never
times out mid-handshake. The dwells are caps, not delays: an early `PONG` promotes
immediately.

## Re-sweep

Any drop calls `reset_unlocked`, which re-enters the sweep. A **BREAK** from the
peer is a clean detach, so the master keeps `preferredBaud_` and attempts a fast
Phase 3 re-lock at the proven baud. If that re-lock misses, Phase 3's timeout
falls back to a full Phase 1 walk rather than advancing to the next baud, so one
bad preferred baud cannot drag the link off the proven one. A **watchdog** drop
clears the preference — a baud that just failed is not a good bet.

The slave always walks Phase 1 from the slowest and meets the master there.

## Wire format

### Control frames (5 bytes)

```
[0xAA] [0x55] [cobsSeq] [cmd] [CRC8 over the first 4 bytes]
```

| Cmd | Value | Meaning |
|---|---|---|
| `PING_CMD` | `0x22` | Sweep probe, and the OK-state keepalive. |
| `PONG_CMD` | `0x33` | Answer to a `PING`. |
| `LOCK_CMD` | `0x44` | `0x44 + baudIndex` — master commits both ends to a baud. |
| `REQ_CMD` | `0x11` | Reserved. |

**Sweep-frame seq byte carries a session epoch.** The 5-byte control
frame's seq byte (`byte[2]`) is unused during the sweep phase (the
receiver's `handleSwp_unlocked` discards it), so the sweep machinery
reuses the slot to carry a one-byte session epoch. Every sweep PING /
PONG / LOCK frame sends `sweepEpoch_` in the seq byte instead of
`txSeq`. The OK-state keepalive (PING) and the OK-state receive path
also carry the epoch, so a still-OK peer that observes a sweep PING
with a different epoch knows the sender restarted under it and can
force a real resync instead of auto-ACKing into a session that no
longer matches. The epoch bumps inside the same `count && state ==
OK` gate `discCount` uses, so it advances in lockstep with
disconnects and a routine / paused kickoff leaves it untouched.

CTRL bytes may contain `0x00`, so they are **not** COBS-framed. In `OK` the
receiver scans for the `0xAA 0x55` sentinel inside the byte stream — which
collides with COBS payload at ~1/65536 — and therefore validates the CRC-8 before
consuming five bytes as CTRL. On a CRC miss only the `0xAA` is dropped and the
rest goes to the COBS framer. A CTRL frame that straddles two UART delivery chunks
is held in `okCarry_` until the next chunk completes or disqualifies it.

### Data frames

```
[0x00] [ COBS( cobsSeq | payload | CRC8 ) ] [0x00]
```

`cobsSeq` is `0..0xFD` (`COBS_SEQ_MAX`); `0xFE` and `0xFF` are reserved as the
ACK/NAK discriminators below. Payload is at most `MAX_CHUNK` (250) bytes.

### ACK / NAK frames

COBS-framed like data, but the first decoded byte is a type discriminator:

```
ACK:  [ 0xFF ] [ ackedCobsSeq ] [ bytesRecvd LE u16 ] [ CRC8 ]   -> 8 wire bytes
NAK:  [ 0xFE ] [ missingCobsSeq ]                     [ CRC8 ]   -> 6 wire bytes
```

`bytesRecvd` is the payload length the receiver actually took, so the sender can
reconcile per-message byte accounting (`bytesRecvdForMessage`).

### BREAK

A UART break condition followed by a hint byte carrying the baud index to restart
at (`0xFF` = no hint). A peer that ignores the hint still resets.

### Messages

`sendMsg` prepends a 6-byte header (`len` u32 LE + `crc16` LE) and chunks the
payload into `MAX_CHUNK` frames. A message whose header + payload fits one chunk
is **merged into a single frame** — the difference between ~20% and ~38% wire
efficiency at 100 bytes. `recvMsg` returns a message only when every chunk has
arrived in order and the CRC-16 verifies.

## ARQ

### ASYNC — Go-Back-N (default)

The sender keeps up to `AUTOLINK_ARQ_PIPELINE_WINDOW` chunks in flight, clamped
at `begin()` to what the *receiver's* stream buffer can actually hold —
`min(AUTOLINK_ARQ_PIPELINE_WINDOW, streamBufferFloor(cfg) / (MAX_CHUNK +
MSG_HDR))`. At the field-tested default (`streamBufferSize` sized for a 2048 B
`maxMsg`) this clamps the compile-time window of 32 down to 16 — a full 32-chunk
window (8000 B) would always overrun a 4108 B receiver buffer regardless of ACK
timing. The clamped value is what actually governs admission; read it back via
`arqWindow()`, not the raw macro. The receiver accepts **strictly in order**:

- **In order** → deliver, and cumulative-ACK the new contiguous high-water mark.
- **Ahead of the expected seq** (a gap) → **drop the frame** and NAK the expected
  seq. Every out-of-order arrival re-NAKs, which is the fast-retransmit signal.
- **Behind** (a stale duplicate, its ACK was lost) → re-ACK so the sender frees
  the slot.
- **The app hasn't drained enough of its buffer to accept the next message** →
  hold at the expected seq and NAK it, same as a gap, but the hold does *not*
  re-NAK on every subsequent retx arrival the way a gap does — it re-NAKs only
  once the app has actually freed more buffer space than it had when the hold
  was first raised. A receiver whose app layer is genuinely stuck (not just
  slow) would otherwise answer a resend storm with a NAK storm of its own.

An ACK is cumulative: it frees every sender slot from `gbnBase` through the acked
seq. Interior slots whose own ACK never arrived — the reason the base was stuck —
are backfilled from the sender's own cache, since a cumulative ACK proves they
landed byte-for-byte.

The only retransmit target is the base (the oldest unacked). A NAK matching it, or
its RTO (`cfg.syncAckTimeoutMs`), triggers a resend of the window **from the base
forward** — the receiver discards out-of-order frames, so the base alone would
never make progress. A NAK for the base is not always acted on immediately: it is
suppressed (counted, but no resend goes out and the sender's RTO clock is left
alone) when a *same-event* NAK arrives inside a short dedup window (at least
`2 * gbnResendFlightMs`, so a peer NAKing faster than one resend can land doesn't
trigger a second one), and when the base has already exhausted its NAK-driven
retry budget without advancing — at that point the peer is holding the base
deliberately (the app-buf-full case above), not dropping frames, and resending
more copies cannot help; the RTO ladder becomes the only recovery path.

Three things keep that resend from becoming a death spiral on a full-duplex UART:

1. **Burst cap** (`cfg.gbnResendBurstMax`, default 8). Replaying the whole window
   every RTO saturates the uplink and starves the peer's reverse ACK path. Later
   RTOs replay the next prefix.
2. **Inter-round backoff** (`decideGbnBackoff`). Rounds that make no forward
   progress double the cadence, capped at `8 * RTO`. The first RTO is never
   delayed, so a healthy recovery pays nothing.
3. **Keep-vs-drop on `maxRetx`** (`decideGbnDropOnMaxRetx`). Exhausting the retry
   budget on the base means one of two things: the peer is gone (nothing inbound
   for a full RTO → honest drop), or the peer is alive and our own resend storm is
   starving its ACK path (→ keep, rearm the base, reset the backoff). A dead peer
   whose floating RX line keeps stamping `lastRxMs` would ride *keep* forever, so
   consecutive keeps are capped (3) before the drop is forced.

**Admission.** The in-flight set is always contiguous, so one bound covers it:
`inflight + this message's chunks <= window`. Otherwise `send()` returns `0`. The
cache pool is `2 * window`, so it always has room when that holds. Applications
that draw random message sizes should clamp against
`maxLenForLiveWindow(window, inflight)`.

### SYNC

One frame in flight. `sendMsg` blocks on the ACK and runs its own retransmit
ladder, resending the **same** seq verbatim up to `cfg.maxRetx`. What happens
when that ladder is exhausted depends on which frame it was:

- **Single-chunk message** (fits in one frame) or a **multi-chunk message's
  body frames** (anything after the header): the message is abandoned —
  `sendMsg` returns failure with `SendMsgReason::SyncMidMessageTimeout` — but
  the link is **not** reset and no BREAK is sent. The receiver has no
  message-assembly state to strand here beyond its own per-frame RTO, which
  times out on its own and resyncs the framer. A hardware backpressure blip on
  one message no longer tears the whole link down.
- **Multi-chunk message's header frame**: an exhausted ladder here still drops
  the link and sends a BREAK, the original behavior — the header carries the
  message length, and losing it changes what the receiver is waiting for, not
  just how much of it arrived.

SYNC never populates the ARQ cache.

## Health watchdogs (`LinkHealth.h`)

Evaluated on every `OK` tick, in order. All are disabled when
`cfg.idleTimeoutMs == 0`.

| Action | Condition |
|---|---|
| `DropTxStall` | TX has been rejected continuously for longer than the idle window. |
| `DropDeadLink` | Pending traffic, but silence both ways past the idle window. The SYNC backstop. |
| `DropSilentPeer` | The link was alive but has heard nothing for `3 * idleTimeoutMs`. Catches a locked link with an empty pipeline and no frame errors. |
| `DropAsymIdle` | ASYNC only: we are transmitting (`txAge < 1 s`) but have heard nothing for `> 300 ms` **and** past `2 * RTO`. The RTO gate prevents a false drop while the window legitimately parks on an RTO sweep. |
| `DropIdle` | Silence both ways past the idle window, with pending traffic or frame errors. |
| `DropPoolExhaust` | Cache full, pending traffic, and RX silent past the repair horizon. A full pool with live RX is routine backpressure, not a fault. |

## OK-state keepalive

A locked link that is otherwise idle (no pending chunks) emits a `PING` once
`idleTimeoutMs / 2` has passed since its last TX. The peer's `OK` receive path
answers with a `PONG`, so one frame refreshes `lastRxMs` at **both** ends and the
responder's own `lastTxMs` suppresses its own keepalive.

It is a `PING`, not an ACK: an ACK carries a `cobsSeq` and would walk the peer's
`gbnBase` — a keepalive must not touch ARQ state. Without it, `DropSilentPeer`
would tear down a perfectly healthy duty-cycled link (a sensor reporting every
60 s, a command link, a paused dashboard). Pinned by `OkKeepaliveTest`.

## Logging

Every phase transition logs a banner. Grep for `P1`, `P2`, `P3`:

```
[I AutoLink] === P1 slowest baud[5]=9600 ===
[I AutoLink] === P2 top-down sweep ===
[I AutoLink] === P3 2-of-3 baud[0]=512000 ===
[I AutoLink] OK at baud[0]=512000
```
