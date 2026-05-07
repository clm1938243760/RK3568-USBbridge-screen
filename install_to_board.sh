#!/bin/sh
set -e

BASE=/root/usb_bridge

mkdir -p "$BASE/bin" "$BASE/config" "$BASE/logs" "$BASE/ui"

install -m 0755 hid_executor scan_patient_capture printer_capture report_uploader bridge_ui "$BASE/bin/"
install -m 0755 bridge_ui_qml "$BASE/bin/"
install -m 0644 usb_bridge_ui.qml "$BASE/ui/"
install -m 0644 api.conf.example "$BASE/config/api.conf"
install -m 0755 S50usbdevice /etc/init.d/S50usbdevice
install -m 0755 S98usbbridge /etc/init.d/S98usbbridge

if [ -x /etc/init.d/S50systemui ]; then
    /etc/init.d/S50systemui stop 2>/dev/null || true
fi
if [ -e /etc/init.d/S50systemui ]; then
    chmod -x /etc/init.d/S50systemui 2>/dev/null || true
fi
killall systemui 2>/dev/null || true

if [ -e /etc/init.d/S49weston ]; then
    chmod +x /etc/init.d/S49weston 2>/dev/null || true
fi

if command -v update-rc.d >/dev/null 2>&1; then
    update-rc.d S50usbdevice defaults
    update-rc.d S98usbbridge defaults
elif command -v chkconfig >/dev/null 2>&1; then
    chkconfig --add S50usbdevice
    chkconfig --add S98usbbridge
else
    echo "No update-rc.d/chkconfig found; add /etc/init.d/S50usbdevice and S98usbbridge to boot manually."
fi

echo "Installed. Edit $BASE/config/api.conf, then run:"
echo "  /etc/init.d/S50usbdevice restart"
echo "  /etc/init.d/S98usbbridge restart"
