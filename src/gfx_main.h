/*
 * Copyright (c) 2023 Finalmouse, LLC
 *
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

#ifndef XLAT_F7_FW_GFX_H
#define XLAT_F7_FW_GFX_H

#include <stdbool.h>
#include <stdint.h>

#define GFX_BTN_HEIGHT    (25)
#define GFX_BTN_WIDTH     (80)


typedef enum gfx_event_type {
    GFX_EVENT_MEASUREMENT,
    GFX_EVENT_DEVICE_CONNECTED,
    GFX_EVENT_DEVICE_DISCONNECTED,
    GFX_EVENT_MODE_CHANGED,
} gfx_event_t;

struct gfx_event {
    gfx_event_t type;
    int32_t value;
};

typedef enum gfx_remote_setting {
    GFX_REMOTE_EDGE,
    GFX_REMOTE_BIAS,
    GFX_REMOTE_MODE,
    GFX_REMOTE_DEBOUNCE,
    GFX_REMOTE_INTERVAL,
    GFX_REMOTE_LEVEL,
    GFX_REMOTE_OUTPUT,
} gfx_remote_setting_t;

void gfx_init(void);
void gfx_task(void const * argument);
void gfx_device_label_set(const char * manufacturer, const char * productname, const char *vidpid);
void gfx_trigger_ready_set(bool state);
void gfx_data_locations_label_set(void);
void gfx_mode_label_set(void);
void gfx_labels_update(void);
void gfx_event_send(gfx_event_t type, int32_t value);
void gfx_xlat_gui(void);
bool gfx_remote_trigger_start(uint32_t n);
void gfx_remote_trigger_stop(void);
void gfx_remote_clear(void);
void gfx_remote_setting_apply(gfx_remote_setting_t setting, uint32_t value);
void gfx_remote_trigger_state(bool *timer_active, int32_t *remaining, int32_t *requested_out, int32_t *fired);

#endif //XLAT_F7_FW_GFX_H
