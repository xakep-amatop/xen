/*
 * xen/arch/arm/cpufreq/imx_cpufreq.c
 *
 * CPUFreq driver for i.MX8 platform
 *
 * Based on Xen arch/arm/cpufreq/scpi_cpufreq.c
 *
 * Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 * Copyright (c) 2022 EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include "asm-arm/device.h"
#include "xen/config.h"
#include <asm/sci.h>
#include <asm/smccc.h>
#include <xen/types.h>
#include <xen/delay.h>
#include <xen/cpumask.h>
#include <xen/sched.h>
#include <xen/xmalloc.h>
#include <xen/err.h>
#include <xen/cpufreq.h>
#include <asm/bug.h>
#include <asm/percpu.h>
#include <xen/pmstat.h>
#include <xen/keyhandler.h>

#define IMX_SIP_CPUFREQ         0xC2000001
#define IMX_SIP_SET_CPUFREQ     0

bool cpufreq_debug = false;

/*
 * To protect changing frequency driven by both CPUFreq governor and
 * CPU throttling work.
 */
static DEFINE_SPINLOCK(freq_lock);

#define OPP_MAX 8

struct freq_opp {
	u64 freq;
	u32 m_volt;
    u32 clock_latency;
};

struct dvfs_info {
	unsigned int count;
	struct freq_opp opps[OPP_MAX];
};

struct cpufreq_data
{
    int cpu;
    struct processor_performance *perf;
    struct cpufreq_frequency_table *freq_table;
    bool turbo_prohibited;
    int resource; /* resource id this CPU belongs to */

};

static struct cpufreq_data *cpufreq_driver_data[NR_CPUS];
static struct dvfs_info *cpufreq_dvfs_info[NR_CPUS];

static int imx_cpufreq_update(int cpuid, struct cpufreq_policy *policy)
{
    if ( !cpumask_test_cpu(cpuid, &cpu_online_map) )
        return -EINVAL;

    if ( policy->turbo != CPUFREQ_TURBO_UNSUPPORTED )
    {
        /* TODO Do we need some actions here? */
        if ( policy->turbo == CPUFREQ_TURBO_ENABLED )
            printk(XENLOG_INFO "cpu%u: Turbo Mode enabled\n", policy->cpu);
        else
            printk(XENLOG_INFO "cpu%u: Turbo Mode disabled\n", policy->cpu);
    }
    else
        printk(XENLOG_INFO "cpu%u: Turbo Mode unsupported\n", policy->cpu);

    return 0;
}

#define dev_name(dev) dt_node_full_name(dev_to_dt(dev))

struct device *get_cpu_device(unsigned int cpu)
{
    if ( cpu < nr_cpu_ids && cpu_possible(cpu) )
        return dt_to_dev(cpu_dt_nodes[cpu]);
    else
        return NULL;
}

static const struct dvfs_info *dvfs_get_info(unsigned int cpu)
{
    struct dt_device_node *opp_np, *child;
    struct dt_device_node *cpu_dt;
    struct device *cpu_dev = get_cpu_device(cpu);
    struct dvfs_info *info;
    int ret;
    u32 val;

    if (cpufreq_dvfs_info[cpu])
    {
        return cpufreq_dvfs_info[cpu];
    }

    info = xzalloc(struct dvfs_info);
    if ( !info )
        return ERR_PTR(-ENOMEM);

    cpu_dt = dev_to_dt(cpu_dev);

    opp_np = dt_parse_phandle(cpu_dt, "operating-points-v2", 0);
    if (!opp_np)
    {
        printk (XENLOG_ERR "Unable to find opp node for cpu: %s\n",
                cpu_dt->full_name);
        ret = -ENODATA;
        goto err;
    }

    dt_for_each_child_node(opp_np, child)
    {
        ret = dt_property_read_u64(child, "opp-hz",
                &info->opps[info->count].freq);
        if (!ret)
            printk(XENLOG_WARNING "%s: opp-hz is not set\n", child->name);

        ret = dt_property_read_u32(child, "opp-microvolt", &val);
        if (!ret)
            printk(XENLOG_WARNING "%s: opp-microvolt is not set\n", child->name);

        info->opps[info->count].m_volt = val;

        ret = dt_property_read_u32(child, "clock-latency-ns", &val);
        if (!ret)
            printk(XENLOG_WARNING "%s: clock-latency-ns is not set\n",
                    child->full_name);

        info->opps[info->count].clock_latency = val;

        info->count++;
    }

    cpufreq_dvfs_info[cpu] = info;
    return info;
err:
    xfree(info);
    return ERR_PTR(ret);
}

