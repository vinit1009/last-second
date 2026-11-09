#!/bin/sh
# Last Second stress run, at boot instead of the demo (SCENARIO=stress target/build.sh):
# eight 1 kHz producers write to one recorder, first alone, then while the service tool keeps
# freezing and reading the whole window. Every producer prints its write() latency at the end.
export PATH=/system/last-second:/system/bin:$PATH
cd /system/last-second
sleep 3
echo "===LS-BEGIN stress==="
blackboxd -n 30000 -p 200 &
sleep 1
for round in alone reader; do
  echo "===LS-ROUND $round==="
  for i in 1 2 3 4 5 6 7 8; do pump -N pump$i -d 5 -t 4 > /tmp/$round-$i.log 2>&1 & done
  sleep 1
  if [ "$round" = reader ]; then
    for k in 1 2 3; do
      bbtool trigger; sleep 1; bbtool dump > /dev/null 2>/tmp/dump-$k.log; tail -1 /tmp/dump-$k.log; bbtool reset
    done
  fi
  sleep 5
  cat /tmp/$round-*.log | grep "ticks;"
  bbtool reset 2>/dev/null
done
echo "===LS-DONE==="
