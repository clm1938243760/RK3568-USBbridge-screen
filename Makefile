CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS_HID = -pthread

COMMON = usb_bridge_common.c
API_CONFIG = api_config.c

all: hid_executor scan_patient_capture printer_capture report_uploader bridge_ui usbdevice usbbridge sendkey

hid_executor: hid_executor.c $(COMMON) usb_bridge_common.h
	$(CC) $(CFLAGS) -o $@ hid_executor.c $(COMMON) $(LDLIBS_HID)

scan_patient_capture: scan_patient_capture.c $(COMMON) $(API_CONFIG) usb_bridge_common.h api_config.h
	$(CC) $(CFLAGS) -o $@ scan_patient_capture.c $(COMMON) $(API_CONFIG)

printer_capture: printer_capture.c $(COMMON) usb_bridge_common.h
	$(CC) $(CFLAGS) -o $@ printer_capture.c $(COMMON)

report_uploader: report_uploader.c $(COMMON) $(API_CONFIG) usb_bridge_common.h api_config.h
	$(CC) $(CFLAGS) -o $@ report_uploader.c $(COMMON) $(API_CONFIG)

bridge_ui: bridge_ui.c $(COMMON) usb_bridge_common.h
	$(CC) $(CFLAGS) -o $@ bridge_ui.c $(COMMON)

usbdevice: usbdevice.c $(COMMON) usb_bridge_common.h
	$(CC) $(CFLAGS) -o $@ usbdevice.c $(COMMON)

usbbridge: usbbridge.c $(COMMON) usb_bridge_common.h
	$(CC) $(CFLAGS) -o $@ usbbridge.c $(COMMON)

sendkey: sendkey.c
	$(CC) $(CFLAGS) -o $@ sendkey.c

clean:
	rm -f hid_executor scan_patient_capture printer_capture report_uploader bridge_ui usbdevice usbbridge sendkey

.PHONY: all clean
