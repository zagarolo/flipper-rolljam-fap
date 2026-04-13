#!/bin/bash
# =============================================
# Flipper FAP Emergency Kill — 3 escalation levels
# =============================================
# Uso: ./kill_fap.sh
#
# Livello 1: loader close (soft, chiede al loader di chiudere l'app)
# Livello 2: power reboot  (reboot del Flipper, pulito)
# Livello 3: USB unbind/rebind sysfs (ultimo resort, forza reset USB)
# =============================================

PORT=/dev/ttyACM0
PYTHON=/home/pi/.venv_ufbt/bin/python3

# =============================================
# REGOLA FERREA — non toccare MAI:
#   - SDR spettro (/tmp/show_spectrum, rtl_fm, rtl_433, rtl_sdr, rtl_tcp)
#   - assistente.service
#   - Quectel modem (wwan0, /dev/ttyUSB*, qmi_wwan)
#   - Qualsiasi processo NON legato a /dev/ttyACM0 (Flipper)
# Questo script agisce SOLO sull'USB device 0483:5740 (STM Flipper).
# =============================================

echo "================================"
echo " Flipper FAP Emergency Kill"
echo " (SDR + assistente + modem INTOCCATI)"
echo "================================"

if [ ! -e "$PORT" ]; then
    echo "[!] $PORT non presente. Flipper già scollegato?"
    exit 1
fi

# ─── LIVELLO 1: loader close ───
echo ""
echo "[1/3] loader close..."
$PYTHON - <<'PYEOF'
import serial, time, sys
try:
    s = serial.Serial('/dev/ttyACM0', 230400, timeout=2)
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b'\r\nloader close\r\n')
    time.sleep(1.5)
    out = s.read(s.in_waiting or 1).decode(errors='ignore')
    print('  →', out[:200].replace('\r',' ').replace('\n',' / ')[:200])
    s.close()
except Exception as e:
    print(f'  ! soft close fallito: {e}', file=sys.stderr)
PYEOF

sleep 1

# ─── LIVELLO 2: power reboot ───
echo ""
echo "[2/3] power reboot..."
$PYTHON - <<'PYEOF'
import serial, time, sys
try:
    s = serial.Serial('/dev/ttyACM0', 230400, timeout=2)
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b'\r\npower reboot\r\n')
    time.sleep(1)
    s.close()
    print('  → reboot inviato')
except Exception as e:
    print(f'  ! reboot fallito: {e}', file=sys.stderr)
PYEOF

# Aspetta reconnect
echo "  → attendo reconnect USB (max 8s)..."
for i in $(seq 1 8); do
    sleep 1
    if [ -e "$PORT" ]; then
        echo "  ✓ Flipper tornato online dopo ${i}s"
        echo ""
        echo "================================"
        echo " FAP KILLED (level 2 - reboot)"
        echo "================================"
        exit 0
    fi
done

# ─── LIVELLO 3: USB unbind/rebind ───
echo ""
echo "[3/3] USB unbind/rebind (ultimo resort)..."
# Trova il bus USB del Flipper (vid 0483 pid 5740)
USBDEV=$(for d in /sys/bus/usb/devices/*/; do
    if [ -f "$d/idVendor" ] && [ -f "$d/idProduct" ]; then
        vid=$(cat "$d/idVendor")
        pid=$(cat "$d/idProduct")
        if [ "$vid" = "0483" ] && [ "$pid" = "5740" ]; then
            basename "$d"
            break
        fi
    fi
done)

if [ -n "$USBDEV" ]; then
    echo "  → Flipper USB device: $USBDEV"
    echo "$USBDEV" | sudo tee /sys/bus/usb/drivers/usb/unbind > /dev/null 2>&1
    sleep 1
    echo "$USBDEV" | sudo tee /sys/bus/usb/drivers/usb/bind   > /dev/null 2>&1
    sleep 3
    if [ -e "$PORT" ]; then
        echo "  ✓ Flipper tornato online via USB rebind"
        echo ""
        echo "================================"
        echo " FAP KILLED (level 3 - USB reset)"
        echo "================================"
        exit 0
    else
        echo "  ! Flipper non torna online. Scollega fisicamente il cavo USB."
        exit 2
    fi
else
    echo "  ! USB device Flipper non trovato in sysfs"
    echo "  ! Scollega fisicamente il cavo USB."
    exit 3
fi
