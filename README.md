# Satellite Telemetry Ground Station

A C++20 Linux ground station that receives simulated satellite telemetry over UDP,
validates every frame, and accounts for every packet that was dropped, duplicated,
or delivered out of order — per subsystem.

```
  gs_sim (satellite)                       gs_station
  ┌────────────────┐   UDP    ┌─────────────────────────────────────────────────┐
  │ EPS   apid=1   │ ───────► │ receive thread                                  │
  │ ADCS  apid=2   │  frames  │   epoll (edge-triggered) → recvmmsg ×64 → shard │
  │ THERM apid=3   │          │            │ apid % N                           │
  └────────────────┘          │            ▼                                    │
         ▲                    │   SpscRing<RawFrame>  ×N   (lock-free, 1/worker) │
   tc netem: loss,            │            ▼                                    │
   delay, jitter              │ worker threads                                  │
                              │   decode → CRC-32 → SequenceTracker → latency   │
                              └─────────────────────────────────────────────────┘
```

## Wire format

| offset | size | field        | notes                                   |
|-------:|-----:|--------------|-----------------------------------------|
| 0      | 2    | magic        | `0x5A47`                                |
| 2      | 1    | version      | `1`                                     |
| 3      | 1    | apid         | subsystem id; also the shard key        |
| 4      | 4    | seq          | per-APID, wraps at 2³²                  |
| 8      | 8    | tx_ns        | sender `CLOCK_MONOTONIC`                |
| 16     | 2    | payload_len  | ≤ 1024                                  |
| 18     | N    | payload      |                                         |
| 18+N   | 4    | crc32        | IEEE 802.3 over bytes `[0, 18+N)`       |

Little-endian, serialized byte-by-byte (no struct casting, no padding assumptions).
Decoding rejects short frames, bad magic/version, length mismatches, oversized
payloads, and CRC failures — a test flips every single bit of a frame and checks
each one is rejected.

## Design decisions

**One SPSC ring per worker, sharded by APID.** A single-producer/single-consumer ring
needs only an acquire/release pair per operation, no CAS loops. Sharding by APID keeps
every ring truly SPSC *and* gives each subsystem's sequence space to exactly one worker,
so `SequenceTracker` has no locks at all.

**Ring internals** (`include/gs/spsc_ring.hpp`): power-of-two capacity (mask, not modulo);
head/tail on separate cache lines; each side caches the other's index and only reloads it
when the ring looks full/empty, so the hot path usually touches no shared cache line.

**Edge-triggered epoll + drain-to-EAGAIN.** One wakeup per burst instead of per packet;
on Linux, `recvmmsg` pulls up to 64 datagrams per syscall. (On macOS the poller falls back
to kqueue and `recvfrom` so the project still builds and tests on a laptop.)

**Sequence tracking** (`src/sequence_tracker.cpp`): a 1024-entry sliding bitmap of
recently-seen sequence numbers. Forward jumps count the gap as lost; a later arrival
inside the window that wasn't seen is reclassified from *lost* to *reordered*; anything
seen is a duplicate. Signed 32-bit distance handles wrap-around.

**Backpressure is visible, not silent.** If a ring is full the receiver drops and counts
it (`ring_full_drops`) rather than blocking the socket drain and letting the kernel drop
invisibly.

## Build, test, run

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure            # 48 GoogleTest tests

cmake -S . -B build-tsan -DGS_SANITIZE=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j && ./build-tsan/gs_tests  # ThreadSanitizer

./build/gs_station --port 9000 --workers 2 --seconds 10 &
./build/gs_sim --port 9000 --count 100000 --rate 20000 --loss 0.05 --dup 0.01 --reorder 0.02

sudo scripts/netem.sh build                           # Linux: real 5% loss + jitter on lo
./build/gs_bench 500000 5 2                           # loopback throughput / latency
```

## Results

CI (`.github/workflows/ci.yml`) runs the test suite plain, under ThreadSanitizer and
AddressSanitizer, runs the netem degraded-link check, and runs the benchmark on an
Ubuntu runner. See [`RESULTS.md`](RESULTS.md) for measured numbers and the hardware they
were measured on.
