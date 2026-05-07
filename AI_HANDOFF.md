# RK3568 USB Bridge AI Handoff Notes

> Purpose: this document is written for a future AI assistant or engineer taking over this project. It summarizes the current design, board environment, important files, bugs encountered, fixes applied, and the practical debugging knowledge learned during field testing.

## 1. Project Goal

The project runs on an ALIENTEK ATK-DLRK3568 / RK3568 embedded Linux board.

The board acts as a USB composite device connected to a Windows PC:

- HID keyboard: automatically types patient/report information into a Windows application.
- HID mouse: automatically clicks configured screen coordinates.
- USB printer gadget: captures report print data from the Windows PC.
- PDF conversion/upload: converts captured print data to PDF and uploads it.
- Local 480x320 status UI: shows service state, scan status, project selection, HID input, print, and upload progress.

The original implementation was Python. This repository is the C/QML implementation.

## 2. Current Repository Layout

Important files:

- `scan_patient_capture.c`
  - Reads barcode scanner input from Linux input events.
  - Calls the patient API with `curl`.
  - Parses API JSON.
  - Handles project selection from `exam_item`.
  - Generates HID form-fill tasks into `/root/usb_bridge/form_queue`.
  - Writes patient/report state files.

- `hid_executor.c`
  - Consumes `/root/usb_bridge/form_queue/form_*.json`.
  - Sends USB HID keyboard and mouse reports through `/dev/hidg0` and `/dev/hidg1`.
  - Handles ASCII typing and Chinese text paste through Windows PowerShell clipboard.
  - Guards against Windows Chinese IME by forcing CapsLock during ASCII typing.

- `printer_capture.c`
  - Watches report wait queue.
  - Reads print job data from `/dev/g_printer0`.
  - Writes print jobs into report PDF queue.
  - Updates UI status: `wait_print`, `printing`, `print_done`, `error`.

- `report_uploader.c`
  - Converts captured reports to PDF using Ghostscript (`gs`).
  - Uploads PDF using `curl`.
  - Updates UI status: `converting`, `uploading`, `done`, `idle`, `error`.

- `usb_bridge_common.c/.h`
  - Shared JSON parser/writer.
  - Logging.
  - Atomic file writes.
  - Base64.
  - UI status writing.

- `api_config.c/.h`
  - Reads `/root/usb_bridge/config/api.conf`.
  - Reads environment overrides.
  - Stores API URLs, timeouts, project-selection key codes, project input device.

- `usb_bridge_ui.qml`
  - Main QML UI.
  - Reads `/root/usb_bridge/state/ui_status.json`.
  - Displays status and project choices.
  - Static Chinese strings are written as `\uXXXX` escapes to avoid encoding damage.

- `bridge_ui_qml`
  - Shell launcher for QML UI.
  - Defaults to auto platform selection because forced `linuxfb`/`eglfs` caused issues on this board.

- `bridge_ui.c`
  - Old C framebuffer fallback UI.
  - Kept as emergency fallback only.
  - It was disabled in production because it showed an old status bar and lacked project options.

- `S50usbdevice`
  - Initializes USB gadget functions.

- `S98usbbridge`
  - Starts/stops supervisors for UI and worker processes.
  - Reads `/root/usb_bridge/config/ui.conf`.
  - Exports `SCANNER_DEV`, `PROJECT_INPUT_DEV`, and UI variables.

- `sendkey.c`
  - Small test tool to simulate Linux input key events.
  - Useful for testing GPIO/project selection keys.

- `api.conf.example`
  - Example runtime configuration for API and input devices.

- `install_to_board.sh`
  - Installs binaries/scripts/config to the RK3568 board.

## 3. Board Runtime Paths

Runtime root:

```sh
/root/usb_bridge
```

Important runtime directories:

```sh
/root/usb_bridge/bin
/root/usb_bridge/config
/root/usb_bridge/logs
/root/usb_bridge/state
/root/usb_bridge/api_raw
/root/usb_bridge/form_queue
/root/usb_bridge/form_done
/root/usb_bridge/error
/root/usb_bridge/report_wait_queue
/root/usb_bridge/report_wait_done
/root/usb_bridge/report_print_queue
/root/usb_bridge/report_pdf_queue
/root/usb_bridge/report_uploaded
/root/usb_bridge/report_error
/root/usb_bridge/ui
```

