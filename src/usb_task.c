/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "stm32f7xx_hal.h"
#include "usb_task.h"

#include <cmsis_os.h>
#include <xlat.h>

#include "tusb.h"

// XLAT additions to the dwc2 hcd (libs/tinyusb, branch xlat-fixes)
void hcd_port_status(uint8_t rhport, bool* connected, bool* enabled);
void hcd_port_power_set(uint8_t rhport, bool on);
#include "tusb_config.h"

#include "gfx_main.h"
#include "remote.h"

extern void hid_app_init(void);

static void board_init(void) {
  // Explicitly disable systick to prevent its ISR runs before scheduler start
  // SysTick->CTRL &= ~1U;

  // If freeRTOS is used, IRQ priority is limited by max syscall ( smaller value is higher priority )
  // NVIC_SetPriority(OTG_FS_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY );
  NVIC_SetPriority(OTG_HS_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY );

  GPIO_InitTypeDef GPIO_InitStruct;

  //------------- rhport1: OTG_HS -------------//
  // MCU with external ULPI PHY
  /* ULPI CLK */
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_HS;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* ULPI D0 */
  GPIO_InitStruct.Pin = GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_HS;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* ULPI D1 D2 D3 D4 D5 D6 D7 */
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_HS;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* ULPI STP */
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_HS;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* NXT */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_HS;
  HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);

  /* ULPI DIR */
  GPIO_InitStruct.Pin = GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Alternate = GPIO_AF10_OTG_HS;
  HAL_GPIO_Init(GPIOI, &GPIO_InitStruct);

  // Enable USB HS & ULPI Clocks
  __HAL_RCC_USB_OTG_HS_ULPI_CLK_ENABLE();
  __HAL_RCC_USB_OTG_HS_CLK_ENABLE();

#if OTG_HS_VBUS_SENSE
  #error OTG HS VBUS Sense enabled is not implemented
#else
  // No VBUS sense
  USB_OTG_HS->GCCFG &= ~USB_OTG_GCCFG_VBDEN;

  // B-peripheral session valid override enable
  USB_OTG_HS->GOTGCTL |= USB_OTG_GOTGCTL_BVALOEN;
  USB_OTG_HS->GOTGCTL |= USB_OTG_GOTGCTL_BVALOVAL;
#endif
}

// Hard reset of the USB host peripheral and ULPI link. Used when the ULPI PHY (USB3320) stops
// responding: a core soft reset alone does not recover it, an MCU reset does. Mimic that:
// drive ULPI STP high (wakes the PHY from low-power / resyncs the link), hold the OTG_HS
// peripheral in RCC reset with its clocks off, then bring everything back up.
static void usb_phy_hard_reset(void) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  // ULPI STP (PC0) as GPIO, driven high
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_SET);
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  __HAL_RCC_USB_OTG_HS_FORCE_RESET();
  __HAL_RCC_USB_OTG_HS_ULPI_CLK_DISABLE();
  __HAL_RCC_USB_OTG_HS_CLK_DISABLE();
  osDelay(50);
  __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
  __HAL_RCC_USB_OTG_HS_ULPI_CLK_ENABLE();
  __HAL_RCC_USB_OTG_HS_RELEASE_RESET();
  osDelay(10);

  // STP back to the ULPI function, re-run the full pin/clock init
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);
  board_init();
}