static int dvfs_get_idx(struct cpufreq_data *data, int *idx)
{
    int ret, i;
    uint32_t rate;
    const struct dvfs_info *info;

    ret = sc_pm_get_clock_rate(mu_ipcHandle, data->resource,
        SC_PM_CLK_CPU, &rate);

    if (ret) {
        printk(XENLOG_ERR "read cpu clock %d failed, ret %d\n",
                data->resource, ret);
        return ret;
    }

    info = dvfs_get_info(data->cpu);
    if ( IS_ERR(info))
    {
        return PTR_ERR(info);
    }

    for (i=0; i< info->count; i++)
        if (info->opps[i].freq == rate)
        {
            *idx = i;
            return 0;
        }

    return -ENODATA;
}

static int dvfs_set(int resource_id, unsigned int freq)
{
    struct arm_smccc_res res;
    arm_smccc_smc(IMX_SIP_CPUFREQ, IMX_SIP_SET_CPUFREQ, resource_id,
            freq * 1000 /* kHz to Hz */, &res);
    if (res.a0)
        return -EINVAL;

    return 0;
}

static int imx_cpufreq_set(unsigned int cpu, unsigned int freq)
{
    struct cpufreq_data *data;
    struct cpufreq_policy *policy;

    if ( cpu >= nr_cpu_ids || !cpu_online(cpu) )
        return 0;

    policy = per_cpu(cpufreq_cpu_policy, cpu);
    if ( !policy || !(data = cpufreq_driver_data[policy->cpu]) ||
         !dvfs_get_info(data->cpu) )
        return 0;

    return dvfs_set(data->resource, freq);
}

static unsigned int imx_cpufreq_get(unsigned int cpu)
{
    struct cpufreq_data *data;
    struct cpufreq_policy *policy;
    const struct dvfs_info *info;
    int ret, idx = 0;

    if ( cpu >= nr_cpu_ids || !cpu_online(cpu) )
        return 0;

    info = dvfs_get_info(cpu);
    if ( IS_ERR(info) < 0)
        return 0;

    policy = per_cpu(cpufreq_cpu_policy, cpu);
    if ( !policy || !(data = cpufreq_driver_data[policy->cpu]))
        return 0;

    ret = dvfs_get_idx(data, &idx);
    if ( ret )
        return 0;

    /* Convert Hz -> kHz */
    return info->opps[idx].freq / 1000;
}

