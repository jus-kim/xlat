#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_HOST_REMOTE_ENDPOINTS_MAX 6

typedef struct usb_host_endpoint_info {
    uint8_t interface_number;
    uint8_t interface_protocol;
    uint8_t endpoint_address;
    uint16_t max_packet_size;
    uint8_t interval;
} usb_host_endpoint_info_t;

void usb_host_task(void const *param);
const char* usb_host_get_manuf_string(void);
const char* usb_host_get_product_string(void);
const char* usb_host_get_serial_string(void);
const char* usb_host_get_vidpid_string(void);
void usb_host_mount_watchdog_tick(void);
bool usb_host_device_ready(void);
void usb_host_device_ready_clear(void);
uint8_t usb_host_get_device_address(void);
const char *usb_host_get_speed_string(void);
uint8_t usb_host_get_endpoint_count(void);
bool usb_host_get_endpoint(uint8_t index, usb_host_endpoint_info_t *endpoint);

