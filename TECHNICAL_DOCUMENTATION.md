# RK3568 USB Bridge Screen Technical Documentation

## 1. Overview

This project runs on an ALIENTEK ATK-DLRK3568 / RK3568 embedded Linux board. The board works as a USB composite device connected to a Windows PC and provides:

- USB HID keyboard and mouse automation.
- USB printer gadget capture.
- Report conversion and upload.
- Barcode-driven patient lookup.
- Local 480x320 Qt Quick status UI.

The implementation is written in C plus QML. It replaces an earlier Python implementation while keeping the runtime layout under `/root/usb_bridge`.

## 2. System Architecture

Runtime data flows through file queues and state files:

```text
Barcode scanner
  -> scan_patient_capture
  -> patient API
  -> project selection UI
  -> /root/usb_bridge/form_queue/form_*.json
  -> hid_executor
  -> /dev/hidg0 keyboard and /dev/hidg1 mouse
  -> wait_print state
  -> printer_capture
  -> /dev/g_printer0
  -> report PDF queue
  -> report_uploader
  -> Ghostscript PDF conversion
  -> upload API
```

The UI watches:

```text
/root/usb_bridge/state/ui_status.json
```

and refreshes every 500 ms.

## 3. Main Components

| File | Purpose |
| --- | --- |
| `scan_patient_capture.c` | Reads barcode scanner events, calls patient API, parses JSON, handles project selection, creates HID tasks. |
| `hid_executor.c` | Consumes form tasks and sends keyboard/mouse HID reports to the Windows host. |
| `printer_capture.c` | Captures printer gadget data from `/dev/g_printer0` and queues report data. |
| `report_uploader.c` | Converts captured report data to PDF with `gs` and uploads with `curl`. |
| `usb_bridge_common.c/.h` | Shared JSON helpers, logging, base64, atomic state writes. |
| `api_config.c/.h` | Runtime API and input-device configuration loader. |
| `usb_bridge_ui.qml` | 480x320 Qt Quick status UI. |
| `bridge_ui_qml` | QML launcher and Qt platform setup. |
| `bridge_ui.c` | Emergency C framebuffer fallback UI. |
| `S50usbdevice` | USB gadget initialization script. |
| `S98usbbridge` | Worker/UI supervisor init script. |
| `install_to_board.sh` | Board-side install script. |

## 4. Runtime Layout

Runtime root:

```sh
/root/usb_bridge
```

Important directories:

```sh
/root/usb_bridge/bin
/root/usb_bridge/config
/root/usb_bridge/logs
/root/usb_bridge/state
/root/usb_bridge/api_raw
/root/usb_bridge/form_queue
/root/usb_bridge/form_done
/root/usb_bridge/report_wait_queue
/root/usb_bridge/report_print_queue
/root/usb_bridge/report_pdf_queue
/root/usb_bridge/report_uploaded
/root/usb_bridge/report_error
/root/usb_bridge/ui
```

Important configuration files:

```sh
/root/usb_bridge/config/api.conf
/root/usb_bridge/config/ui.conf
```

Important logs:

```sh
/root/usb_bridge/logs/boot.log
/root/usb_bridge/logs/scan_patient_capture.log
/root/usb_bridge/logs/hid_executor.log
/root/usb_bridge/logs/printer_capture.log
/root/usb_bridge/logs/report_uploader.log
/root/usb_bridge/logs/bridge_ui_qml.log
```

## 5. Build

On the RK3568 SDK or Ubuntu host with the ALIENTEK Buildroot toolchain:

```sh
make clean
make CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
```

Single target examples:

```sh
make scan_patient_capture CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
make hid_executor CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
make sendkey CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
```

## 6. Installation

Copy files to the board, then run:

```sh
cd /tmp
chmod +x install_to_board.sh
./install_to_board.sh
/etc/init.d/S50usbdevice restart
/etc/init.d/S98usbbridge restart
```

Manual install example:

```sh
install -m 0755 hid_executor scan_patient_capture printer_capture report_uploader bridge_ui /root/usb_bridge/bin/
install -m 0755 bridge_ui_qml /root/usb_bridge/bin/
install -m 0644 usb_bridge_ui.qml /root/usb_bridge/ui/
install -m 0755 S50usbdevice /etc/init.d/S50usbdevice
install -m 0755 S98usbbridge /etc/init.d/S98usbbridge
```

## 7. Configuration

Recommended `/root/usb_bridge/config/ui.conf`:

```conf
BRIDGE_UI_MODE=qml
BRIDGE_UI_QML_PLATFORM=wayland
BRIDGE_UI_NO_FALLBACK=1
BRIDGE_UI_START_DELAY=8
BRIDGE_DISABLE_FACTORY_DESKTOP=1
BRIDGE_DISABLE_WESTON=0
BRIDGE_UI_MAX_FAILS=5
SCANNER_DEV=/dev/input/event8
PROJECT_INPUT_DEV=/dev/input/event5
```

Recommended `/root/usb_bridge/config/api.conf`:

```conf
PATIENT_QUERY_URL=http://192.168.112.139:9061/api/client/getTJPatientInfo
PATIENT_API_TIMEOUT=10

REPORT_UPLOAD_URL=http://8.148.73.190:5000/upload
REPORT_UPLOAD_FIELD_NAME=file
REPORT_UPLOAD_TIMEOUT=30

PROJECT_INPUT_DEV=/dev/input/event5
PROJECT_KEY_PREV=105
PROJECT_KEY_NEXT=106
PROJECT_KEY_CONFIRM=28
```

Input event numbers can change after reboot or USB re-enumeration. If scanning or project selection stops, verify `/dev/input/event*`.

## 8. Display Strategy

The board factory UI uses:

```sh
/etc/init.d/S49weston
/etc/init.d/S50systemui
```

The stable production strategy is:

- Keep Weston running as the Wayland compositor.
- Stop and disable only factory `systemui`.
- Run the custom QML UI on Wayland.

`S98usbbridge` implements this by default:

- Stops and disables `S50systemui`.
- Keeps or starts `S49weston`.
- Starts `bridge_ui_qml`.

`usb_bridge_ui.qml` opens a full-screen black Wayland window to cover desktop residue, but the actual UI panel remains fixed at 480x320 and centered. On the final 480x320 panel, this fills the screen exactly. On HDMI or PC displays, it remains a correctly sized 480x320 UI area.

## 9. UI Status Contract

Workers update:

```sh
/root/usb_bridge/state/ui_status.json
```

Common fields:

```json
{
  "time": "2026-05-07 16:47:31",
  "phase": "printing",
  "scan_text": "p2604270002",
  "patient_id": "P...",
  "report_no": "R...",
  "selected_index": 0,
  "project_count": 1,
  "project_options": ["exam item"]
}
```

Known phases:

```text
service_start
idle
scan
select_project
ready_input
inputting
wait_print
printing
print_done
converting
uploading
done
error
```

## 10. Project Selection

If the patient API returns one or more records, `scan_patient_capture` extracts unique `exam_item` values and displays them in the UI. Selection accepts:

- Previous: configured previous key plus arrow up/left.
- Next: configured next key plus arrow down/right.
- Confirm: configured confirm key plus Enter/KPEnter.

Field-tested input devices:

```conf
SCANNER_DEV=/dev/input/event8
PROJECT_INPUT_DEV=/dev/input/event5
```

## 11. Debug Commands

Check running services:

```sh
ps | grep -E 'usb_bridge|bridge_ui|qml|hid_executor|scan_patient_capture|printer_capture|report_uploader|weston|systemui' | grep -v grep
```

Check UI logs:

```sh
tail -n 120 /root/usb_bridge/logs/bridge_ui_qml.log
tail -n 120 /root/usb_bridge/logs/boot.log
cat /root/usb_bridge/state/ui_status.json
```

Check scanner:

```sh
tail -f /root/usb_bridge/logs/scan_patient_capture.log
cat /proc/bus/input/devices
```

Check HID:

```sh
ls -l /dev/hidg0 /dev/hidg1
ls -l /root/usb_bridge/form_queue /root/usb_bridge/form_done
tail -n 120 /root/usb_bridge/logs/hid_executor.log
```

Check printer gadget:

```sh
ls -l /dev/g_printer0
tail -n 120 /root/usb_bridge/logs/printer_capture.log
```

Restart:

```sh
/etc/init.d/S50usbdevice restart
/etc/init.d/S98usbbridge restart
```

## 12. Operational Notes

- Keep source and shell files as LF line endings. `.gitattributes` enforces this for C, headers, QML, Makefile, and init scripts.
- Avoid enabling the old C framebuffer UI in production unless QML cannot start.
- If QML starts twice, stop `S98usbbridge`, kill `qmlscene/qml`, remove `/tmp/bridge_ui.pid`, and restart.
- If QML reports `Failed to create wl_display`, confirm Weston is running and `BRIDGE_UI_QML_PLATFORM=wayland`.
- If `linuxfb` is used, Rockchip DRM/fbdev mmap issues may cause aborts on this board.
