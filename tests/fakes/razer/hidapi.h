#pragma once
// Flat header layout matches HIDAPI's exported include directory on all platforms.
// No real HID library is linked into VIA protocol tests.
#include <cstddef>
#include <cwchar>
struct hid_device_;
using hid_device = hid_device_;
struct hid_device_info {
    char *path;
    unsigned short vendor_id, product_id;
    wchar_t *serial_number;
    wchar_t *product_string;
    unsigned short usage_page, usage;
    int interface_number;
    hid_device_info *next;
};
extern "C" {
int hid_send_feature_report(hid_device *, const unsigned char *, std::size_t);
int hid_get_feature_report(hid_device *, unsigned char *, std::size_t);
int hid_init();
hid_device_info *hid_enumerate(unsigned short, unsigned short);
void hid_free_enumeration(hid_device_info *);
hid_device *hid_open_path(const char *);
int hid_write(hid_device *, const unsigned char *, std::size_t);
int hid_read_timeout(hid_device *, unsigned char *, std::size_t, int);
void hid_close(hid_device *);
}
