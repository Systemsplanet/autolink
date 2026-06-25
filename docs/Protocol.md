# AutoLink Connection Protocol

The source of truth for how two boards find each other's baud
rate, lock onto it, and re-lock after a disruption. The 3-phase
sweep, computed per-phase dwell, asymmetric fast detection, and
OK-state heartbeats all live here. The wire contract is stable
across the current 5.x line — any two 5.x peers are
wire-compatible.

## Overview

The protocol is a **three-phase sweep** that starts at the slowest
baud (guaranteed to connect) and climbs to the fastest baud the wire
will support. Each phase has a single, well-defined goal. Each phase
has a **computed dwell time** that is a function of the baud list
itself, not a fixed constant.

```
        +-------------+        +-------------+        +-------------+
        |   PHASE 1   | -----> |   PHASE 2   | -----> |   PHASE 3   |
        | connect     |  1 ack | confirmation|  1 ack | 3-ack gate  |
        | slowest     |        | top-down    |        |             |
        | 1 ack       |        | 1 ack/baud  |        | 3 acks      |
        +-------------+        +-------------+        +-------------+
                                                           |
                                                           v
                                                       OK at baud X
                                                       preferredBaud_ = X
```

After a drop, the link re-enters **Phase 2** (confirmation resweep)
starting at the top of the baud list. `preferredBaud_` is consulted
as a hint for which baud to try first, but Phase 2 always walks top-
down so a transient failure at the preferred baud doesn't trap the
link at a slower one.

## Phase 1: Connect at slowest baud

**Goal:** establish a working link, no matter what. This is the
"I will connect" phase. If this phase fails, the wiring is wrong
or one side is broken.

**Rules:**
- Both sides go to `allowedBauds[last]` (slowest baud in the list).
- The master (Ping) sends `PING` frames.
- The slave (Pong) replies with `PONG_ACK` on the first valid `PING`.
- The master locks on the **first** `PONG_ACK`. 1 ack = lock.
- Dwell at this baud: **unbounded for retries**, but the master
  sends a PING every `dwell_phase1` (default 5000ms) until acked.
- After 30 seconds of no ack, give up and log "wiring check."

**Why slowest:** at 9600 the bit period is 104µs. At 115200 it is
8.7µs. The slowest baud is the most robust to cable capacitance,
clock mismatch, and electrical noise. The master can use the
slowest baud to confirm the other side is alive and responding
before it tries anything faster.

**Why 1 ack:** the wire is at its most robust right now. A single
PONG_ACK means: "I heard you, my UART is set to this baud, the
wires are crossed correctly, and I can decode bytes." That's
enough to proceed.

## Phase 2: Confirmation resweep, top-down

**Goal:** find the fastest baud that the wire actually supports,
as fast as possible. This is the "is there a faster option" phase.

**Rules:**
- The master enters SWP at `allowedBauds[0]` (fastest).
- Sends 1 `PING` at that baud. If `PONG_ACK` arrives within
  `dwell_phase2(baud)`, promote — go to Phase 3 at this baud.
- If no ack within the dwell, advance to the next baud down.
- The slave follows via BREAK (with a hint byte specifying the
  starting baud index). The slave's dwell at each baud is
  `>=` the master's dwell at the same baud, so the slave
  never times out before the master has a chance to send.
