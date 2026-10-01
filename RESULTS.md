# Measured results

Numbers here are copied from real runs. Each section states where it ran.

## macOS development laptop (Apple Silicon, kqueue + recvfrom fallback path)

Not the target platform — included for completeness.

| target pps | achieved pps | loss | p50 µs | p99 µs |
|-----------:|-------------:|-----:|-------:|-------:|
| 100,000    | 99,999       | 0%   | 13     | 4,241  |
| 300,000    | 247,035      | 0%   | 13     | 4,205  |
| 500,000    | 266,183      | 0%   | 14     | 6,080  |

Sender-bound at ~260K pps (one `sendto` per datagram). The millisecond p99 tail is
scheduler wake-up latency on this OS, not queueing — `ring_full_drops` stayed 0.

Impairment run (`gs_sim --loss 0.05 --dup 0.01 --reorder 0.02`, 30,000 frames):
station reported 1,545 lost / 269 duplicates — identical to what the simulator injected.

## Linux (GitHub Actions, ubuntu-24.04)

Runner: 2 vCPU, AMD EPYC 9V74 (shared VM). epoll + recvmmsg/sendmmsg path.

**Correctness** — all 48 GoogleTest tests pass in Release, under ThreadSanitizer, and under
AddressSanitizer.

**Degraded link** (`scripts/netem.sh`: `tc qdisc add dev lo root netem loss 5% delay 2ms 1ms`,
60,000 frames at 20K pps):

| injected loss | measured loss | reordered (from jitter) | duplicates | CRC errors |
|--------------:|--------------:|------------------------:|-----------:|-----------:|
| 5.00%         | 4.93%         | 36,290                  | 0          | 0          |

7 of 60,000 frames were lost at the very end of their APID's stream; tail loss can't be
detected from sequence numbers alone (a real link would use a pass-end marker or timeout).

**Throughput / latency** (`gs_bench`, 2 workers):

| target pps | achieved pps | loss | p50 µs | p99 µs |
|-----------:|-------------:|-----:|-------:|-------:|
| 100,000    | 99,999       | 0%   | 251    | 2,614  |
| 250,000    | 146,948      | 0%   | 308    | 2,138  |
| 500,000    | 144,570      | 0%   | 561    | 2,125  |

The bench runs four busy threads (pacing sender, receiver, two workers) on two vCPUs, so
this measures CPU contention as much as the pipeline. Zero frames were lost and the rings
never filled at any rate. Numbers on dedicated ≥4-core Linux hardware are still to be
measured.
