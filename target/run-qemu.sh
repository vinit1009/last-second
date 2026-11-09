#!/bin/bash
# Boots the image in QEMU, lets the demo run, and saves the console log and each frozen window.
#   target/run-qemu.sh [image-dir] [out-dir]
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${1:-$REPO/../last-second-image}
OUT=${2:-$REPO/runs}
mkdir -p "$OUT"
LOG="$OUT/serial.log"
rm -f "$LOG"
qemu-system-x86_64 -pidfile "$OUT/qemu.pid" -display none -monitor none -serial "file:$LOG" \
  -m 1G -smp 1 -cpu max -icount shift=0,sleep=off \
  -drive "file=$IMAGE/output/disk-qemu.vmdk,if=ide,id=drv0" -kernel "$IMAGE/output/ifs.bin" &
for _ in $(seq 1 150); do
  grep -q "===LS-DONE===" "$LOG" 2>/dev/null && break
  sleep 2
done
kill "$(cat "$OUT/qemu.pid")" 2>/dev/null || true
tr -d '\r' < "$LOG" > "$LOG.tmp" && mv "$LOG.tmp" "$LOG"
grep -q "===LS-DONE===" "$LOG" || { echo "the demo did not finish; see $LOG" >&2; exit 1; }
for scene in heartbeat crash; do
  awk "/===LS-CSV-BEGIN $scene===/{f=1;next}/===LS-CSV-END $scene===/{f=0}f" "$LOG" | grep -E '^(t_ms|-?[0-9])' > "$OUT/$scene.csv"
  echo "$OUT/$scene.csv: $(($(wc -l < "$OUT/$scene.csv") - 1)) samples"
done
grep -E "^(blackboxd|pump|hog|bbtool|state|trigger|then):|pump.*core" "$LOG" || true
