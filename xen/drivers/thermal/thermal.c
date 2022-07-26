/*
 *  Copyright (C) 2022 Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include "asm-arm/cache.h"
#include <xen/cpufreq.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/err.h>
#include <xen/hypfs.h>
#include <xen/guest_access.h>
#include <xen/list.h>
#include <xen/param.h>
#include <xen/string.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <xen/thermal.h>
#include <xen/types.h>
#include <xen/time.h>
#include <xen/xmalloc.h>

#include <public/platform.h>

struct thermal_governor *__read_mostly thermal_opt_governor;
LIST_HEAD_READ_MOSTLY(thermal_governor_list);

LIST_HEAD_READ_MOSTLY(thermal_sensor_list);

/* set xen as default thermal */
enum thermal_controller thermal_controller = THERMCTL_xen;

static int __init thermal_cmdline_parse(const char *s);

static int __init setup_thermal_option(const char *str)
{
    const char *arg = strpbrk(str, ",:");
    int choice;

    if ( !arg )
        arg = strchr(str, '\0');
    choice = parse_bool(str, arg);

    if ( choice < 0 && !cmdline_strcmp(str, "dom0-kernel") )
    {
        xen_processor_pmbits &= ~XEN_PROCESSOR_PM_PX;
        thermal_controller = THERMCTL_dom0_kernel;
        opt_dom0_vcpus_pin = 1;
        return 0;
    }

    if ( choice == 0 || !cmdline_strcmp(str, "none") )
    {
        xen_processor_pmbits &= ~XEN_PROCESSOR_PM_PX;
        thermal_controller = THERMCTL_none;
        return 0;
    }

    if ( choice > 0 || !cmdline_strcmp(str, "xen") )
    {
        xen_processor_pmbits |= XEN_PROCESSOR_PM_PX;
        thermal_controller = THERMCTL_xen;
        if ( *arg && *(arg + 1) )
            return thermal_cmdline_parse(arg + 1);
    }

    return (choice < 0) ? -EINVAL : 0;
}
custom_param("thermal", setup_thermal_option);

struct thermal_governor *__find_thermal_governor(const char *governor)
{
    struct thermal_governor *t;

    if ( !governor )
        return NULL;

    list_for_each_entry(t, &thermal_governor_list, governor_list)
        if ( !strncasecmp(governor, t->name, THERMAL_NAME_LEN) )
            return t;

    return NULL;
}

int __init thermal_register_governor(struct thermal_governor *governor)
{
    if ( !governor )
        return -EINVAL;

    if ( __find_thermal_governor(governor->name) != NULL )
        return -EEXIST;

    list_add(&governor->governor_list, &thermal_governor_list);
    return 0;
}

bool_t __read_mostly thermal_verbose;

static int __init thermal_handle_common_option(const char *name, const char *val)
{
    if ( !strcmp(name, "verbose") )
    {
        thermal_verbose = !val || !!simple_strtoul(val, NULL, 0);
        return 1;
    }

    return 0;
}

static void thermal_hypfs_init(void);

static int __init thermal_cmdline_parse(const char *s)
{
    static struct thermal_governor *__initdata thermal_governors[] =
    {
        THERMAL_DEFAULT_GOVERNOR,
        &thermal_gov_step_wise,
        &thermal_gov_fair_share,
        &thermal_gov_user_space,
        &thermal_gov_static_level,
    };
    static char __initdata buf[128];
    char *str = buf;
    unsigned int gov_index = 0;
    int rc = 0;

    strlcpy(buf, s, sizeof(buf));
    do
    {
        char *val, *end = strchr(str, ',');
        unsigned int i;

        if ( end )
            *end++ = '\0';
        val = strchr(str, '=');
        if ( val )
            *val++ = '\0';

        if ( !thermal_opt_governor )
        {
            if ( !val )
            {
                for (i = 0; i < ARRAY_SIZE(thermal_governors); ++i)
                {
                    if ( !strcmp(str, thermal_governors[i]->name) )
                    {
                        thermal_opt_governor = thermal_governors[i];
                        gov_index = i;
                        str = NULL;
                        break;
                    }
                }
            }
            else
                thermal_opt_governor = THERMAL_DEFAULT_GOVERNOR;
        }

        if ( str && !thermal_handle_common_option(str, val) &&
            (!thermal_governors[gov_index]->handle_option ||
             !thermal_governors[gov_index]->handle_option(str, val)) )
        {
            printk(XENLOG_WARNING "thermal/%s: option '%s' not recognized\n",
                   thermal_governors[gov_index]->name, str);
            rc = -EINVAL;
        }

        str = end;
    }
    while (str);

    thermal_hypfs_init();

    return rc;
}

