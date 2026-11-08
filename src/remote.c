/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "remote.h"
#include "stdio_glue.h"
#include "gfx_main.h"
#include "hardware_config.h"
#include "usb_task.h"
#include "xlat.h"
#include "xlat_config.h"

#define REMOTE_RX_SIZE 256U
#define REMOTE_LINE_SIZE 256U
#define REMOTE_TX_SIZE 512U
#define REMOTE_DONE_DELAY_MS 250U

typedef struct remote_state {
    uint16_t rx_ndtr;
    uint16_t line_length;
    bool line_discard;
    bool run_active;
    bool done_pending;
    uint32_t done_at;
    int32_t run_requested;
    char line[REMOTE_LINE_SIZE];
    char tx[REMOTE_TX_SIZE];
} remote_state_t;

static uint8_t remote_rx[REMOTE_RX_SIZE] XLAT_REMOTE_STATE __attribute__((aligned(32)));
static remote_state_t remote XLAT_REMOTE_STATE;

extern uint8_t __xlat_remote_state_start__;
extern uint8_t __xlat_remote_state_end__;

void remote_ram_init(void)
{
    memset(&__xlat_remote_state_start__, 0,
           (size_t)(&__xlat_remote_state_end__ - &__xlat_remote_state_start__));
}

static void remote_write_buffer(size_t length)
{
    if (length >= sizeof(remote.tx)) {
        length = sizeof(remote.tx) - 1U;
    }
    remote.tx[length++] = '\n';
    vcp_write_line_remote(remote.tx, (int)length);
}

static void remote_writef(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(remote.tx, sizeof(remote.tx) - 1U, format, args);
    va_end(args);
    if (length < 0) {
        return;
    }
    if ((size_t)length >= sizeof(remote.tx) - 1U) {
        length = (int)sizeof(remote.tx) - 2;
    }
    remote_write_buffer((size_t)length);
}