static int imx_cpufreq_target_unlocked(struct cpufreq_policy *policy,
                                        unsigned int target_freq,
                                        unsigned int relation)
{
    struct cpufreq_data *data = cpufreq_driver_data[policy->cpu];
    struct processor_performance *perf;
    struct cpufreq_freqs freqs;
    cpumask_t online_policy_cpus;
    unsigned int next_state = 0; /* Index into freq_table */
    unsigned int next_perf_state = 0; /* Index into perf table */
    unsigned int j;
    int result;

    if ( unlikely(!data || !data->perf || !data->freq_table ||
                IS_ERR(dvfs_get_info(data->cpu))) )
        return -ENODEV;

    if ( policy->turbo == CPUFREQ_TURBO_DISABLED ||
            cpufreq_driver_data[policy->cpu]->turbo_prohibited)
        if ( target_freq > policy->cpuinfo.second_max_freq )
            target_freq = policy->cpuinfo.second_max_freq;

    perf = data->perf;
    result = cpufreq_frequency_table_target(policy,
                                            data->freq_table,
                                            target_freq,
                                            relation, &next_state);
    if ( unlikely(result) )
        return -ENODEV;

    cpumask_and(&online_policy_cpus, &cpu_online_map, policy->cpus);

    next_perf_state = data->freq_table[next_state].index;
    if ( perf->state == next_perf_state )
    {
        if ( unlikely(policy->resume) )
            policy->resume = 0;
        else
            return 0;
    }

    /* Convert MHz -> kHz */
    freqs.old = perf->states[perf->state].core_frequency * 1000;
    freqs.new = data->freq_table[next_state].frequency;

    result = imx_cpufreq_set(policy->cpu, freqs.new);
    if ( result < 0 )
        return result;

    if (cpufreq_debug)
        printk(XENLOG_ERR "Switch CPU%u freq: %u kHz --> %u kHz\n", policy->cpu,
               freqs.old, freqs.new);

    for_each_cpu( j, &online_policy_cpus )
        cpufreq_statistic_update(j, perf->state, next_perf_state);

    perf->state = next_perf_state;
    policy->cur = freqs.new;

    return result;
}

static int imx_cpufreq_target(struct cpufreq_policy *policy,
                               unsigned int target_freq, unsigned int relation)
{
    int result;

    spin_lock(&freq_lock);
    result = imx_cpufreq_target_unlocked(policy, target_freq, relation);
    spin_unlock(&freq_lock);

    return result;
}

static int imx_cpufreq_verify(struct cpufreq_policy *policy)
{
    struct cpufreq_data *data;
    struct processor_performance *perf;

    if ( !policy || !(data = cpufreq_driver_data[policy->cpu]) ||
         !processor_pminfo[policy->cpu] )
        return -EINVAL;

    perf = &processor_pminfo[policy->cpu]->perf;

    /* Convert MHz -> kHz */
    cpufreq_verify_within_limits(policy, 0,
        perf->states[perf->platform_limit].core_frequency * 1000);

    return cpufreq_frequency_table_verify(policy, data->freq_table);
}

/* TODO Add a way to recognize Boost frequencies */
static inline bool is_turbo_freq(int index, int count)
{
    /* ugly Boost frequencies recognition */
    switch ( count )
    {
    /* A53 and A72 set 3 turbo frequencies and 1 low */
    case 4:
        return index <= 2 ? true : false;
    default:
        return false;
    }
}

static int device_domain_resource(struct device *cpu_dev)
{
    struct dt_phandle_args clock_specs;
    int ret;

    ret = dt_parse_phandle_with_args(cpu_dev->of_node,
                "clocks",
                "#clock-cells",
                0,
                &clock_specs);

    if (clock_specs.args_count > 2) {
        printk(XENLOG_WARNING "%s: too many cells in clock specifier %d\n",
            cpu_dev->of_node->name, clock_specs.args_count);
    }

    return clock_specs.args_count ? clock_specs.args[0] : 0;
}

