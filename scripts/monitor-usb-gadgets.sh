#!/bin/bash

DISPLAY=:0
PATTERN="dwc2 .*\.usb: "
COOLDOWN=30 # seconds, adjust to taste
LAST_RUN=0

# Path to your script to run on a match
RUN_SCRIPT="/root/usb-gadgets.sh"

# You may want to add logging here as well.
echo "Starting USB Gadget Log Monitor..."

# Tail -f doesn't catch kernel messages as they happen, but dmesg --follow does.
dmesg --follow | while read -r line; do
    if [[ "$line" =~ $PATTERN ]]; then
        NOW=$(date +%s)
        if (( NOW - LAST_RUN > COOLDOWN )); then
            echo "[usb-gadget-monitor] Match: $line"
            "$RUN_SCRIPT" stop
            killall qpwgraph
            sleep 1
            "$RUN_SCRIPT" start
            sleep 2
            #ssh -X 127.0.0.1 qpwgraph &
            #"$RUN_SCRIPT"
            LAST_RUN=$NOW
            logger "[usb-gadget-monitor] Ran $RUN_SCRIPT on $line"
        else
            echo "[usb-gadget-monitor] Rate-limited: $line"
            logger "[usb-gadget-monitor] Rate-limited: $line"
        fi
    fi
done
