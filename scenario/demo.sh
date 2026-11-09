#!/bin/sh
# Last Second demo, run at boot. Two failures of a 1 kHz pump loop, each caught by the recorder:
#   1. a runaway job above the pump's priority starves it for 20 ms (missed heartbeat);
#   2. the pump crashes (producer died).
# Each frozen window is dumped as CSV between markers on the console.
export PATH=/system/last-second:/system/bin:$PATH
cd /system/last-second
MODE=$(cat secpol-mode 2>/dev/null || echo no)
# With a security policy, every process runs as its own type: only pump_t and bbtool_t may
# connect to the recorder (blackbox_t).
if [ "$MODE" = no ]; then T() { shift; "$@"; }; else T() { t=$1; shift; on -T "$t" "$@"; }; fi
sleep 3
echo "===LS-BEGIN $MODE==="
echo "windows saved by earlier boots:"; ls /data/var/blackbox 2>/dev/null | grep window- || echo "  none"
T blackbox_t blackboxd -n 30000 -p 200 -s /data/var/blackbox &
sleep 1

echo "===LS-SCENE heartbeat==="
T pump_t pump -d 5 &
sleep 3
T hog_t hog -p 40 -b 20
sleep 1
T bbtool_t bbtool status
echo "===LS-CSV-BEGIN heartbeat==="
T bbtool_t bbtool dump -n 1500
echo "===LS-CSV-END heartbeat==="
slay -f pump
sleep 1
T bbtool_t bbtool reset

echo "===LS-SCENE crash==="
rm -f /data/var/dumper/pump.core*   # the data partition persists between boots
T pump_t pump -d 5 -c 2500 &
sleep 4
T bbtool_t bbtool status
echo "===LS-CSV-BEGIN crash==="
T bbtool_t bbtool dump -n 1500
echo "===LS-CSV-END crash==="
echo "core dumps:"; ls -l /data/var/dumper 2>/dev/null | grep -i pump
T bbtool_t bbtool reset
echo "saved windows on disk (kept across reboots):"; ls -l /data/var/blackbox | grep window-
if [ "$MODE" != no ]; then
  echo "===LS-SCENE intruder==="
  # Any other type must be refused by the kernel before the recorder sees a message. (In open mode
  # this succeeds and secpolgenerate records it; that rule is removed from the policy by hand.)
  T intruder_t bbtool status
  T intruder_t sh -c 'echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx > /dev/blackbox' && echo "intruder: write accepted" || echo "intruder: write refused"
fi
if [ -d /dev/secpolgenerate ]; then
  echo "===LS-POLICY-BEGIN==="; cat /dev/secpolgenerate/policy; echo "===LS-POLICY-END==="
  echo "===LS-ERRORS-BEGIN==="; cat /dev/secpolgenerate/errors; echo "===LS-ERRORS-END==="
fi
echo "===LS-DONE==="