static struct thermal_sensor *get_sensor_by_id(int sensor_id)
{
    struct thermal_sensor *sens;

    list_for_each_entry(sens, &thermal_sensor_list, sensor_list)
        if ( sens->sensor_id == sensor_id )
            return sens;

    return NULL;
}


int thermal_notify(int sensor_id, void *data, int trip)
{
    struct thermal_sensor *sensor;
    int ret = 0;

    if ( unlikely(!thermal_opt_governor) )
        return -ENODEV;

    sensor = get_sensor_by_id(sensor_id);
    if ( !sensor )
        return -EINVAL;

    thermal_opt_governor->throttle(sensor_id, sensor, trip);

    if ( sensor->ops->throttle )
        ret = sensor->ops->throttle(sensor_id, sensor->data);

    return ret;
}

static void sensor_timer_work(void *data)
{
    struct thermal_sensor *sensor = data;
    int trip;

    if ( !sensor->throttle )
        return;

    trip = ( sensor->throttle ) ? sensor->active_trip : THERMAL_TRIPS_NONE;
    thermal_notify(sensor->sensor_id, data, trip);
}

void activate_throttle(struct thermal_sensor *sensor, unsigned int delay,
        int active_trip)
{
    spin_lock(&sensor->lock);

    sensor->throttle = true;
    sensor->active_trip = active_trip;
    set_timer(&sensor->timer, NOW() + MILLISECS(delay));

    spin_unlock(&sensor->lock);
}

void deactivate_throttle(struct thermal_sensor *sensor)
{
    spin_lock(&sensor->lock);
    sensor->throttle = false;
    stop_timer(&sensor->timer);
    spin_unlock(&sensor->lock);
}

bool get_throttle_flag(int temp, int trip_temp,
        enum thermal_trend trend)
{
    switch ( trend )
    {
        case THERMAL_TREND_RAISING:
        case THERMAL_TREND_RAISE_FULL:
        case THERMAL_TREND_STABLE:
            return true;
        case THERMAL_TREND_DROPPING:
        case THERMAL_TREND_DROP_FULL:
            return temp > trip_temp;
        default:
            return false;
    }
}

int get_target_relation(enum thermal_trend trend)
{
    if ( trend == THERMAL_TREND_RAISING || trend == THERMAL_TREND_RAISE_FULL )
        return CPUFREQ_RELATION_H;

    return CPUFREQ_RELATION_L;
}

int __init register_thermal_sensor(int sensor_id,
        struct thermal_sensor_ops *sensor_ops, void *data, int trips)
{
    struct thermal_sensor *sensor;
    if ( unlikely(get_sensor_by_id(sensor_id)) )
        return -EEXIST;

    if ( (!sensor_ops) || (!sensor_ops->get_temp) || (!sensor_ops->throttle) )
        return -EINVAL;

    sensor = xzalloc(struct thermal_sensor);
    if ( !sensor )
        return -ENOMEM;

    sensor->sensor_id = sensor_id;
    sensor->ops = sensor_ops;
    sensor->data = data;
    sensor->throttle = false;
    sensor->trips = trips;
    spin_lock_init(&sensor->lock);
    init_timer(&sensor->timer, sensor_timer_work, (void *)sensor,
            sensor->sensor_id);
    list_add(&sensor->sensor_list, &thermal_sensor_list);
    return 0;
}

#ifdef CONFIG_HYPFS
static DEFINE_SPINLOCK(sensors_lock);
static HYPFS_DIR_INIT(sensors_listdir, "%u");

static int sensor_dir_read(const struct hypfs_entry *entry,
                            XEN_GUEST_HANDLE_PARAM(void) uaddr)
{
    int ret = 0;
    struct thermal_sensor *sens;
    struct hypfs_dyndir_id *data;

    data = hypfs_get_dyndata();

    list_for_each_entry(sens, &thermal_sensor_list, sensor_list)
    {
        data->id = sens->sensor_id;
        data->data = sens;

        ret = hypfs_read_dyndir_id_entry(&sensors_listdir, sens->sensor_id,
                    list_is_last(&sens->sensor_list, &thermal_sensor_list),
                                         &uaddr);
        if ( ret )
            break;
    }

    return ret;
}

static unsigned int sensor_dir_getsize(const struct hypfs_entry *entry)
{
    const struct thermal_sensor *c;
    unsigned int size = 0;

    list_for_each_entry(c, &thermal_sensor_list, sensor_list)
        size += hypfs_dynid_entry_size(entry, c->sensor_id);

    return size;
}