Important runtime files:

```sh
/root/usb_bridge/config/api.conf
/root/usb_bridge/config/ui.conf
/root/usb_bridge/config/MarkInfo_SearchTitle_Config_100.json
/root/usb_bridge/state/ui_status.json
/root/usb_bridge/state/current_patient.json
```

Logs:

```sh
/root/usb_bridge/logs/boot.log
/root/usb_bridge/logs/scan_patient_capture.log
/root/usb_bridge/logs/hid_executor.log
/root/usb_bridge/logs/printer_capture.log
/root/usb_bridge/logs/report_uploader.log
/root/usb_bridge/logs/bridge_ui_qml.log
```

## 4. Current Known Runtime Configuration

The board was field-tested with:

```sh
SCANNER_DEV=/dev/input/event8
PROJECT_INPUT_DEV=/dev/input/event5
```

Meaning:

- Barcode scanner is currently `/dev/input/event8`.
- GPIO/project-selection keys are currently `/dev/input/event5`.
- The scanner event may change after reboot or USB re-enumeration, so verify if scanning stops.

Recommended `/root/usb_bridge/config/ui.conf`:

```conf
BRIDGE_UI_MODE=qml
BRIDGE_UI_QML_PLATFORM=wayland
BRIDGE_UI_NO_FALLBACK=1
BRIDGE_UI_START_DELAY=8
BRIDGE_DISABLE_FACTORY_DESKTOP=1
BRIDGE_DISABLE_WESTON=0
BRIDGE_UI_MAX_FAILS=5
BRIDGE_UI_WAYLAND_WAIT=30
SCANNER_DEV=/dev/input/event8
PROJECT_INPUT_DEV=/dev/input/event5
```

Recommended `/root/usb_bridge/config/api.conf` includes:

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

Key codes:

```text
KEY_UP      = 103
KEY_LEFT    = 105
KEY_RIGHT   = 106
KEY_DOWN    = 108
KEY_ENTER   = 28
KEY_KPENTER = 96
```

The project selector accepts:

- Previous: configured previous key, plus arrow up/left.
- Next: configured next key, plus arrow down/right.
- Confirm: configured confirm key, plus Enter/KPEnter.

External USB keyboard support:

- Project selection listens to `PROJECT_INPUT_DEV` first.
- It also listens to other `/dev/input/event*` devices during selection.
- Therefore an external keyboard can use up/down arrows and Enter.

## 5. Build Commands

On Ubuntu with the ALIENTEK Buildroot toolchain:

```sh
cd ~/test/c
make clean
make CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
```

Single target examples:

```sh
make scan_patient_capture CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
make hid_executor CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
make sendkey CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
```

Upload examples:

```sh
scp scan_patient_capture hid_executor bridge_ui_qml usb_bridge_ui.qml S98usbbridge root@192.168.110.78:/tmp/
```

Board install examples:

```sh
install -m 0755 /tmp/scan_patient_capture /root/usb_bridge/bin/scan_patient_capture
install -m 0755 /tmp/hid_executor /root/usb_bridge/bin/hid_executor
install -m 0755 /tmp/bridge_ui_qml /root/usb_bridge/bin/bridge_ui_qml
install -m 0644 /tmp/usb_bridge_ui.qml /root/usb_bridge/ui/usb_bridge_ui.qml
install -m 0755 /tmp/S98usbbridge /etc/init.d/S98usbbridge
```

Restart:

```sh
/etc/init.d/S98usbbridge restart
```

## 6. Main Workflow

Normal business flow:

1. `scan_patient_capture` reads barcode scanner key events.
2. It builds a SQL query and calls patient API with `curl`.
3. Raw API response is stored in `/root/usb_bridge/api_raw/api_*.json`.
4. JSON response is parsed.
5. If response contains multiple rows, unique `exam_item` values are shown in UI.
6. User selects an option with GPIO keys or external keyboard.
7. Selected full record is used to generate `/root/usb_bridge/form_queue/form_*.json`.
8. `hid_executor` consumes the form task.
9. It sends mouse clicks and keyboard/paste input to the Windows PC.
10. State changes to `wait_print`.
11. `printer_capture` waits for the Windows PC to print the report to USB printer gadget.
12. Print data is captured and queued.
13. `report_uploader` converts to PDF with `gs`.
14. PDF is uploaded with `curl`.
15. UI shows `done`, then returns to `idle`.

