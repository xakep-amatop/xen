/*
 *  xen/include/xen/thermal.h
 *
 *  Copyright (C) 2022        Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __XEN_THERMAL_H__
#define __XEN_THERMAL_H__

#include <xen/list.h>
#include <xen/spinlock.h>
#include <xen/timer.h>
#include <xen/types.h>

#define THERMAL_NAME_LEN    16
#define THERMAL_TRIPS_NONE  -1
#define SENSORID_NONE    (~0U)

enum thermal_trip_type {
    THERMAL_TRIP_ACTIVE = 0,
    THERMAL_TRIP_PASSIVE,
    THERMAL_TRIP_HOT,
    THERMAL_TRIP_CRITICAL,
};

enum thermal_trend {
    THERMAL_TREND_STABLE, /* temperature is stable */
    THERMAL_TREND_RAISING, /* temperature is raising */
    THERMAL_TREND_DROPPING, /* temperature is dropping */
    THERMAL_TREND_RAISE_FULL, /* apply highest cooling action */
    THERMAL_TREND_DROP_FULL, /* apply lowest cooling action */
};

struct thermal_sensor;

struct thermal_sensor_ops {
    int (*get_temp) (void *, int *);
    int (*set_trips) (struct thermal_sensor *, int, int);
    int (*change_mode) (struct thermal_sensor *,
        int thermal_device_mode);
    int (*get_trip_type) (void *, int,
        enum thermal_trip_type *);
    int (*get_trip_temp) (void *, int, int *);
    int (*set_trip_temp) (struct thermal_sensor *, int, int);
    int (*get_trip_hyst) (void *, int, int *);
    int (*set_trip_hyst) (struct thermal_sensor *, int, int);
    int (*get_crit_temp) (struct thermal_sensor *, int *);
    int (*set_emul_temp) (struct thermal_sensor *, int);
    int (*get_trend) (struct thermal_sensor *, int,
               enum thermal_trend *);
    int (*notify) (struct thermal_sensor *, int,
               enum thermal_trip_type);
    int (*throttle)(int, void *);
};

struct thermal_sensor {
    int sensor_id;
    struct thermal_sensor_ops *ops;
    void *data;
    int trips;
    spinlock_t lock;
    bool_t throttle;
    int active_trip;
    struct timer timer;
    struct list_head sensor_list;
};

/*********************************************************************
 *                          THERMAL GOVERNORS                        *
 *********************************************************************/
#define THERMAL_GOV_START  1
#define THERMAL_GOV_STOP   2
#define THERMAL_GOV_LIMITS 3

struct thermal_governor {
    char    name[THERMAL_NAME_LEN];
    int     (*throttle)(int sensor_id, struct thermal_sensor *sensor,
                        int trip);
    bool_t  (*handle_option)(const char *name, const char *value);
    struct list_head governor_list;
};

extern struct thermal_governor *thermal_opt_governor;
extern struct thermal_governor thermal_gov_step_wise;
extern struct thermal_governor thermal_gov_fair_share;
extern struct thermal_governor thermal_gov_user_space;
extern struct thermal_governor thermal_gov_static_level;

extern struct list_head thermal_governor_list;

extern int thermal_register_governor(struct thermal_governor *governor);
extern struct thermal_governor *__find_thermal_governor(const char *governor);

#define THERMAL_DEFAULT_GOVERNOR &thermal_gov_step_wise

/*********************************************************************
 *                          COMMON FUNCTIONALITY                     *
 *********************************************************************/

int thermal_notify(int sensor_id, void *data, int trip);
void activate_throttle(struct thermal_sensor *sensor, unsigned int delay,
        int active_trip);
void deactivate_throttle(struct thermal_sensor *sensor);
bool get_throttle_flag(int temp, int trip_temp,
        enum thermal_trend trend);
int get_target_relation(enum thermal_trend trend);
int register_thermal_sensor(int sensor_id,
        struct thermal_sensor_ops *sensor_ops, void *data, int trips);
#endif /* __XEN_THERMAL_H__ */
