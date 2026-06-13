/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2025 Compat Layer Authors
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROK_COMPAT_CONFIG_H
#define LIBSIGROK_COMPAT_CONFIG_H

/**
 * @file
 *
 * Compat configuration key definitions and helpers.
 *
 * The actual SR_CONF_* compat keys are added to libsigrok.h directly.
 * This header provides additional compat definitions for config-related
 * constants and macros used by standard sigrok drivers.
 */

/* Standard sigrok config key ranges (for reference) */
#define SR_CONF_SCANOPTS_OFFSET  20000
#define SR_CONF_DRVOPTS_OFFSET   10000
#define SR_CONF_DEVOPTS_OFFSET   30000
#define SR_CONF_ACQOPTS_OFFSET   50000

/* Standard sigrok trigger match types */
enum sr_trigger_match_type {
    SR_TRIGGER_ZERO = 0,
    SR_TRIGGER_ONE = 1,
    SR_TRIGGER_RISING = 2,
    SR_TRIGGER_FALLING = 3,
    SR_TRIGGER_EDGE = 4,
    SR_TRIGGER_OVER = 5,
    SR_TRIGGER_UNDER = 6,
};

/* Standard sigrok channel change flags (for config_channel_set) */
enum sr_channel_change {
    SR_CHANNEL_SET_ENABLED = (1 << 0),
    SR_CHANNEL_SET_NAME    = (1 << 1),
    SR_CHANNEL_SET_TRIGGER = (1 << 2),
    SR_CHANNEL_SET_VDIV    = (1 << 3),
    SR_CHANNEL_SET_COUPLING = (1 << 4),
    SR_CHANNEL_SET_VFACTOR  = (1 << 5),
};

/* Standard sigrok's SR_CONF_GET/Set/LIST capability flags */
#define SR_CONF_GET     (1 << 0)
#define SR_CONF_SET     (1 << 1)
#define SR_CONF_LIST    (1 << 2)

/* Helper macro to build config key with capability flags */
#define SR_CONF_GET_SET(key)       (key | SR_CONF_GET | SR_CONF_SET)
#define SR_CONF_GET_SET_LIST(key)  (key | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST)
#define SR_CONF_GET_LIST(key)      (key | SR_CONF_GET | SR_CONF_LIST)

#endif