## 7. UI Status Contract

All UI updates are written to:

```sh
/root/usb_bridge/state/ui_status.json
```

Common fields:

```json
{
  "time": "2026-05-07 15:46:52",
  "phase": "select_project",
  "scan_text": "p2604270002",
  "patient_id": "P...",
  "report_no": "R...",
  "selected_index": 0,
  "project_count": 1,
  "project_options": ["人体成分检查"]
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

Important QML note:

- Some Qt versions return `xhr.status == 0` for local `file://` XMLHttpRequest.
- The QML must accept both status `200` and `0`.
- This was a real bug: UI started but did not react until `status === 0` was accepted.

## 8. Bugs Encountered And Fixes

### 8.1 Wrong architecture / missing binaries

Earlier logs showed executables missing or not executable:

```text
/root/usb_bridge/bin/hid_executor: No such file or directory
```

Root causes:

- Python files were present but C binaries were expected.
- Cross-compiled files were installed into the wrong directory.
- Some binaries were not copied to `/root/usb_bridge/bin`.

Fix:

- Cross-compile with the Buildroot aarch64 toolchain.
- Install C binaries into `/root/usb_bridge/bin`.

### 8.2 Remote SSH host key changed after reflashing board

After board reflashing:

```text
WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED
```

Fix:

```sh
ssh-keygen -f "/home/alientek/.ssh/known_hosts" -R "192.168.110.78"
```

### 8.3 Scanner event number changed

The scanner was once detected incorrectly as `event8` while the real device was `event5`. Later after changes/reboot:

```text
event8 - USBKey Chip USBKey Module
```

Field result:

- Current scanner is `/dev/input/event8`.
- GPIO/project keys are `/dev/input/event5`.

Current design:

- `SCANNER_DEV` can force scanner input.
- `PROJECT_INPUT_DEV` can force project-selection input.

Important:

- `SCANNER_DEV` is read from environment by `scan_patient_capture`.
- `S98usbbridge` exports variables from `ui.conf` so children receive it.

### 8.4 `PROJECT_INPUT_DEV` not exported to child processes

Symptom:

```text
project_input_dev=<all>
```

Even though config existed elsewhere.

Root cause:

- `S98usbbridge` sourced `ui.conf`, but shell variables were not exported.
- Child process did not receive `PROJECT_INPUT_DEV`.

Fix:

`S98usbbridge` now exports:

```sh
export SCANNER_DEV PROJECT_INPUT_DEV BRIDGE_UI_MODE BRIDGE_UI_QML_PLATFORM BRIDGE_UI_NO_FALLBACK BRIDGE_UI_START_DELAY
```

### 8.5 API returns multiple projects

Sample API response:

```json
{
  "code": "SUCCESS",
  "data": [
    {"exam_item": "耳石复位治疗,耳鸣评估,真菌荧光检查,视力检查", "...": "..."},
    {"exam_item": "人体成分检查", "...": "..."}
  ],
  "success": true
}
```

Requirement:

- Show options after API response.
- Show `exam_item`.
- If multiple rows return, show all options.
- If one row returns, show one option.
- Confirm before HID input.

Implementation:

- `select_record_index()` creates the selection UI status.
- It waits for project keys.
- Confirm returns selected original record index.

### 8.6 Duplicate project options

Symptom:

- API returned duplicate rows.
- UI showed repeated `exam_item` options.

Fix:

- Project selection now deduplicates by trimmed `exam_item`.
- Duplicate rows are logged:

```text
project duplicate skipped record=... same_as_option=... item=...
```

### 8.7 Scanner and GPIO on same event

At one stage scanner and GPIO were both thought to be event5.

Problems:

- If selecting listens to all events, scanner Enter and GPIO Enter can collide.
- If scanner phase listens to all events, GPIO arrow keys may leak into scan buffer.

Current behavior:

- Project selection opens configured `PROJECT_INPUT_DEV` first, then also listens to other event devices for external keyboard support.
- Scanner mode ignores navigation keys like up/down/left/right.
- Confirm Enter is still used both as barcode terminator and project confirm, depending on phase.

### 8.8 External keyboard project selection

Requirement:

- External keyboard should select options with up/down arrows and Enter.

Fix:

- Project selection listens to `PROJECT_INPUT_DEV` plus all other `/dev/input/event*`.
- Accepted keys:
  - Up/left: previous.
  - Down/right: next.
  - Enter/KPEnter: confirm.

### 8.9 Duplicate scan / scanning twice

Symptom:

- Scanning twice caused another task to be generated.
- A too-aggressive first fix blocked scans based on UI phase and caused “scan no response”.

Final fix:

- Removed phase-based workflow lock.
- Added simple debounce:

```text
Ignore same scan text repeated within 3 seconds.
```

This prevents accidental double-trigger but does not lock out the next real scan.

### 8.10 HID appeared to finish but did not input

Symptoms:

- UI showed input complete.
- Nothing appeared on Windows PC.
- `form_queue` still had a pending form task in one case.

Diagnosis steps:

```sh
ps | grep hid_executor | grep -v grep
ls -l /root/usb_bridge/form_queue
ls -l /root/usb_bridge/form_done
tail -n 120 /root/usb_bridge/logs/hid_executor.log
```

Root causes seen:

- `hid_executor` not running.
- Form task generated but not consumed.
- In other cases, HID events were sent but Windows focus/coordinates/input method caused no visible input.

Fixes:

- Added `event_count` logging in `hid_executor`.
- If event count is 0, it now reports error instead of pretending success.
- Ensure `hid_executor` is started by `S98usbbridge`.

### 8.11 Windows Chinese input method interfering with ASCII typing

Symptom:

- Windows PC had Chinese IME active.
- HID typing into CMD/text boxes produced Chinese or candidate input.

Fix:

- Before ASCII typing and before typing PowerShell clipboard command, `hid_executor` temporarily forces CapsLock ON.
- It converts ASCII letters to lowercase while CapsLock is on, so Windows receives uppercase typed characters.
- After typing, it restores previous CapsLock state.

CapsLock details:

- HID key code:

```c
KEY_CAPSLOCK = 0x39
```

- Host LED status byte:

```text
0x00 = CapsLock off
0x02 = CapsLock on
0x03 = NumLock + CapsLock
```

Code logic:

```c
(b[0] & 2) != 0
```

### 8.12 QML UI can show, but did not update

Symptom:

- QML window appeared.
- UI did not respond to status changes.

Root cause:

- Qt returned `xhr.status == 0` for local file reads.
- QML only accepted status 200.

Fix:

```qml
if (xhr.readyState === XMLHttpRequest.DONE && (xhr.status === 200 || xhr.status === 0)) {
    ...
}
```

Debug method:

Manually write status:

```sh
cat > /root/usb_bridge/state/ui_status.json <<'EOF'
{
  "time": "TEST-123",
  "phase": "select_project",
  "scan_text": "P2508020041190853",
  "patient_id": "P2508020041190853",
  "report_no": "R2026042218060300001",
  "selected_index": 1,
  "project_count": 2,
  "project_options": ["耳石复位治疗,耳鸣评估", "人体成分检查"]
}
EOF
```

The UI should change immediately.

### 8.13 QML startup failures and old UI fallback

Multiple attempts had QML aborts:

```text
bridge_ui_qml exited rc=134
Aborted
```

There was also an old C fallback UI that appeared and confused debugging.

Fixes:

- `bridge_ui_qml` defaults to platform `auto`, not forced `linuxfb` or `eglfs`.
- Current `bridge_ui_qml` resolves `auto` itself: Wayland when `WAYLAND_DISPLAY` exists, X11 when `DISPLAY` exists, otherwise `linuxfb` when `/dev/fb0` exists, otherwise `minimal`.
- `S98usbbridge` supports:

```conf
BRIDGE_UI_MODE=qml
BRIDGE_UI_NO_FALLBACK=1
```

- Old `bridge_ui` fallback can be disabled:

```sh
chmod -x /root/usb_bridge/bin/bridge_ui
```

### 8.14 Rockchip framebuffer mmap crash

Kernel trace:

```text
rockchip_drm_gem_object_mmap
rockchip_fbdev_mmap
fb_mmap
```

Root cause:

- Qt `linuxfb` and old C UI `mmap(/dev/fb0)` hit a Rockchip DRM/fbdev driver issue.

Fix:

- Do not force QML `linuxfb`.
- C UI defaults to write-based framebuffer access instead of mmap.
- QML became the main UI after the desktop system was disabled.