// USB Host task
// This top level thread process all usb events and invoke callbacks
void usb_host_task(void const *param) {
  (void) param;

  board_init();

  // init host stack on configured roothub port
  tusb_rhport_init_t host_init = {
    .role = TUSB_ROLE_HOST,
    .speed = TUSB_SPEED_AUTO
  };

  while(!xlat_initialized) {
    osDelay(1);
  }

  // Match desktop OS hosts: leave HID devices in report protocol (their power-on default per
  // HID spec 7.2.6) instead of TinyUSB's default of switching boot-capable interfaces to boot
  // protocol. Report mode is what mice are designed and optimized for.
  tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);

  if (!tusb_init(BOARD_TUH_RHPORT, &host_init)) {
    printf("Failed to init USB Host Stack\n");
    vTaskSuspend(NULL);
  }

  // Force a clean port re-detect: if a device was already attached at MCU
  // reset, the CONN_DETECT edge happens before the host stack is ready and
  // enumeration can stall after SET_ADDRESS (no mount callback fires).
  // Toggle HPRT_PPWR off/on so the device pull-up is re-seen as a fresh attach.
  {
    const uint32_t w1c = USB_OTG_HPRT_PENA | USB_OTG_HPRT_PCDET |
                         USB_OTG_HPRT_PENCHNG | USB_OTG_HPRT_POCCHNG;
    volatile uint32_t *hprt = (volatile uint32_t *)
        ((uint32_t)USB_OTG_HS + USB_OTG_HOST_PORT_BASE);
    *hprt = (*hprt & ~w1c) & ~USB_OTG_HPRT_PPWR; // power off port
    osDelay(50);
    *hprt = (*hprt & ~w1c) | USB_OTG_HPRT_PPWR;  // power on -> re-attach event
  }

#if CFG_TUH_ENABLED && CFG_TUH_MAX3421
  // FeatherWing MAX3421E use MAX3421E's GPIO0 for VBUS enable
  enum { IOPINS1_ADDR  = 20u << 3, /* 0xA0 */ };
  tuh_max3421_reg_write(BOARD_TUH_RHPORT, IOPINS1_ADDR, 0x01, false);
#endif

  // RTOS forever loop
  uint32_t port_dead_ms = 0;     // device attached but port disabled
  uint32_t no_device_ms = 0;     // no device seen after a recovery attempt
  uint8_t restart_attempts = 0;
  while (1) {
    // process USB events, wake up periodically for the port watchdog
    tuh_task_ext(100, false);

    // USB watchdog. Observed failure with a flaky dongle: the ULPI PHY / core wedges, the port
    // gets disabled with the device still attached and nothing recovers on its own. Escalate:
    //  1. soft restart: host stack deinit/reinit (core soft reset, VBUS cycle)
    //  2. hard restart: RCC peripheral reset + ULPI link reset, then reinit
    //  3. MCU reset
    bool connected, enabled;
    hcd_port_status(BOARD_TUH_RHPORT, &connected, &enabled);
    bool restart = false;
    if (connected && !enabled) {
      port_dead_ms += 100;
      restart = (port_dead_ms >= 2000);
    } else if (!connected && restart_attempts > 0) {
      // device did not come back after a restart: PHY probably still dead
      no_device_ms += 100;
      restart = (no_device_ms >= 3000);
    } else {
      port_dead_ms = 0;
      no_device_ms = 0;
      if (connected && enabled) {
        restart_attempts = 0; // healthy
      }
    }

    if (restart) {
      port_dead_ms = 0;
      no_device_ms = 0;
      restart_attempts++;
      printf("USB dead (connected=%d enabled=%d), recovery attempt %u\n", connected, enabled, restart_attempts);
      if (restart_attempts >= 3) {
        printf("USB recovery failed, resetting XLAT\n");
        osDelay(100);
        NVIC_SystemReset();
      }
      tuh_deinit(BOARD_TUH_RHPORT);
      if (restart_attempts >= 2) {
        usb_phy_hard_reset();
      }
      osDelay(500); // VBUS off long enough for the dongle to restart
      if (!tuh_rhport_init(BOARD_TUH_RHPORT, &host_init)) {
        printf("USB host re-init failed\n");
      }
    }
  }
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Buffer for string descriptors
static char manufacturer_string[64];
static char product_string[64];
static char serial_string[64];
static char vidpid_string[32];  // Format: "VID:XXXX PID:XXXX"

static usb_host_endpoint_info_t remote_endpoints[USB_HOST_REMOTE_ENDPOINTS_MAX] XLAT_REMOTE_STATE;
static uint8_t remote_endpoint_count XLAT_REMOTE_STATE;
static uint8_t remote_device_address XLAT_REMOTE_STATE;
static bool remote_device_ready XLAT_REMOTE_STATE;