static int imx_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
    int result;
    int resource;
    unsigned int i;
    unsigned int valid_states = 0;
    unsigned int curr_state, curr_freq;
    struct processor_performance *perf;
    struct device *cpu_dev;
    struct cpufreq_data *data;

    cpu_dev = get_cpu_device(policy->cpu);
    if ( !cpu_dev )
        return -ENODEV;

    data = xzalloc(struct cpufreq_data);
    if ( !data )
        return -ENOMEM;

    cpufreq_driver_data[policy->cpu] = data;
    data->perf = &processor_pminfo[policy->cpu]->perf;

    perf = data->perf;
    policy->shared_type = perf->shared_type;

    data->freq_table = xmalloc_array(struct cpufreq_frequency_table,
                                    (perf->state_count + 1));
    if ( !data->freq_table )
    {
        result = -ENOMEM;
        goto err_unreg;
    }

    /* Detect transition latency */
    policy->cpuinfo.transition_latency = 0;
    for ( i = 0; i < perf->state_count; i++ )
    {
        /* Compare in ns */
        if ( perf->states[i].transition_latency * 1000 >
             policy->cpuinfo.transition_latency )
            /* Convert us -> ns */
            policy->cpuinfo.transition_latency =
                perf->states[i].transition_latency * 1000;
    }

    policy->governor = cpufreq_opt_governor ? : CPUFREQ_DEFAULT_GOVERNOR;

    /* Boost is not supported by default */
    policy->turbo = CPUFREQ_TURBO_UNSUPPORTED;

    /* Initialize frequency table */
    for ( i = 0; i < perf->state_count; i++ )
    {
        /* Compare in MHz */
        if ( i > 0 && perf->states[i].core_frequency >=
             data->freq_table[valid_states - 1].frequency / 1000 )
            continue;

        data->freq_table[valid_states].index = i;
        /* Convert MHz -> kHz */
        data->freq_table[valid_states].frequency =
            perf->states[i].core_frequency * 1000;

        data->freq_table[valid_states].flags = 0;
        if ( is_turbo_freq(valid_states, perf->state_count) )
        {
            printk(XENLOG_INFO "cpu%u: Turbo freq detected: %u\n",
                   policy->cpu, data->freq_table[valid_states].frequency);
            data->freq_table[valid_states].flags |= CPUFREQ_BOOST_FREQ;

            if ( policy->turbo == CPUFREQ_TURBO_UNSUPPORTED )
            {
                printk(XENLOG_INFO "cpu%u: Turbo Mode detected and enabled\n",
                       policy->cpu);
                policy->turbo = CPUFREQ_TURBO_ENABLED;
            }
        }

        valid_states++;
    }
    data->freq_table[valid_states].frequency = CPUFREQ_TABLE_END;
    perf->state = 0;

    result = cpufreq_frequency_table_cpuinfo(policy, data->freq_table);
    if ( result )
        goto err_freqfree;

    /* Fill in fields needed for frequency changing */
    resource = device_domain_resource(cpu_dev);
    if ( resource < 0 )
    {
        result = resource;
        goto err_freqfree;
    }
    data->resource = resource;

    data->cpu = policy->cpu;
    /* Retrieve current frequency */
    curr_freq = imx_cpufreq_get(policy->cpu);

    /* Find corresponding state */
    curr_state = 0;
    for ( i = 0; data->freq_table[i].frequency != CPUFREQ_TABLE_END; i++ )
    {
        if ( curr_freq == data->freq_table[i].frequency )
        {
            curr_state = i;
            break;
        }
    }

    /* Update fields with actual values */
    policy->cur = curr_freq;
    perf->state = data->freq_table[curr_state].index;

    /*
     * the first call to ->target() should result in us actually
     * writing something to the appropriate registers.
     */
    policy->resume = 1;

    return result;

err_freqfree:
    xfree(data->freq_table);
err_unreg:
    xfree(data);
    cpufreq_driver_data[policy->cpu] = NULL;

    return result;
}

static int imx_cpufreq_cpu_exit(struct cpufreq_policy *policy)
{
    struct cpufreq_data *data = cpufreq_driver_data[policy->cpu];

    if ( data )
    {
        xfree(data->freq_table);
        xfree(data);
        cpufreq_driver_data[policy->cpu] = NULL;
        xfree(cpufreq_dvfs_info[policy->cpu]);
        cpufreq_dvfs_info[policy->cpu] = NULL;
    }

    return 0;
}

static struct cpufreq_driver imx_cpufreq_driver = {
    .name   = "imx-cpufreq",
    .verify = imx_cpufreq_verify,
    .target = imx_cpufreq_target,
    .get    = imx_cpufreq_get,
    .init   = imx_cpufreq_cpu_init,
    .exit   = imx_cpufreq_cpu_exit,
    .update = imx_cpufreq_update,
};

