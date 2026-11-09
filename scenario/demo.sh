#!/bin/sh
# Last Second demo, run at boot. Two failures of a 1 kHz pump loop, each caught by the recorder:
#   1. a runaway job above the pump's priority starves it for 20 ms (missed heartbeat);
#   2. the pump crashes (producer died).
# Each frozen window is dumped as CSV between markers on the console.
export PATH=/system/last-second:/system/bin:$PATH
cd /system/last-second
sleep 3
echo "===LS-BEGIN==="
blackboxd -n 30000 -p 200 &
sleep 1

echo "===LS-SCENE heartbeat==="
pump -d 5 &
sleep 3
hog -p 40 -b 20
sleep 1
bbtool status
echo "===LS-CSV-BEGIN heartbeat==="
bbtool dump -n 1500
echo "===LS-CSV-END heartbeat==="
slay -f pump
sleep 1
bbtool reset

echo "===LS-SCENE crash==="
pump -d 5 -c 2500 &
sleep 4
bbtool status
echo "===LS-CSV-BEGIN crash==="
bbtool dump -n 1500
echo "===LS-CSV-END crash==="
echo "core dumps:"; ls -l /data/var/dumper 2>/dev/null | grep -i pump
bbtool reset
echo "===LS-DONE==="