static void uint64_to_string(uint64_t value, char *buffer, size_t size)
{
    char reverse[21];
    size_t length = 0;
    do {
        reverse[length++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && length < sizeof(reverse));

    size_t output = 0;
    while (length && output + 1U < size) {
        buffer[output++] = reverse[--length];
    }
    buffer[output] = '\0';
}

static bool parse_uint32(const char *text, uint32_t *value)
{
    if (!text || !*text || *text == '-') {
        return false;
    }
    char *end;
    unsigned long parsed = strtoul(text, &end, 10);
    if (*end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool remote_is_running(void)
{
    bool timer_active;
    int32_t remaining;
    int32_t requested;
    int32_t fired;
    gfx_remote_trigger_state(&timer_active, &remaining, &requested, &fired);
    return timer_active || remote.run_active;
}

static void remote_settings_line(const char *command)
{
    const char *edge = hw_config_input_trigger_is_rising_edge() ? "rising" : "falling";
    const char *bias;
    switch (hw_config_input_bias_get()) {
        case INPUT_BIAS_PULLUP: bias = "up"; break;
        case INPUT_BIAS_PULLDOWN: bias = "down"; break;
        default: bias = "none"; break;
    }
    const char *mode;
    switch (xlat_mode_get()) {
        case XLAT_MODE_MOUSE_MOTION: mode = "motion"; break;
        case XLAT_MODE_KEYBOARD: mode = "key"; break;
        default: mode = "click"; break;
    }
    remote_writef("@ok %s edge=%s bias=%s mode=%s debounce_ms=%lu interval_ms=%lu level=%s output=%u",
                  command, edge, bias, mode,
                  (unsigned long)(xlat_gpio_irq_holdoff_us_get() / 1000U),
                  (unsigned long)xlat_auto_trigger_interval_ms_get(),
                  xlat_auto_trigger_level_is_high() ? "high" : "low",
                  xlat_auto_trigger_output_get());
}

static void remote_status(void)
{
    struct xlat_stats stats;
    bool timer_active;
    int32_t remaining;
    int32_t requested;
    int32_t triggers;
    char sum[21];
    xlat_stats_snapshot(&stats);
    gfx_remote_trigger_state(&timer_active, &remaining, &requested, &triggers);
    uint64_to_string(stats.sum, sum, sizeof(sum));
    remote_writef("@ok status running=%u remaining=%ld requested=%ld triggers=%ld count=%lu last=%lu avg=%lu stdev=%lu min=%lu max=%lu sum=%s buffered=%lu capacity=%lu",
                  remote_is_running() ? 1U : 0U, (long)remaining, (long)requested, (long)triggers,
                  (unsigned long)stats.count, (unsigned long)stats.last,
                  (unsigned long)stats.average, (unsigned long)stats.standard_deviation,
                  (unsigned long)stats.minimum, (unsigned long)stats.maximum, sum,
                  (unsigned long)xlat_sample_count_get(), (unsigned long)xlat_sample_capacity_get());
}

static void sanitize_usb_string(const char *input, char *output, size_t size)
{
    size_t i = 0;
    while (input[i] && i + 1U < size) {
        unsigned char c = (unsigned char)input[i];
        output[i] = (c >= 0x20U && c <= 0x7eU && c != '"') ? (char)c : '_';
        i++;
    }
    output[i] = '\0';
}

static void remote_usb(void)
{
    uint8_t endpoint_count = usb_host_get_endpoint_count();
    for (uint8_t i = 0; i < endpoint_count; i++) {
        usb_host_endpoint_info_t endpoint;
        if (usb_host_get_endpoint(i, &endpoint)) {
            remote_writef("@ep itf=%u proto=%u addr=0x%02x mps=%u binterval=%u",
                          endpoint.interface_number, endpoint.interface_protocol,
                          endpoint.endpoint_address, endpoint.max_packet_size, endpoint.interval);
        }
    }

    char manufacturer[64];
    char product[64];
    sanitize_usb_string(usb_host_get_manuf_string(), manufacturer, sizeof(manufacturer));
    sanitize_usb_string(usb_host_get_product_string(), product, sizeof(product));
    remote_writef("@ok usb ready=%u addr=%u speed=%s vidpid=%s report_id=%u button_bits=%u motion_bits=%u keyboard=%u ep_count=%u manufacturer=\"%s\" product=\"%s\"",
                  usb_host_device_ready() ? 1U : 0U, usb_host_get_device_address(),
                  usb_host_get_speed_string(), usb_host_get_vidpid_string(),
                  xlat_report_id_get(), *xlat_button_bits_get(), *xlat_motion_bits_get(),
                  xlat_keyboard_usage_page_found_get() ? 1U : 0U, endpoint_count,
                  manufacturer, product);
}

static void remote_dump(char *start_arg, char *count_arg, char *extra)
{
    if (remote_is_running()) {
        remote_writef("@err dump busy");
        return;
    }
    uint32_t total = xlat_sample_count_get();
    uint32_t start = 0;
    uint32_t count = total;
    if (extra || (start_arg && !parse_uint32(start_arg, &start)) ||
        (count_arg && !parse_uint32(count_arg, &count)) || start > total) {
        remote_writef("@err dump bad_arg");
        return;
    }
    if (start_arg && !count_arg) {
        count = total - start;
    }
    if (count > total - start) {
        count = total - start;
    }

    uint32_t sum = 0;
    uint32_t end = start + count;
    for (uint32_t index = start; index < end;) {
        int length = snprintf(remote.tx, sizeof(remote.tx), "@d %lu", (unsigned long)index);
        for (unsigned int item = 0; item < 16U && index < end; item++, index++) {
            uint32_t value = xlat_sample_get(index);
            sum += value;
            length += snprintf(remote.tx + length, sizeof(remote.tx) - (size_t)length,
                               " %lu", (unsigned long)value);
        }
        remote_write_buffer((size_t)length);
    }
    remote_writef("@ok dump start=%lu n=%lu total=%lu sum=%lu",
                  (unsigned long)start, (unsigned long)count, (unsigned long)total, (unsigned long)sum);
}

static bool valid_debounce(uint32_t value)
{
    return value == 20U || value == 50U || value == 100U || value == 200U ||
           value == 500U || value == 1000U;
}

static void remote_set(char *key, char *value, char *extra)
{
    if (remote_is_running()) {
        remote_writef("@err set busy");
        return;
    }
    if (!key || !value || extra) {
        remote_writef("@err set bad_value");
        return;
    }

    uint32_t number;
    bool valid = true;
    gfx_remote_setting_t setting = GFX_REMOTE_EDGE;
    if (strcmp(key, "edge") == 0) {
        setting = GFX_REMOTE_EDGE;
        if (strcmp(value, "falling") == 0) number = 0;
        else if (strcmp(value, "rising") == 0) number = 1;
        else valid = false;
    } else if (strcmp(key, "bias") == 0) {
        setting = GFX_REMOTE_BIAS;
        if (strcmp(value, "none") == 0) number = INPUT_BIAS_NOPULL;
        else if (strcmp(value, "up") == 0) number = INPUT_BIAS_PULLUP;
        else if (strcmp(value, "down") == 0) number = INPUT_BIAS_PULLDOWN;
        else valid = false;
    } else if (strcmp(key, "mode") == 0) {
        setting = GFX_REMOTE_MODE;
        if (strcmp(value, "click") == 0) number = XLAT_MODE_MOUSE_CLICK;
        else if (strcmp(value, "motion") == 0) number = XLAT_MODE_MOUSE_MOTION;
        else if (strcmp(value, "key") == 0) number = XLAT_MODE_KEYBOARD;
        else valid = false;
    } else if (strcmp(key, "level") == 0) {
        setting = GFX_REMOTE_LEVEL;
        if (strcmp(value, "low") == 0) number = 0;
        else if (strcmp(value, "high") == 0) number = 1;
        else valid = false;
    } else if (strcmp(key, "debounce_ms") == 0) {
        setting = GFX_REMOTE_DEBOUNCE;
        valid = parse_uint32(value, &number) && valid_debounce(number);
    } else if (strcmp(key, "interval_ms") == 0) {
        setting = GFX_REMOTE_INTERVAL;
        valid = parse_uint32(value, &number) && number >= 100U && number <= 1000U;
    } else if (strcmp(key, "output") == 0) {
        setting = GFX_REMOTE_OUTPUT;
        valid = parse_uint32(value, &number) && (number == 6U || number == 11U);
    } else {
        valid = false;
    }

    if (!valid) {
        remote_writef("@err set bad_value");
        return;
    }
    gfx_remote_setting_apply(setting, number);
    remote_settings_line("set");
}

static void remote_command(char *line)
{
    char *save;
    char *command = strtok_r(line, " \t", &save);
    if (!command) {
        return; // blank line
    }
    char *arg1 = strtok_r(NULL, " \t", &save);
    char *arg2 = strtok_r(NULL, " \t", &save);
    char *arg3 = strtok_r(NULL, " \t", &save);

    if (strcmp(command, "ping") == 0) {
        remote_writef("@ok ping proto=1 fw=%s uptime_ms=%lu heap_free=%lu", APP_VERSION_FULL,
                      (unsigned long)HAL_GetTick(), (unsigned long)xPortGetFreeHeapSize());
    } else if (strcmp(command, "status") == 0) {
        remote_status();
    } else if (strcmp(command, "clear") == 0) {
        gfx_remote_clear();
        remote_writef("@ok clear");
    } else if (strcmp(command, "reset") == 0) {
        remote_writef("@ok reset");
        while ((USART1->ISR & USART_ISR_TC) == 0U) {}
        NVIC_SystemReset();
    } else if (strcmp(command, "start") == 0) {
        uint32_t requested = 1000;
        if (arg2 || arg3 || (arg1 && !parse_uint32(arg1, &requested)) || requested < 1U || requested > 100000U) {
            remote_writef("@err start bad_arg");
        } else if (remote_is_running()) {
            remote_writef("@err start busy");
        } else if (!usb_host_device_ready()) {
            remote_writef("@err start no_device");
        } else if (!gfx_remote_trigger_start(requested)) {
            remote_writef("@err start busy");
        } else {
            remote_writef("@ok start requested=%lu", (unsigned long)requested);
        }
    } else if (strcmp(command, "stop") == 0) {
        bool was_running = remote_is_running();
        gfx_remote_trigger_stop();
        remote_writef("@ok stop was_running=%u", was_running ? 1U : 0U);
    } else if (strcmp(command, "dump") == 0) {
        remote_dump(arg1, arg2, arg3);
    } else if (strcmp(command, "get") == 0) {
        remote_settings_line("get");
    } else if (strcmp(command, "set") == 0) {
        remote_set(arg1, arg2, arg3);
    } else if (strcmp(command, "usb") == 0) {
        remote_usb();
    } else {
        remote_writef("@err %s unknown", command);
    }
}

static void remote_rx_byte(uint8_t byte)
{
    if (byte == '\r') {
        return;
    }
    if (byte == '\n') {
        if (remote.line_discard) {
            remote_writef("@err line too_long");
        } else {
            remote.line[remote.line_length] = '\0';
            remote_command(remote.line);
        }
        remote.line_length = 0;
        remote.line_discard = false;
        return;
    }
    if (remote.line_discard) {
        return;
    }
    if (remote.line_length + 1U >= sizeof(remote.line)) {
        remote.line_discard = true;
        return;
    }
    remote.line[remote.line_length++] = (char)byte;
}

static void remote_rx_poll(void)
{
    uint32_t errors = USART1->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE);
    if (errors) {
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF;
    }

    uint16_t ndtr = (uint16_t)DMA2_Stream2->NDTR;
    if (ndtr == remote.rx_ndtr) {
        return;
    }
    SCB_InvalidateDCache_by_Addr((uint32_t *)remote_rx, sizeof(remote_rx));
    __DSB();

    uint16_t old_position = (uint16_t)(REMOTE_RX_SIZE - remote.rx_ndtr);
    uint16_t new_position = (uint16_t)((REMOTE_RX_SIZE - ndtr) % REMOTE_RX_SIZE);
    while (old_position != new_position) {
        remote_rx_byte(remote_rx[old_position]);
        old_position++;
        if (old_position == REMOTE_RX_SIZE) {
            old_position = 0;
        }
    }
    remote.rx_ndtr = ndtr;
}

static void remote_dma_init(void)
{
    __HAL_RCC_DMA2_CLK_ENABLE();
    DMA2_Stream2->CR &= ~DMA_SxCR_EN;
    while (DMA2_Stream2->CR & DMA_SxCR_EN) {}
    DMA2->LIFCR = DMA_LIFCR_CFEIF2 | DMA_LIFCR_CDMEIF2 | DMA_LIFCR_CTEIF2 |
                  DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTCIF2;
    DMA2_Stream2->PAR = (uint32_t)&USART1->RDR;
    DMA2_Stream2->M0AR = (uint32_t)remote_rx;
    DMA2_Stream2->NDTR = REMOTE_RX_SIZE;
    DMA2_Stream2->FCR = 0;
    DMA2_Stream2->CR = DMA_SxCR_CHSEL_2 | DMA_SxCR_MINC | DMA_SxCR_CIRC;
    remote.rx_ndtr = REMOTE_RX_SIZE;
    USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF;
    USART1->CR3 |= USART_CR3_DMAR;
    DMA2_Stream2->CR |= DMA_SxCR_EN;
}

static bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void remote_run_check(void)
{
    bool timer_active;
    int32_t remaining;
    int32_t requested;
    int32_t triggers;
    gfx_remote_trigger_state(&timer_active, &remaining, &requested, &triggers);

    if (timer_active) {
        if (!remote.run_active || remote.done_pending) {
            remote.run_active = true;
            remote.done_pending = false;
            remote.run_requested = requested;
        }
        return;
    }
    if (!remote.run_active) {
        return;
    }
    uint32_t now = HAL_GetTick();
    if (!remote.done_pending) {
        remote.done_pending = true;
        remote.done_at = now + REMOTE_DONE_DELAY_MS;
        return;
    }
    if (time_reached(now, remote.done_at)) {
        struct xlat_stats stats;
        xlat_stats_snapshot(&stats);
        remote_writef("@event done requested=%ld triggers=%ld count=%lu",
                      (long)remote.run_requested, (long)triggers, (unsigned long)stats.count);
        remote.run_active = false;
        remote.done_pending = false;
    }
}

void remote_task(void const *argument)
{
    (void)argument;
    remote_dma_init();
    remote_writef("@event boot fw=%s proto=1", APP_VERSION_FULL);

    while (1) {
        remote_rx_poll();
        remote_run_check();
        osDelay(10);
    }
}