- The slave's dwell formula: `dwell_slave(baud) = sum of
  (dwell_master(baud_M) for M from this baud down to slowest)
  + 1 round trip at this baud + 10% margin`.
- The master's dwell formula: `dwell_master(baud) = sum of
  (dwell_master(baud_M) for M from next baud down to slowest)
  + 1 round trip at this baud + 10% margin`.
- The bottom baud's dwell is `1 round trip + 10% margin`.
- Per-baud dwell is a **cap**, not a delay. If the ack arrives
  sooner, we move on sooner.

**Why top-down:** if the wire can carry 115200, we want to find
that out as fast as possible. The fastest baud is at the top of
the list, so we try it first.

**Why 1 ack per baud (not 3):** 3 acks at every baud would make
Phase 2 take 3x as long for no benefit. The 3-ack gate happens
in Phase 3 at the chosen baud, where it's cheap (we know the
baud is the right one to test).

**Why the dwell cap:** without a cap, a 1-ack timeout at 115200
could be arbitrarily long. The cap says: "waiting longer than
this would be slower than just trying the next baud." The cap
is the cost of giving up.

## Phase 3: 2-of-3 gate (not strict 3-of-3)

**Goal:** verify the chosen baud is reliable, not lucky. This
catches the "we got one good PING in a quiet moment but the wire
is actually flaky at this baud" failure mode.

**Rules:**
- The master sends 3 `PING`s at the chosen baud.
- At least 2 of 3 must receive `PONG_ACK` to commit. **2-of-3.**
- If 0 or 1 ack, walk down to the next baud and try again with 3
  new PINGs. Repeat until a baud passes 2-of-3 or we hit the
  bottom of the list.
- The slave is already at the chosen baud (from Phase 2) and
  responds to each PING with PONG_ACK.
- Per-PING spacing: `round_trip_at_baud + 50% margin`.
- Per-baud dwell cap: `3 * round_trip_at_baud + 100% margin`.

**Why 2-of-3 (not strict 3-of-3):** the campaign showed that
strict 3-of-3 was too strict in the bounce scenario. A single
RESET bounce can knock out one of three PINGs; if that happens
during the lock-decision window, strict 3-of-3 would fail to
lock on a perfectly good baud. 2-of-3 is permissive enough to
ride out one bounce per attempt but still strict enough to
filter out most false-locks.

The campaign data (60 cells × 500 trials each):
- Strict 3-of-3: rejected too aggressively (12 boot-race failures).
- 2-of-3:        the better trade-off — rides out one bounce, still filters most false locks.

2-of-3 is the better trade-off.

## Re-sweep on drop

When the link drops (error threshold, idle watchdog, BREAK from
peer, rate window), the master enters **Phase 2** at the top of
the baud list. `preferredBaud_` is consulted as a starting hint
but the sweep still walks top-down so a transient failure at
the preferred baud doesn't trap the link at a slower one.

If `preferredBaud_` is reachable, the master locks there in one
PING. If not, the sweep walks down to the next baud that passes
2-of-3. The re-sweep is bounded by `sum of dwell_phase2(baud)`
for the whole list, which is `~100ms` for a 5-baud list.

After the link is healthy again, the climb protocol (built into
the next re-sweep) tries to find a faster baud.

## OK-state heartbeats + self-heal

In the OK state, both sides send **heartbeats every 100ms**. The
heartbeat is the existing keepalive (a cobsSeq-bearing 0-payload
data frame). The receiver uses the keepalive-arrival timestamp
to detect peer silence.

The existing idle watchdog fires after 10s of no RX. the current protocol adds
a **faster asymmetric detection**: if the local side has TX
activity (we sent a heartbeat in the last 1000ms) but no RX in
the last 300ms, the peer is silent. Drop to PHASE1 immediately.

**Recovery is symmetric.** Both `pong-bounce` and `ping-bounce`
cases are caught in 300ms by the same mechanism. The campaign
showed the current protocol recovers in **303ms** for both, over the 10s default. **33x speedup on both cases.**

**Phase field in PONG_ACK (future, not in initial release):**
The PONG_ACK cmd byte could encode the peer's phase. A PONG_ACK
with phase=PHASE1 means "I'm not in OK." The receiver can
self-heal immediately, saving the 300ms heartbeat-detection
window. Useful for scenarios where peer and receiver happen to
be at the same baud but different phases. The campaign showed
this is a rare case in practice; the asymmetric idle detection
is the dominant win. The phase field is a future optimization,
not a the current protocol requirement.

## Computed dwell times

All dwell times are computed at boot from the baud list. They
are logged once at startup and surfaced in the dashboard.

**Round trip at baud N:**
```
round_trip(N) = 2 * (5 bytes * 10 bits / N) + 500us turnaround
```

**Phase 2 dwell (master) at baud N:**
```
dwell_phase2_master(N) = (sum of dwell_phase2_master(M)
                          for M slower than N)
                       + round_trip(N)
                       * 1.1   // 10% margin
```

**Phase 2 dwell (slave) at baud N:**
```
dwell_phase2_slave(N) = (sum of dwell_phase2_master(M)
                         for M from N down to slowest)
                      + round_trip(N)
                      * 1.1
```

**Phase 3 dwell at baud N:**
```
dwell_phase3(N) = 3 * round_trip(N) * 1.5
```

**Phase 1 dwell:**
```
dwell_phase1 = 5000ms  // per PING retry, unbounded total
```

**Example (5-baud list 115200, 57600, 38400, 19200, 9600):**
```
round_trip(115200) = 0.43ms + 0.5ms = 0.93ms
round_trip(57600)  = 0.87ms + 0.5ms = 1.37ms
round_trip(38400)  = 1.30ms + 0.5ms = 1.80ms
round_trip(19200)  = 2.60ms + 0.5ms = 3.10ms
round_trip(9600)   = 5.21ms + 0.5ms = 5.71ms

dwell_phase2_master(9600)   = 6.3ms
dwell_phase2_master(19200)  = 6.3 + 3.4 = 9.7ms
dwell_phase2_master(38400)  = 9.7 + 6.3 + 2.0 = 18.0ms
dwell_phase2_master(57600)  = 18.0 + 6.3 + 2.0 + 1.5 = 27.8ms
dwell_phase2_master(115200) = 27.8 + 6.3 + ... + 1.0 = 36.4ms