### 8.15 Board desktop / Weston interfered with our UI

Found:

```text
/etc/init.d/S49weston
/etc/init.d/S50systemui
```

These were the board’s factory desktop/system UI.

Commands used:

```sh
/etc/init.d/S50systemui stop 2>/dev/null
/etc/init.d/S49weston stop 2>/dev/null
killall systemui 2>/dev/null
killall weston 2>/dev/null
chmod -x /etc/init.d/S49weston
chmod -x /etc/init.d/S50systemui
```

Current recommended behavior:

- Keep Weston running as the Wayland compositor.
- Stop and disable only factory `systemui`.
- Run the custom QML UI with `BRIDGE_UI_QML_PLATFORM=wayland`.
- `usb_bridge_ui.qml` is a frameless `Window` and `bridge_ui_qml` sets `QT_WAYLAND_DISABLE_WINDOWDECORATION=1` to avoid visible desktop window frames.
- `usb_bridge_ui.qml` uses a full-screen black Wayland window, but keeps the actual UI panel fixed at 480x320 and centered. This hides Weston desktop residue on HDMI while matching the future 480x320 panel.
- `bridge_ui_qml` waits for a Wayland socket at startup and starts `S49weston` if needed. It auto-detects sockets under `/run/user/0`, `/run`, and `/var/run`. Default wait: `BRIDGE_UI_WAYLAND_WAIT=30`.
- `bridge_ui_qml` sets `QML_XHR_ALLOW_FILE_READ=1` and uses Qt Quick software rendering by default on Wayland to avoid local-file warnings and Mali buffer-sharing failures.
- Set `BRIDGE_DISABLE_WESTON=1` only when deliberately testing non-Wayland UI backends.

After this, the custom QML UI could own the display.

Important after disabling Weston:

- Qt's own `auto` platform choice may still try `wayland` and abort with `Failed to create wl_display`.
- Keep `BRIDGE_UI_QML_PLATFORM=auto` only with the updated `bridge_ui_qml` script, or explicitly set `BRIDGE_UI_QML_PLATFORM=linuxfb`.
- Remove `BRIDGE_UI_DISABLE=1` from `ui.conf` when the screen should show the custom UI.

Current code behavior:

- `S98usbbridge start` now disables the factory desktop automatically by default.
- `install_to_board.sh` also stops and removes execute permission from `/etc/init.d/S49weston` and `/etc/init.d/S50systemui`.
- `S98usbbridge` stops the UI supervisor after repeated QML crashes instead of restarting forever. Default limit: `BRIDGE_UI_MAX_FAILS=5`.
- To skip this behavior for debugging, set this in `/root/usb_bridge/config/ui.conf` before starting `S98usbbridge`:

```conf
BRIDGE_DISABLE_FACTORY_DESKTOP=0
```

To allow unlimited UI restarts while debugging, set:

```conf
BRIDGE_UI_MAX_FAILS=0
```

### 8.16 Old UI still appeared after closing desktop

Symptom:

- Factory desktop was closed.
- Old custom status bar appeared, without project options.

Root cause:

- Old C fallback `bridge_ui` or old QML process was still running.

Fix:

```sh
pkill -9 -f /root/usb_bridge/bin/bridge_ui
pkill -9 -f /root/usb_bridge/bin/bridge_ui_qml
pkill -9 -f qmlscene
pkill -9 -f qml
rm -f /tmp/bridge_ui.pid
```

And set:

```conf
BRIDGE_UI_MODE=qml
BRIDGE_UI_NO_FALLBACK=1
```

### 8.17 Supervisor restart problem

Symptom:

- Restarting S98 killed children, but old supervisors restarted them again.
- Logs showed duplicate workers:

```text
bridge_ui run ...
bridge_ui run ...
hid_executor run ...
hid_executor run ...
```

Fix:

- `S98usbbridge stop_all()` kills supervisor PIDs from `/tmp/*.pid`.
- `supervise()` now traps `TERM` and kills child before exiting.

## 9. Useful Debug Commands

### Check running services

```sh
ps | grep -E 'usb_bridge|bridge_ui|qml|hid_executor|scan_patient_capture|printer_capture|report_uploader' | grep -v grep
```

### Check UI status

```sh
cat /root/usb_bridge/state/ui_status.json
tail -n 100 /root/usb_bridge/logs/bridge_ui_qml.log
```