int imx_cpufreq_throttle(bool enable, int cpu)
{
    struct cpufreq_policy *policy;
    int result = 0;

    policy = per_cpu(cpufreq_cpu_policy, cpu);
    if ( !policy )
       return 0;

    if ( !enable )
    {
        /* Just allow to set any frequencies... */
        cpufreq_driver_data[policy->cpu]->turbo_prohibited = false;
    }
    else
    {
        spin_lock(&freq_lock);
        /* Check if we are running on turbo frequency */
        if ( policy->cur > policy->cpuinfo.second_max_freq )
        {
            /* Set max non-turbo frequency */
            result = imx_cpufreq_set(policy->cpu,
                                      policy->cpuinfo.second_max_freq);
            if ( result < 0 )
            {
                spin_unlock(&freq_lock);
                return result;
            }
        }
        /* Signal that turbo frequencies are not allowed to be set */
        cpufreq_driver_data[policy->cpu]->turbo_prohibited = true;
        spin_unlock(&freq_lock);
    }

    printk(XENLOG_INFO "cpu%u: %s CPU throttling\n", policy->cpu,
           cpufreq_driver_data[policy->cpu]->turbo_prohibited? "Enable" : "Disable");

    return 0;
}

int cpufreq_cpu_init(unsigned int cpuid)
{
    return cpufreq_add_cpu(cpuid);
}

static int thermal_init(void)
{
	struct dt_device_node *ths;
	unsigned int num_ths = 0;
	int rc;

	dt_for_each_device_node(dt_host, ths) {
		rc = device_init(ths, DEVICE_THS, NULL);
		if (!rc)
			num_ths ++;
	}

	return (num_ths > 0) ? 0 : -ENODEV;
}

void cpufreq_debug_toggle(unsigned char key)
{
    cpufreq_debug = !cpufreq_debug;
    printk(XENLOG_ERR "CPUFreq debug is %s\n", cpufreq_debug ? "enabled" : "disabled");
}

/* TODO Implement me */
static void cpufreq_imx_driver_deinit(void)
{
}

static bool is_dvfs_capable(unsigned int cpu)
{
    static const struct dt_device_match dvfs_clock_match[] =
    {
        DT_MATCH_COMPATIBLE("fsl,scu-clk"),
        DT_MATCH_COMPATIBLE("fsl,imx8qm-clk"),
        { /* sentinel */ },
    };
    struct device *cpu_dev;
    struct dt_phandle_args clock_spec;
    const struct dvfs_info *info;
    int ret;

    cpu_dev = get_cpu_device(cpu);
    if ( !cpu_dev )
    {
        printk(XENLOG_ERR "cpu%d: failed to get device\n", cpu);
        return false;
    }

    /* First of all find a clock node this CPU is a consumer of */
    ret = dt_parse_phandle_with_args(cpu_dev->of_node,
                                     "clocks",
                                     "#clock-cells",
                                     0,
                                     &clock_spec);
    if ( ret )
    {
        printk(XENLOG_ERR "cpu%d: failed to get clock node\n", cpu);
        return false;
    }

    /* Make sure it is an available DVFS clock node */
    if ( !dt_match_node(dvfs_clock_match, clock_spec.np) ||
         !dt_device_is_available(clock_spec.np) )
    {
        printk(XENLOG_ERR "cpu%d: clock node '%s' is either non-DVFS or non-available\n",
               cpu, dev_name(&clock_spec.np->dev));
        return false;
    }

    if ( clock_spec.args_count < 2 )
    {
        printk(XENLOG_ERR "format mismatch for cpu %d\n", cpu);
    }

    info = dvfs_get_info(cpu);
    if ( ret )
    {
        printk(XENLOG_ERR "cpu%d: failed to get DVFS info of imx id %u\n", cpu,
                clock_spec.args[0]);
        return false;
    }
    printk(XENLOG_DEBUG "cpu%d: is DVFS capable, belongs to pd%u\n",
           cpu, clock_spec.args[0]);

    return true;
}

