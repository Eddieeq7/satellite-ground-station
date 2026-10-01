#!/usr/bin/env bash
# Degraded-link test on Linux: impair the loopback interface with tc netem,
# stream telemetry through it, and check the station's loss accounting.
#
#   sudo scripts/netem.sh [build_dir]
#
# netem on `lo` applies to both directions of every loopback flow, so only the
# simulator→station path matters here (UDP has no return traffic).
set -euo pipefail
BUILD=${1:-build}
PORT=9400
COUNT=60000
RATE=20000           # packets/s → ~3 s of traffic
LOSS=5               # percent
DELAY="2ms 1ms"      # 2 ms ± 1 ms jitter (jitter causes reordering)

cleanup() { tc qdisc del dev lo root 2>/dev/null || true; }
trap cleanup EXIT

tc qdisc add dev lo root netem loss ${LOSS}% delay ${DELAY}
echo "lo qdisc: $(tc qdisc show dev lo)"

"$BUILD/gs_station" --port $PORT --workers 2 --seconds 6 > station.json &
STATION=$!
sleep 0.5
"$BUILD/gs_sim" --port $PORT --count $COUNT --rate $RATE --apids 3 | tee sim.json
wait $STATION
cleanup
trap - EXIT

echo "station report:"; cat station.json
python3 - "$COUNT" "$LOSS" <<'PY'
import json, sys
count, loss = int(sys.argv[1]), float(sys.argv[2])
r = json.load(open("station.json"))
expected_lost = count * loss / 100
measured = r["lost"] / count * 100
print(f"injected {loss:.1f}% loss · station measured {measured:.2f}% lost, "
      f"{r['reordered']} reordered, {r['duplicates']} duplicates, {r['crc_errors']} CRC errors")
# Losses at the very end of a stream are invisible to sequence numbers (no later
# packet reveals the gap), so allow a small unaccounted tail per APID.
unaccounted = count - (r["received"] + r["lost"])
print(f"tail losses not detectable from sequence numbers: {unaccounted}")
assert 0 <= unaccounted <= 30, "all but trailing losses must be accounted for"
assert abs(measured - loss) < 1.0, "measured loss should be within 1 pt of injected loss"
print("PASS")
PY
