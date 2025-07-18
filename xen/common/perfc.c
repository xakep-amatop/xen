
#include <xen/cpumask.h>
#include <xen/errno.h>
#include <xen/guest_access.h>
#include <xen/lib.h>
#include <xen/perfc.h>
#include <xen/spinlock.h>
#include <xen/time.h>

#include <public/sysctl.h>

#define PERFCOUNTER( var, name )              { name, TYPE_SINGLE, 0 },
#define PERFCOUNTER_ARRAY( var, name, size )  { name, TYPE_ARRAY,  size },
#define PERFSTATUS( var, name )               { name, TYPE_S_SINGLE, 0 },
#define PERFSTATUS_ARRAY( var, name, size )   { name, TYPE_S_ARRAY,  size },
static const struct {
    const char *name;
    enum { TYPE_SINGLE, TYPE_ARRAY,
           TYPE_S_SINGLE, TYPE_S_ARRAY
    } type;
    unsigned int nr_elements;
} perfc_info[] = {
#include <xen/perfc_defn.h>
};

#define NR_PERFCTRS (sizeof(perfc_info) / sizeof(perfc_info[0]))

DEFINE_PER_CPU(perfc_t[NUM_PERFCOUNTERS], perfcounters);

void cf_check perfc_printall(unsigned char key)
{
    unsigned int i, j;
    s_time_t now = NOW();

    printk("Xen performance counters SHOW  (now = %"PRI_stime")\n", now);

    for ( i = j = 0; i < NR_PERFCTRS; i++ )
    {
        unsigned int k, cpu;
        unsigned long long sum = 0;

        printk("%-32s  ",  perfc_info[i].name);
        switch ( perfc_info[i].type )
        {
        case TYPE_SINGLE:
        case TYPE_S_SINGLE:
            for_each_online_cpu ( cpu )
                sum += per_cpu(perfcounters, cpu)[j];
            if ( perfc_info[i].type == TYPE_S_SINGLE )
                sum = (perfc_t) sum;
            printk("TOTAL[%12Lu]", sum);
            if ( sum )
            {
                k = 0;
                for_each_online_cpu ( cpu )
                {
                    if ( k > 0 && (k % 4) == 0 )
                        printk("\n%53s", "");
                    printk("  CPU%02u[%10u]", cpu, per_cpu(perfcounters, cpu)[j]);
                    ++k;
                }
            }
            ++j;
            break;
        case TYPE_ARRAY:
        case TYPE_S_ARRAY:
            for_each_online_cpu ( cpu )
            {
                perfc_t *counters = per_cpu(perfcounters, cpu) + j;

                for ( k = 0; k < perfc_info[i].nr_elements; k++ )
                    sum += counters[k];
            }
            if ( perfc_info[i].type == TYPE_S_ARRAY )
                sum = (perfc_t) sum;
            printk("TOTAL[%12Lu]", sum);
            if (sum)
            {
#ifdef CONFIG_PERF_ARRAYS
                for ( k = 0; k < perfc_info[i].nr_elements; k++ )
                {
                    sum = 0;
                    for_each_online_cpu ( cpu )
                        sum += per_cpu(perfcounters, cpu)[j + k];
                    if ( perfc_info[i].type == TYPE_S_ARRAY )
                        sum = (perfc_t) sum;
                    if ( (k % 4) == 0 )
                        printk("\n%16s", "");
                    printk("  ARR%02u[%10Lu]", k, sum);
                }
#else
                k = 0;
                for_each_online_cpu ( cpu )
                {
                    perfc_t *counters = per_cpu(perfcounters, cpu) + j;
                    unsigned int n;

                    sum = 0;
                    for ( n = 0; n < perfc_info[i].nr_elements; n++ )
                        sum += counters[n];
                    if ( perfc_info[i].type == TYPE_S_ARRAY )
                        sum = (perfc_t) sum;
                    if ( k > 0 && (k % 4) == 0 )
                        printk("\n%53s", "");
                    printk("  CPU%02u[%10Lu]", cpu, sum);
                    ++k;
                }
#endif
            }
            j += perfc_info[i].nr_elements;
            break;
        }
        printk("\n");
    }
}