static const struct hypfs_entry *sensor_dir_enter(
    const struct hypfs_entry *entry)
{
    struct hypfs_dyndir_id *data;

    data = hypfs_alloc_dyndata(struct hypfs_dyndir_id);
    if ( !data )
        return ERR_PTR(-ENOMEM);
    data->id = SENSORID_NONE;

    spin_lock(&sensors_lock);

    return entry;
}

static void sensor_dir_exit(const struct hypfs_entry *entry)
{
    spin_unlock(&sensors_lock);

    hypfs_free_dyndata();
}

static struct hypfs_entry *sensor_dir_findentry(
    const struct hypfs_entry_dir *dir, const char *name, unsigned int name_len)
{
    unsigned long id;
    const char *end;
    struct thermal_sensor *sensor;

    id = simple_strtoul(name, &end, 10);
    if ( end != name + name_len || id > UINT_MAX )
        return ERR_PTR(-ENOENT);

    sensor = get_sensor_by_id(id);

    if ( !sensor )
        return ERR_PTR(-ENOENT);

    return hypfs_gen_dyndir_id_entry(&sensors_listdir, id, sensor);
}

#define TRIPS_STRING_MAX 150
static char __read_mostly tripsstr[TRIPS_STRING_MAX] = {
    [0 ... TRIPS_STRING_MAX - 2] = '?',
    [TRIPS_STRING_MAX - 1] = 0
};

static int get_trips_line(const struct thermal_sensor *sensor, char *line)
{
    int i, pos = 0;
    int trip_temp;
    enum thermal_trip_type trip_type;

    for (i = 0; i < sensor->trips; i++)
    {
        sensor->ops->get_trip_temp(sensor->data, i, &trip_temp);
        sensor->ops->get_trip_type(sensor->data, i, &trip_type);

        pos += scnprintf(&line[pos], TRIPS_STRING_MAX,
                "T[%d]:temp=%d:type=%d;", i, trip_temp, trip_type);
        if ( pos > TRIPS_STRING_MAX )
            return pos;
    }

    line[pos - 1] = '\0';
    return pos;
}

static int sensor_var_read(const struct hypfs_entry *entry,
                             XEN_GUEST_HANDLE_PARAM(void) uaddr)
{
    const struct hypfs_dyndir_id *data;
    const struct thermal_sensor *sensor;

    data = hypfs_get_dyndata();
    sensor = data->data;
    ASSERT(sensor);

    if ( strcmp(entry->name, "temperature") == 0 )
    {
        int temperature;
        int ret;
        ret = sensor->ops->get_temp(sensor->data, &temperature);
        if ( ret )
            return -ENOENT;

        return copy_to_guest(uaddr, &temperature, sizeof(temperature)) ?
            -EFAULT : 0;
    }
    else if ( strcmp(entry->name, "throttle") == 0 )
    {
        return
            copy_to_guest(uaddr, &sensor->throttle, sizeof(sensor->throttle)) ?
            -EFAULT : 0;
    }
    else if ( strcmp(entry->name, "trips") == 0 )
    {
        int sz;
        sz = get_trips_line(sensor, tripsstr);
        return
            copy_to_guest(uaddr, tripsstr, sz) ?
            -EFAULT : 0;
    }

    return -EFAULT;
}

static unsigned int sensor_var_getsize(const struct hypfs_entry *entry)
{
    const struct hypfs_dyndir_id *data;
    const struct thermal_sensor *sensor;

    data = hypfs_get_dyndata();
    sensor = data->data;
    ASSERT(sensor);
    if ( strcmp(entry->name, "temperature") == 0 )
        return sizeof(int);
    else if ( strcmp(entry->name, "throttle") == 0 )
        return sizeof(bool_t);
    else if ( strcmp(entry->name, "trips") == 0 )
        return get_trips_line(sensor, tripsstr);

    return -EFAULT;
}

static const struct hypfs_funcs sensor_var_funcs = {
    .enter = hypfs_node_enter,
    .exit = hypfs_node_exit,
    .read = sensor_var_read,
    .write = hypfs_write_deny,
    .getsize = sensor_var_getsize,
    .findentry = hypfs_leaf_findentry,
};

static int __read_mostly temperature_val;
static bool __read_mostly throttle_val;

static HYPFS_FIXEDSIZE_INIT(temperature, XEN_HYPFS_TYPE_INT, "temperature",
                            temperature_val, &sensor_var_funcs, 0);
static HYPFS_FIXEDSIZE_INIT(throttle, XEN_HYPFS_TYPE_BOOL, "throttle",
                            throttle_val, &sensor_var_funcs, 0);
static HYPFS_VARSIZE_INIT(trips, XEN_HYPFS_TYPE_STRING, "trips",
                          TRIPS_STRING_MAX, &sensor_var_funcs);