// TinyUSB's *_sync control calls busy-wait with no timeout, and tuh_mount_cb runs them at this
// task's high priority. If one never completes, every lower task starves (screen and serial too)
// until someone resets the board. Only while mounting, a hang past this limit does that same reset.
#define USB_MOUNT_WATCHDOG_MS 3000U
static volatile uint32_t mount_deadline_tick XLAT_REMOTE_STATE; // 0 = not armed

// Called from the 1 kHz tick interrupt
void usb_host_mount_watchdog_tick(void)
{
    uint32_t deadline = mount_deadline_tick;
    if (deadline != 0U && (int32_t)(HAL_GetTick() - deadline) >= 0) {
        NVIC_SystemReset();
    }
}

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index,
                                          const tusb_desc_configuration_t *desc_config)
{
    (void)daddr;
    (void)cfg_index;
    remote_endpoint_count = 0;

    uint16_t total = tu_le16toh(desc_config->wTotalLength);
    const uint8_t *descriptor = (const uint8_t *)desc_config;
    uint16_t offset = 0;
    bool hid_interface = false;
    uint8_t interface_number = 0;
    uint8_t interface_protocol = 0;

    while (offset + 2U <= total) {
        uint8_t length = descriptor[offset];
        uint8_t type = descriptor[offset + 1U];
        if (length == 0 || (uint32_t)offset + length > total) {
            break;
        }

        if (type == TUSB_DESC_INTERFACE && length >= sizeof(tusb_desc_interface_t)) {
            const tusb_desc_interface_t *interface = (const tusb_desc_interface_t *)(descriptor + offset);
            hid_interface = interface->bInterfaceClass == TUSB_CLASS_HID;
            interface_number = interface->bInterfaceNumber;
            interface_protocol = interface->bInterfaceProtocol;
        } else if (type == TUSB_DESC_ENDPOINT && hid_interface &&
                   length >= sizeof(tusb_desc_endpoint_t) && remote_endpoint_count < USB_HOST_REMOTE_ENDPOINTS_MAX) {
            const tusb_desc_endpoint_t *endpoint = (const tusb_desc_endpoint_t *)(descriptor + offset);
            if ((endpoint->bEndpointAddress & TUSB_DIR_IN_MASK) &&
                endpoint->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                usb_host_endpoint_info_t *capture = &remote_endpoints[remote_endpoint_count++];
                capture->interface_number = interface_number;
                capture->interface_protocol = interface_protocol;
                capture->endpoint_address = endpoint->bEndpointAddress;
                capture->max_packet_size = tu_edpt_packet_size(endpoint);
                capture->interval = endpoint->bInterval;
            }
        }
        offset = (uint16_t)(offset + length);
    }
    return true;
}

// Helper function to convert UTF-16LE to UTF-8
static void utf16le_to_utf8(const uint16_t* utf16, size_t utf16_len, char* utf8, size_t utf8_len) {
    size_t utf8_pos = 0;
    for (size_t i = 0; i < utf16_len && utf8_pos < utf8_len - 1; i++) {
        uint16_t chr = utf16[i];
        if (chr == 0) break;
        
        if (chr < 0x80) {
            utf8[utf8_pos++] = (char)chr;
        } else if (chr < 0x800) {
            if (utf8_pos + 1 >= utf8_len - 1) break;
            utf8[utf8_pos++] = (char)(0xC0 | (chr >> 6));
            utf8[utf8_pos++] = (char)(0x80 | (chr & 0x3F));
        } else {
            if (utf8_pos + 2 >= utf8_len - 1) break;
            utf8[utf8_pos++] = (char)(0xE0 | (chr >> 12));
            utf8[utf8_pos++] = (char)(0x80 | ((chr >> 6) & 0x3F));
            utf8[utf8_pos++] = (char)(0x80 | (chr & 0x3F));
        }
    }
    utf8[utf8_pos] = '\0';
}

