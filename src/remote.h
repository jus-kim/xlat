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

#ifndef XLAT_REMOTE_H
#define XLAT_REMOTE_H

#define XLAT_REMOTE_SAMPLES __attribute__((section(".xlat_remote.samples"), aligned(32)))
#define XLAT_REMOTE_STATE __attribute__((section(".xlat_remote.state")))

void remote_ram_init(void);
void remote_task(void const *argument);

#endif
