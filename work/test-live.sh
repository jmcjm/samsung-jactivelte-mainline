#!/bin/bash
# Insert krait-cc.ko on the running phone and watch whether it survives.
# PHONE / PHONE_PW as in flash-modules.sh.
set -u
PHONE=${PHONE:?set PHONE to the phone's ssh target}
PW=${PHONE_PW:?set PHONE_PW to the phone's sudo password}
W=$(cd "$(dirname "$0")" && pwd)
LOG="$W/dmesg-krait-$(date +%H%M%S).log"
echo "[*] dmesg stream -> $LOG"
ssh -o ConnectTimeout=10 -o ServerAliveInterval=2 $PHONE \
    "echo '$PW' | sudo -S dmesg -w" > "$LOG" 2>/dev/null &
LOGPID=$!
sleep 3
echo "[*] insmod krait-cc.ko"
timeout 45 ssh -o ConnectTimeout=10 $PHONE \
  "echo '$PW' | sudo -S sh -c 'sync; insmod /tmp/krait-cc.ko; echo RC=\$?'" 2>&1 \
  | grep -vE '^\[sudo|^Password' || echo "  >>> SSH DROPPED (the core died) <<<"
sleep 10
kill $LOGPID 2>/dev/null
echo "[*] system state:"
timeout 20 ssh -o ConnectTimeout=8 $PHONE 'uptime; lsmod | grep -i krait || echo "(not in lsmod)"' 2>&1 | tail -4 \
  || echo "  >>> SYSTEM NOT RESPONDING <<<"
echo "=== LOG ==="
grep -viE 'pmOS-rd' "$LOG" | tail -40
