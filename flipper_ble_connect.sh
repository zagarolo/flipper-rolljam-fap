#!/bin/bash
# Auto-reconnect ALL paired BLE/BT devices — forces connection even if multipoint
INTERVAL=8

while true; do
    while IFS= read -r line; do
        addr=$(echo "$line" | awk '{print $2}')
        [ -z "$addr" ] && continue
        if bluetoothctl info "$addr" 2>/dev/null | grep -q "Connected: yes"; then
            continue
        fi
        # Force connect — retry twice for multipoint devices like Pixel Buds
        timeout 5 bluetoothctl connect "$addr" >/dev/null 2>&1
        if ! bluetoothctl info "$addr" 2>/dev/null | grep -q "Connected: yes"; then
            timeout 5 bluetoothctl connect "$addr" >/dev/null 2>&1
        fi
    done < <(bluetoothctl devices Paired 2>/dev/null || bluetoothctl paired-devices 2>/dev/null)
    sleep "$INTERVAL"
done