// Get and buffer string descriptors
static void update_string_descriptors(uint8_t daddr, tusb_desc_device_t *desc_device) {
    uint16_t temp_buf[64];
    
    // Get manufacturer string
    if (tuh_descriptor_get_manufacturer_string_sync(daddr, 0x0409, temp_buf, sizeof(temp_buf)) == XFER_RESULT_SUCCESS) {
        // First byte is length, second byte is descriptor type (3)
        uint8_t str_len = ((uint8_t*)temp_buf)[0] - 2; // Subtract header size (2 bytes)
        utf16le_to_utf8((uint16_t*)&temp_buf[1], str_len/2, manufacturer_string, sizeof(manufacturer_string));
    } else {
        manufacturer_string[0] = '\0';
    }
    
    // Get product string
    if (tuh_descriptor_get_product_string_sync(daddr, 0x0409, temp_buf, sizeof(temp_buf)) == XFER_RESULT_SUCCESS) {
        uint8_t str_len = ((uint8_t*)temp_buf)[0] - 2;
        utf16le_to_utf8((uint16_t*)&temp_buf[1], str_len/2, product_string, sizeof(product_string));
    } else {
        product_string[0] = '\0';
    }
    
    // Get serial string
    if (tuh_descriptor_get_serial_string_sync(daddr, 0x0409, temp_buf, sizeof(temp_buf)) == XFER_RESULT_SUCCESS) {
        uint8_t str_len = ((uint8_t*)temp_buf)[0] - 2;
        utf16le_to_utf8((uint16_t*)&temp_buf[1], str_len/2, serial_string, sizeof(serial_string));
    } else {
        serial_string[0] = '\0';
    }

    // Update VID/PID string
    snprintf(vidpid_string, sizeof(vidpid_string), "%04x:%04x",
            desc_device->idVendor, desc_device->idProduct);

}

// Update the mount callback to get string descriptors
void tuh_mount_cb(uint8_t daddr) {
    mount_deadline_tick = (HAL_GetTick() + USB_MOUNT_WATCHDOG_MS) | 1U;

    // Get device descriptor first
    tusb_desc_device_t desc_device;

    printf("[%5lu] Device with address %d mounted\n", (unsigned long) xTaskGetTickCount(), daddr);

    if (tuh_descriptor_get_device_sync(daddr, &desc_device, sizeof(desc_device)) == XFER_RESULT_SUCCESS) {
        // Update string descriptors
        update_string_descriptors(daddr, &desc_device);
        
        printf("\t-> %04x:%04x %s: %s\n",
               desc_device.idVendor, desc_device.idProduct,
               manufacturer_string, product_string);


        // Send a message to the gfx thread, to refresh the device info
        gfx_event_send(GFX_EVENT_DEVICE_CONNECTED, 0);
    }
    remote_device_address = daddr;
    remote_device_ready = true;
    mount_deadline_tick = 0;
}

// XXX FIXME: doesn't seem to be called
void tuh_umount_cb(uint8_t dev_addr) {
  // application tear-down
  printf("[%5lu] A device with address %d is unmounted\n", (unsigned long) xTaskGetTickCount(), dev_addr);
  xlat_clear_device_info();
  remote_device_ready = false;
}


// Public functions to get buffered strings
const char* usb_host_get_manuf_string(void) {
    return manufacturer_string;
}

const char* usb_host_get_product_string(void) {
    return product_string;
}

const char* usb_host_get_serial_string(void) {
    return serial_string;
}

const char * usb_host_get_vidpid_string(void)
{
    return vidpid_string;
}

bool usb_host_device_ready(void)
{
    return remote_device_ready;
}

void usb_host_device_ready_clear(void)
{
    remote_device_ready = false;
}

uint8_t usb_host_get_device_address(void)
{
    return remote_device_address;
}

const char *usb_host_get_speed_string(void)
{
    if (!remote_device_ready) {
        return "none";
    }
    switch (tuh_speed_get(remote_device_address)) {
        case TUSB_SPEED_LOW: return "low";
        case TUSB_SPEED_FULL: return "full";
        case TUSB_SPEED_HIGH: return "high";
        default: return "none";
    }
}

uint8_t usb_host_get_endpoint_count(void)
{
    return remote_endpoint_count;
}

bool usb_host_get_endpoint(uint8_t index, usb_host_endpoint_info_t *endpoint)
{
    if (index >= remote_endpoint_count) {
        return false;
    }
    *endpoint = remote_endpoints[index];
    return true;
}