dwell_phase3(115200) = 3 * 0.93 * 1.5 = 4.2ms
dwell_phase3(9600)   = 3 * 5.71 * 1.5 = 25.7ms
```

**Best case** (3 acks at top baud): Phase 1 ~50ms + Phase 2 ~3ms + Phase 3 ~4ms = **~57ms cold start**.

**Worst case** (lock at slowest baud): Phase 1 ~50ms + Phase 2 ~80ms (full sweep) + Phase 3 ~26ms = **~156ms cold start**.

**Average case** (lock somewhere in the middle): ~80-120ms cold start.

## Wire format

### Control frames (5 bytes each)

```
[0xAA] [0x55] [cobsSeq] [cmd] [CRC8 of first 4 bytes]
```

- `0xAA 0x55` preamble
- `cobsSeq`: 1-byte sequence counter (same counter as data frames)
- `cmd`: command byte
- `CRC8`: CRC-8 of the first 4 bytes (preamble + cobsSeq + cmd)

**Command bytes:**
| Cmd | Value | Meaning |
|-----|-------|---------|
| `PING_CMD` | 0x22 | Master-to-slave: "are you there at this baud?" |
| `PONG_ACK_CMD` | 0x33 | Slave-to-master: "yes, I heard you" |
| `REQ_CMD` | 0x11 | Reserved (unused) |
| `BEST_CMD` | 0x44 | Reserved (unused) |

### BREAK signal

```
[break condition (15 bits low)] [hint byte]
```

The hint byte is the baud index to start the sweep at (0 = fastest,
`allowedBaudsCount-1` = slowest, 0xFF = no hint / start from 0).
A peer that doesn't read the hint byte still treats the signal
as a BREAK and resets, but will start the resweep at the top of
the baud list (fastest-first).

### Data frames

Frame format:
```
[0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]
```

## State machine

```
              +---------+
              |   IDLE  |  (before begin)
              +---------+
                    |
                    v
              +---------+
   +--------> | PHASE_1 |  (slowest baud, 1 ack to lock)
   |          +---------+
   |                | 1 PONG_ACK
   |                v
   |          +---------+
   |          | PHASE_2 |  (top-down, 1 ack per baud to promote)
   |          +---------+
   |                | 1 PONG_ACK
   |                v
   |          +---------+
   |          | PHASE_3 |  (3-ack gate at chosen baud)
   |          +---------+
   |                | 3 of 3 PONG_ACK
   |                v
   |          +-----+
   |          | OK  |  (preferredBaud_ = chosen baud)
   |          +-----+
   |                | error, idle, BREAK
   +----------------+
        (re-enter PHASE_2 from top of list)
```

OK transitions back to PHASE_2 on any drop. The climb is the
re-sweep itself; there's no separate climb timer.

## Logging

Every phase transition logs a clear banner. Operators can grep
for `PHASE 1`, `PHASE 2`, `PHASE 3` to see where the link is.

```
[I AutoLink] the current protocol protocol, baud table 115200, 57600, 38400, 19200, 9600
[I AutoLink] P1 dwell=5000ms (per retry, unbounded)
[I AutoLink] P2 dwell master: 115200=36ms, 57600=28ms, 38400=18ms, 19200=10ms, 9600=6ms
[I AutoLink] P2 dwell slave:  115200=109ms, 57600=72ms, 38400=44ms, 19200=26ms, 9600=6ms
[I AutoLink] P3 dwell max: 115200=4ms, ..., 9600=26ms
[D AutoLink] === PHASE 1: connect at slowest baud[4]=9600 ===
[D AutoLink] phase1: PING@9600 cobsSeq=0
[D AutoLink] phase1: PONG_ACK@9600 cobsSeq=0 -> LOCK at baud[4]
[D AutoLink] === PHASE 2: confirmation resweep, top-down ===
[D AutoLink] phase2: PING@115200 cobsSeq=1
[D AutoLink] phase2: PONG_ACK@115200 cobsSeq=1 -> PROMOTE
[D AutoLink] === PHASE 3: 3-ack gate at baud[0]=115200 ===
[D AutoLink] phase3: PING@115200 cobsSeq=2 -> ACK 1/3
[D AutoLink] phase3: PING@115200 cobsSeq=3 -> ACK 2/3
[D AutoLink] phase3: PING@115200 cobsSeq=4 -> ACK 3/3 -> CONFIRMED
[I AutoLink] OK at baud[0]=115200 (cold start 87ms)
```

## Why this is the right shape

**The boot race is gone.** Phase 1 uses the slowest baud. No
matter when Pong boots, the slowest baud has a long enough bit
period for Pong to come online and decode the next PING.

**The fastest baud is found when possible.** Phase 2 starts at
the top. If 115200 is clean, we lock there in `~3ms + 3 PONG_ACKs`
= `~12ms`. If 115200 is noisy but 57600 is clean, we lock at
57600 in `~36ms + 3 PONG_ACKs` = `~45ms`.

**The slowest baud is found when needed.** If only 9600 works,
Phase 2 walks all the way down in `~80ms` and Phase 3 verifies
in `~26ms`. Total cold start: `~150ms`.

**False locks are rejected.** Phase 3 demands 2-of-3. A wire that
gives 1 lucky PING and 2 misses passes any single-ack gate but
fails the 2-of-3 gate.

**Re-lock is fast.** On any drop, Phase 2 runs again from the
top. If the preferred baud is still reachable, the master locks
in one PING. If not, the sweep walks down. Worst case re-sweep
time: `~100ms`.

**The climb is automatic.** There's no separate climb timer.
Every re-sweep IS a climb attempt. If the link is healthy at
the preferred baud, the master stays. If the master is exploring
a faster baud, the next re-sweep will try the candidate baud
again.

