# USB bridge C version

This folder contains C replacements for the Python workers:

- `hid_executor.c` -> `/root/usb_bridge/bin/hid_executor`
- `scan_patient_capture.c` -> `/root/usb_bridge/bin/scan_patient_capture`
- `printer_capture.c` -> `/root/usb_bridge/bin/printer_capture`
- `report_uploader.c` -> `/root/usb_bridge/bin/report_uploader`
- `bridge_ui.c` -> `/root/usb_bridge/bin/bridge_ui`
- `usb_bridge_ui.qml` / `bridge_ui_qml` -> Qt Quick 480x320 status UI
- `api_config.c` / `api_config.h` -> shared API configuration loader
- `usbdevice.c` -> init replacement for `S50usbdevice`
- `usbbridge.c` -> init replacement for `S98usbbridge`
- `S50usbdevice` -> init.d shell script for USB Gadget mounting/configuration
- `S98usbbridge` -> init.d shell script for workers and UI autostart
- `install_to_board.sh` -> install binaries, config example, and init.d scripts on the board

Build on the RK/Linux target:

```sh
make
```

Runtime behavior keeps the original paths under `/root/usb_bridge`, reads API settings from `/root/usb_bridge/config/api.conf` plus environment overrides, and still uses external `curl` and `gs` for HTTP and PostScript-to-PDF conversion.

API configuration example:

```sh
install -m 0644 api.conf.example /root/usb_bridge/config/api.conf
vi /root/usb_bridge/config/api.conf
```

The preferred status UI is Qt Quick:

```sh
/root/usb_bridge/bin/bridge_ui_qml
```

It reads `/root/usb_bridge/state/ui_status.json` every 500 ms and renders `/root/usb_bridge/ui/usb_bridge_ui.qml` at 480x320. The fallback C framebuffer UI is still available as `/root/usb_bridge/bin/bridge_ui`.

Example install:

```sh
install -m 0755 hid_executor scan_patient_capture printer_capture report_uploader bridge_ui /root/usb_bridge/bin/
install -m 0755 bridge_ui_qml /root/usb_bridge/bin/
install -m 0644 usb_bridge_ui.qml /root/usb_bridge/ui/
install -m 0755 S50usbdevice /etc/init.d/S50usbdevice
install -m 0755 S98usbbridge /etc/init.d/S98usbbridge
```

Or install everything on the board:

```sh
sh install_to_board.sh
```