static int get_sharing_cpus(unsigned int cpu, cpumask_t *mask)
{
    struct device *cpu_dev = get_cpu_device(cpu), *tcpu_dev;
    unsigned int tcpu;
    int domain, tdomain;

    BUG_ON(!cpu_dev);

    domain = device_domain_resource(cpu_dev);
    if ( domain < 0 )
        return domain;

    cpumask_clear(mask);
    cpumask_set_cpu(cpu, mask);

    for_each_online_cpu( tcpu )
    {
        if ( tcpu == cpu )
            continue;

        tcpu_dev = get_cpu_device(tcpu);
        if ( !tcpu_dev )
            continue;

        tdomain = device_domain_resource(tcpu_dev);
        if ( tdomain == domain )
            cpumask_set_cpu(tcpu, mask);
    }

    return 0;
}

static int get_transition_latency(unsigned int cpu)
{
    const struct dvfs_info *info;

    info = dvfs_get_info(cpu);
    if ( IS_ERR(info) || info->count == 0 )
        return 0;

    return info->opps[0].clock_latency;
}

static int init_cpufreq_table(unsigned int cpu,
                              struct cpufreq_frequency_table **table)
{
    struct cpufreq_frequency_table *freq_table = NULL;
    struct device *cpu_dev = get_cpu_device(cpu);
    const struct dvfs_info *info;
    int i;

    BUG_ON(!cpu_dev);

    info = dvfs_get_info(cpu);
    if ( IS_ERR(info) )
        return PTR_ERR(info);

    if ( !info->count )
        return -EIO;

    freq_table = xzalloc_array(struct cpufreq_frequency_table, info->count + 1);
    if ( !freq_table )
        return -ENOMEM;

    for ( i = 0; i < info->count; i++ )
    {
        freq_table[i].index = i;
        /* Convert Hz -> kHz */
        freq_table[i].frequency = info->opps[i].freq / 1000;
    }

    freq_table[i].index = i;
    freq_table[i].frequency = CPUFREQ_TABLE_END;

    *table = &freq_table[0];

    return 0;
}

static void free_cpufreq_table(struct cpufreq_frequency_table **table)
{
    if ( !table )
        return;

    xfree(*table);
    *table = NULL;
}

static int upload_cpufreq_data(cpumask_t *mask,
                               struct cpufreq_frequency_table *table)
{
    struct xen_processor_performance *perf;
    struct xen_processor_px *states;
    uint32_t platform_limit = 0, state_count = 0;
    unsigned int max_freq = 0, prev_freq = 0, cpu = cpumask_first(mask);
    int i, latency, ret = 0;

    perf = xzalloc(struct xen_processor_performance);
    if ( !perf )
        return -ENOMEM;

    /* Check frequency table and find max frequency */
    for ( i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++ )
    {
        unsigned int freq = table[i].frequency;

        if ( freq == CPUFREQ_ENTRY_INVALID )
            continue;

        if ( table[i].index != state_count || freq <= prev_freq )
        {
            printk(XENLOG_ERR "cpu%d: frequency table format error\n", cpu);
            ret = -EINVAL;
            goto out;
        }

        prev_freq = freq;
        state_count++;
        if ( freq > max_freq )
            max_freq = freq;
    }

    /*
     * The frequency table we have is just a temporary place for storing
     * provided by SCP DVFS info. Create performance states array.
     */
    if ( !state_count )
    {
        printk(XENLOG_ERR "cpu%d: no available performance states\n", cpu);
        ret = -EINVAL;
        goto out;
    }

    states = xzalloc_array(struct xen_processor_px, state_count);
    if ( !states )
    {
        ret = -ENOMEM;
        goto out;
    }

    set_xen_guest_handle(perf->states, states);
    perf->state_count = state_count;

    latency = get_transition_latency(cpu);