### Check scanner

```sh
tail -f /root/usb_bridge/logs/scan_patient_capture.log
SCANNER_DEV=/dev/input/event8 /root/usb_bridge/bin/scan_patient_capture
```

Expected scan logs:

```text
scanner found: /dev/input/event8
listening on /dev/input/event8
SCAN: ...
request api POST json: ...
api raw saved: ...
project option ...
project select waiting ...
```

### Check API raw response

```sh
ls -lt /root/usb_bridge/api_raw | head
cat /root/usb_bridge/api_raw/$(ls -t /root/usb_bridge/api_raw | grep '^api_.*\.json$' | head -n 1)
```

### Check HID queue

```sh
ls -l /root/usb_bridge/form_queue
ls -l /root/usb_bridge/form_done
tail -n 120 /root/usb_bridge/logs/hid_executor.log
```

### Check HID devices

```sh
ls -l /dev/hidg0 /dev/hidg1
```

If missing:

```sh
/etc/init.d/S50usbdevice restart
sleep 2
ls -l /dev/hidg0 /dev/hidg1
```

### Simulate project selection key

Build `sendkey`:

```sh
make sendkey CC=/opt/atk-dlrk356x-toolchain/bin/aarch64-buildroot-linux-gnu-gcc
```

Use on board:

```sh
/root/usb_bridge/bin/sendkey /dev/input/event5 106  # next
/root/usb_bridge/bin/sendkey /dev/input/event5 105  # previous
/root/usb_bridge/bin/sendkey /dev/input/event5 28   # confirm
```

### Kill UI processes

```sh
pkill -9 -f /root/usb_bridge/bin/bridge_ui
pkill -9 -f /root/usb_bridge/bin/bridge_ui_qml
pkill -9 -f qmlscene
pkill -9 -f qml
rm -f /tmp/bridge_ui.pid
```

### Disable board desktop

```sh
/etc/init.d/S50systemui stop 2>/dev/null
/etc/init.d/S49weston stop 2>/dev/null
killall systemui 2>/dev/null
killall weston 2>/dev/null
chmod -x /etc/init.d/S49weston
chmod -x /etc/init.d/S50systemui
```

Restore:

```sh
chmod +x /etc/init.d/S49weston
chmod +x /etc/init.d/S50systemui
/etc/init.d/S49weston start
/etc/init.d/S50systemui start
```

## 10. Current Operational Advice

For the current field-tested system:

1. Keep factory `systemui` disabled, but keep Weston running for QML/Wayland:

```sh
chmod +x /etc/init.d/S49weston
chmod -x /etc/init.d/S50systemui
/etc/init.d/S49weston start
```

`S98usbbridge` and `install_to_board.sh` now perform this automatically unless `BRIDGE_DISABLE_FACTORY_DESKTOP=0` is set. Set `BRIDGE_DISABLE_WESTON=1` only when deliberately testing non-Wayland UI backends.

2. Use QML only; no old fallback:

```conf
BRIDGE_UI_MODE=qml
BRIDGE_UI_QML_PLATFORM=wayland
BRIDGE_UI_NO_FALLBACK=1
BRIDGE_UI_START_DELAY=8
BRIDGE_DISABLE_FACTORY_DESKTOP=1
BRIDGE_DISABLE_WESTON=0
```

3. Export fixed input devices through `ui.conf`:

```conf
SCANNER_DEV=/dev/input/event8
PROJECT_INPUT_DEV=/dev/input/event5
```

4. If scanner stops after reboot, check event number again:

```sh
cat /proc/bus/input/devices
```

or use logs from evtest-like tools.

5. If API works but no HID input occurs, first check:

```sh
ls -l /root/usb_bridge/form_queue
tail -n 120 /root/usb_bridge/logs/hid_executor.log
ps | grep hid_executor | grep -v grep
```

6. If UI appears but does not change, manually write `ui_status.json` and check whether the right-bottom debug phase/time changes.

## 11. Git State

Repository initialized in:

```text
F:\4.29\c
```

Initial commit:

```text
03cf9b7 Save RK3568 USB bridge C and QML implementation
```

`.gitignore` excludes:

```text
.check/
.vscode/
*.o
hid_executor
scan_patient_capture
printer_capture
report_uploader
bridge_ui
usbdevice
usbbridge
sendkey
```

This handoff document should be committed after creation.