void cf_check perfc_reset(unsigned char key)
{
    unsigned int i, j;
    s_time_t now = NOW();

    if ( key != '\0' )
        printk("Xen performance counters RESET (now = %"PRI_stime")\n", now);

    /* leave STATUS counters alone -- don't reset */

    for ( i = j = 0; i < NR_PERFCTRS; i++ )
    {
        unsigned int cpu;

        switch ( perfc_info[i].type )
        {
        case TYPE_SINGLE:
            for_each_online_cpu ( cpu )
                per_cpu(perfcounters, cpu)[j] = 0;
            fallthrough;
        case TYPE_S_SINGLE:
            ++j;
            break;
        case TYPE_ARRAY:
            for_each_online_cpu ( cpu )
                memset(per_cpu(perfcounters, cpu) + j, 0,
                       perfc_info[i].nr_elements * sizeof(perfc_t));
            fallthrough;
        case TYPE_S_ARRAY:
            j += perfc_info[i].nr_elements;
            break;
        }
    }
}

#ifdef CONFIG_SYSCTL
static struct xen_sysctl_perfc_desc perfc_d[NR_PERFCTRS];
static xen_sysctl_perfc_val_t *perfc_vals;
static unsigned int      perfc_nbr_vals;
static cpumask_t         perfc_cpumap;

static int perfc_copy_info(XEN_GUEST_HANDLE_64(xen_sysctl_perfc_desc_t) desc,
                           XEN_GUEST_HANDLE_64(xen_sysctl_perfc_val_t) val)
{
    unsigned int i, j, v;

    /* We only copy the name and array-size information once. */
    if ( !cpumask_equal(&cpu_online_map, &perfc_cpumap) )
    {
        unsigned int nr_cpus;
        perfc_cpumap = cpu_online_map;
        nr_cpus = cpumask_weight(&perfc_cpumap);

        perfc_nbr_vals = 0;

        for ( i = 0; i < NR_PERFCTRS; i++ )
        {
            safe_strcpy(perfc_d[i].name, perfc_info[i].name);

            switch ( perfc_info[i].type )
            {
            case TYPE_SINGLE:
            case TYPE_S_SINGLE:
                perfc_d[i].nr_vals = nr_cpus;
                break;
            case TYPE_ARRAY:
            case TYPE_S_ARRAY:
                perfc_d[i].nr_vals = perfc_info[i].nr_elements;
                break;
            }
            perfc_nbr_vals += perfc_d[i].nr_vals;
        }

        xfree(perfc_vals);
        perfc_vals = xmalloc_array(xen_sysctl_perfc_val_t, perfc_nbr_vals);
    }

    if ( guest_handle_is_null(desc) )
        return 0;

    if ( perfc_vals == NULL )
        return -ENOMEM;

    /* We gather the counts together every time. */
    for ( i = j = v = 0; i < NR_PERFCTRS; i++ )
    {
        unsigned int cpu;

        switch ( perfc_info[i].type )
        {
        case TYPE_SINGLE:
        case TYPE_S_SINGLE:
            for_each_cpu ( cpu, &perfc_cpumap )
                perfc_vals[v++] = per_cpu(perfcounters, cpu)[j];
            ++j;
            break;
        case TYPE_ARRAY:
        case TYPE_S_ARRAY:
            memset(perfc_vals + v, 0, perfc_d[i].nr_vals * sizeof(*perfc_vals));
            for_each_cpu ( cpu, &perfc_cpumap )
            {
                perfc_t *counters = per_cpu(perfcounters, cpu) + j;
                unsigned int k;

                for ( k = 0; k < perfc_d[i].nr_vals; k++ )
                    perfc_vals[v + k] += counters[k];
            }
            v += perfc_d[i].nr_vals;
            j += perfc_info[i].nr_elements;
            break;
        }
    }
    BUG_ON(v != perfc_nbr_vals);

    if ( copy_to_guest(desc, perfc_d, NR_PERFCTRS) )
        return -EFAULT;
    if ( copy_to_guest(val, perfc_vals, perfc_nbr_vals) )
        return -EFAULT;
    return 0;
}

/* Dom0 control of perf counters */
int perfc_control(struct xen_sysctl_perfc_op *pc)
{
    static DEFINE_SPINLOCK(lock);
    int rc;

    spin_lock(&lock);

    switch ( pc->cmd )
    {
    case XEN_SYSCTL_PERFCOP_reset:
        rc = perfc_copy_info(pc->desc, pc->val);
        perfc_reset(0);
        break;

    case XEN_SYSCTL_PERFCOP_query:
        rc = perfc_copy_info(pc->desc, pc->val);
        break;

    default:
        rc = -EINVAL;
        break;
    }

    spin_unlock(&lock);

    pc->nr_counters = NR_PERFCTRS;
    pc->nr_vals = perfc_nbr_vals;

    return rc;
}
#endif /* CONFIG_SYSCTL */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