    /* Performance states must start from higher values */
    for ( i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++ )
    {
        unsigned int freq = table[i].frequency;
        unsigned int index = state_count - 1 - table[i].index;

        if ( freq == CPUFREQ_ENTRY_INVALID )
            continue;

        if ( freq == max_freq )
            platform_limit = index;

        /* Convert kHz -> MHz */
        states[index].core_frequency = freq / 1000;
        /* Convert ns -> us */
        states[index].transition_latency = DIV_ROUND_UP(latency, 1000);
    }

    perf->flags = XEN_PX_PSD | XEN_PX_PSS | XEN_PX_PCT | XEN_PX_PPC |
                  XEN_PX_DATA; /* all P-state data in a one-shot */
    perf->platform_limit = platform_limit;
    perf->shared_type = CPUFREQ_SHARED_TYPE_ANY;
    perf->domain_info.domain = cpumask_first(mask);
    perf->domain_info.num_processors = cpumask_weight(mask);

    /* Iterate through all CPUs which are on the same boat */
    for_each_cpu( cpu, mask )
    {
        ret = set_px_pminfo(cpu, perf);
        if ( ret )
        {
            printk(XENLOG_ERR "cpu%d: failed to set Px states (%d)\n", cpu, ret);
            break;
        }

        printk(XENLOG_DEBUG "cpu%d: set Px states\n", cpu);
    }

    xfree(states);
out:
    xfree(perf);

    return ret;
}

static int __init imx_cpufreq_postinit(void)
{
    struct cpufreq_frequency_table *freq_table = NULL;
    cpumask_t processed_cpus, shared_cpus;
    unsigned int cpu;
    int ret = -ENODEV;

    cpumask_clear(&processed_cpus);

    for_each_online_cpu( cpu )
    {
        if ( cpumask_test_cpu(cpu, &processed_cpus) )
            continue;

        if ( !is_dvfs_capable(cpu) )
        {
            printk(XENLOG_DEBUG "cpu%d: isn't DVFS capable, skip it\n", cpu);
            continue;
        }

        ret = get_sharing_cpus(cpu, &shared_cpus);
        if ( ret )
        {
            printk(XENLOG_ERR "cpu%d: failed to get sharing cpumask (%d)\n", cpu, ret);
            return ret;
        }

        BUG_ON(cpumask_empty(&shared_cpus));
        cpumask_or(&processed_cpus, &processed_cpus, &shared_cpus);

        /* Create intermediate frequency table */
        ret = init_cpufreq_table(cpu, &freq_table);
        if ( ret )
        {
            printk(XENLOG_ERR "cpu%d: failed to initialize frequency table (%d)\n",
                   cpu, ret);
            return ret;
        }

        ret = upload_cpufreq_data(&shared_cpus, freq_table);
        /* Destroy intermediate frequency table */
        free_cpufreq_table(&freq_table);
        if ( ret )
        {
            printk(XENLOG_ERR "cpu%d: failed to upload cpufreq data (%d)\n", cpu, ret);
            return ret;
        }

        printk(XENLOG_DEBUG "cpu%d: uploaded cpufreq data\n", cpu);
    }

    return ret;
}


static int __init cpufreq_imx_driver_init(void)
{
    int ret;

    if ( cpufreq_controller != FREQCTL_xen )
        return 0;

    ret = thermal_init();
    if ( ret )
    {
        printk(XENLOG_ERR "failed to initialize thermal (%d)\n", ret);
        goto out;
    }

    ret = cpufreq_register_driver(&imx_cpufreq_driver);
    if ( ret )
        goto out;

    ret = imx_cpufreq_postinit();
out:
    if ( ret )
    {
        printk(XENLOG_ERR "failed to initialize i.MX8 CPUFreq driver (%d)\n", ret);
        cpufreq_imx_driver_deinit();
        return ret;
    }

    register_keyhandler('C', cpufreq_debug_toggle,
                        "enable debug for CPUFreq", 0);

    printk(XENLOG_INFO "initialized i.MX8 CPUFreq driver\n");
    return ret;
}
__initcall(cpufreq_imx_driver_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