static const struct hypfs_funcs sensor_dir_funcs = {
    .enter = sensor_dir_enter,
    .exit = sensor_dir_exit,
    .read = sensor_dir_read,
    .write = hypfs_write_deny,
    .getsize = sensor_dir_getsize,
    .findentry = sensor_dir_findentry,
};


static HYPFS_DIR_INIT(thermal_dir, "thermal");
static HYPFS_DIR_INIT_FUNC(sensors_dir, "sensors", &sensor_dir_funcs);

#define AVAIL_GOVERNORS_MAX 60
static char __read_mostly thermal_avail_governors[AVAIL_GOVERNORS_MAX];
static char __read_mostly thermal_governor_name[THERMAL_NAME_LEN];

static int get_avail_governors_line(char *line)
{
    struct thermal_governor *t;
    int i = 0;

    list_for_each_entry(t, &thermal_governor_list, governor_list)
    {
        i += scnprintf(&line[i], THERMAL_NAME_LEN, "%s ",
                t->name);
        if ( i > AVAIL_GOVERNORS_MAX )
            return -EFAULT;
    }

    line[i - 1] = '\0';

    return i;
}

static int avail_gov_read(const struct hypfs_entry *entry,
                             XEN_GUEST_HANDLE_PARAM(void) uaddr)
{
    int sz = get_avail_governors_line(thermal_avail_governors);

    return copy_to_guest(uaddr, thermal_avail_governors, sz) ?
           -EFAULT : 0;
}

static unsigned int avail_gov_getsize(const struct hypfs_entry *entry)
{
    return get_avail_governors_line(thermal_avail_governors);
}

static const struct hypfs_funcs avail_gov_funcs = {
    .enter = hypfs_node_enter,
    .exit = hypfs_node_exit,
    .read = avail_gov_read,
    .write = hypfs_write_deny,
    .getsize = avail_gov_getsize,
    .findentry = hypfs_leaf_findentry,
};

static HYPFS_VARSIZE_INIT(avail_governors, XEN_HYPFS_TYPE_STRING,
        "avail_governors", AVAIL_GOVERNORS_MAX, &avail_gov_funcs);

static int thermal_gov_read(const struct hypfs_entry *entry,
                             XEN_GUEST_HANDLE_PARAM(void) uaddr)
{
    return copy_to_guest(uaddr, thermal_opt_governor->name, THERMAL_NAME_LEN) ?
           -EFAULT : 0;
}

static unsigned int thermal_gov_getsize(const struct hypfs_entry *entry)
{
    return THERMAL_NAME_LEN;
}

static int thermal_gov_write(struct hypfs_entry_leaf *leaf,
                 XEN_GUEST_HANDLE_PARAM(const_void) uaddr, unsigned int ulen)
{
    char name[THERMAL_NAME_LEN];
    struct thermal_governor *new_governor;
    if ( ulen > THERMAL_NAME_LEN )
        return -ENOSPC;

    if ( copy_from_guest(name, uaddr, ulen) )
        return -EFAULT;

    new_governor = __find_thermal_governor(name);
    if ( !new_governor )
        return -EINVAL;

    thermal_opt_governor = new_governor;
    return 0;
}

static const struct hypfs_funcs thermal_gov_funcs = {
    .enter = hypfs_node_enter,
    .exit = hypfs_node_exit,
    .read = thermal_gov_read,
    .write = thermal_gov_write,
    .getsize = thermal_gov_getsize,
    .findentry = hypfs_leaf_findentry,
};

static HYPFS_VARSIZE_INIT(thermal_gov, XEN_HYPFS_TYPE_STRING,
        "thermal_governor", THERMAL_NAME_LEN, &thermal_gov_funcs);

static void thermal_hypfs_init(void)
{
    hypfs_add_dir(&hypfs_root, &thermal_dir, true);
    hypfs_string_set_reference(&avail_governors, thermal_avail_governors);
    hypfs_add_leaf(&thermal_dir, &avail_governors, true);
    hypfs_string_set_reference(&thermal_gov, thermal_governor_name);
    hypfs_add_leaf(&thermal_dir, &thermal_gov, true);
    hypfs_add_dir(&thermal_dir, &sensors_dir, true);
    hypfs_add_dyndir(&sensors_dir, &sensors_listdir);
    hypfs_add_leaf(&sensors_listdir, &temperature, true);
    hypfs_add_leaf(&sensors_listdir, &throttle, true);
    hypfs_string_set_reference(&trips, tripsstr);
    hypfs_add_leaf(&sensors_listdir, &trips, true);
}
#else /* CONFIG_HYPFS */

static void thermal_hypfs_init(void)
{
}

#endif /* CONFIG_HYPFS */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
