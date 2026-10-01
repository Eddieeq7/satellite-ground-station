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

_Pending first CI run._
