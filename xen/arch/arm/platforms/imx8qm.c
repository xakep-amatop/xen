/*
 * xen/arch/arm/platforms/imx8qm.c
 *
 * i.MX 8QM setup
 *
 * Copyright (c) 2016 Freescale Inc.
 * Copyright 2018 NXP
 *
 * Copyright 2019 NXP
 *
 * Peng Fan <peng.fan@nxp.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/io.h>
#include <asm/sci.h>
#include <asm/psci.h>
#include <asm/p2m.h>
#include <asm/platform.h>
#include <asm/platforms/imx8qm.h>
#include <asm/smccc.h>
#include <asm/vscmi.h>
#include <xen/config.h>
#include <xen/err.h>
#include <xen/guest_access.h>
#include <xen/lib.h>
#include <xen/list.h>
#include <xen/vmap.h>
#include <xen/mm.h>
#include <xen/libfdt/libfdt.h>

#undef IMX8QM_PLAT_DEBUG

/*
 * We expect no more than the below number of always on resources.
 * The number seems reasonable as these resources are rather exceptions
 * than the normal cases.
 */
#define SC_R_ALWAYS_ON_LAST     32

/*
 * We expect not more than the below number of resources which need
 * SMMU's stream ID to be set.
 */
#define SC_R_SID_LAST           32

/*
 * We expect not more than the below number of resources defined in a
 * power-domain node's property.
 */
#define SC_R_POWER_DOMAIN_LAST  32

#define SC_R_NONE               0xFFF0

static const char * const imx8qm_dt_compat[] __initconst =
{
    "fsl,imx8qm",
    NULL
};

struct imx8qm_domain {
    domid_t domain_id;
    u32 partition_id;
    u32 partition_id_parent;
    u32 always_on_num_rsrc;
    u32 always_on_rsrcs[SC_R_ALWAYS_ON_LAST];
};

static int SC_ERR_TO_POSIX[] =
{
    0,             /* SC_ERR_NONE, 0 */
    ECONNREFUSED,  /* SC_ERR_VERSION */
    EINVAL,        /* SC_ERR_CONFIG */
    EINVAL,        /* SC_ERR_PARM */
    EACCES,        /* SC_ERR_NOACCESS */
    EPERM,         /* SC_ERR_LOCKED */
    EAGAIN,        /* SC_ERR_UNAVAILABLE */
    ENOENT,        /* SC_ERR_NOTFOUND */
    ENODEV,        /* SC_ERR_NOPOWER */
    EIO,           /* SC_ERR_IPC */
    EBUSY,         /* SC_ERR_BUSY */
    EFAULT         /* SC_ERR_FAIL */
};

static int sc_err_to_posix(int sc)
{
    if ( sc < SC_ERR_LAST )
        return SC_ERR_TO_POSIX[sc];
    return -EINVAL;
}

static int imx8qm_alloc_partition(struct imx8qm_domain *dom)
{
    sc_rm_pt_t parent_part, os_part;
    sc_err_t sci_err;

    sci_err = sc_rm_get_partition(mu_ipcHandle, &parent_part);
    if ( sci_err != SC_ERR_NONE )
        goto fail;

    sci_err = sc_rm_partition_alloc(mu_ipcHandle, &os_part, false, false,
                                    false, true, false);
    if ( sci_err != SC_ERR_NONE )
        goto fail;

    sci_err = sc_rm_set_parent(mu_ipcHandle, os_part, parent_part);
    if ( sci_err != SC_ERR_NONE )
        goto fail;

    dom->partition_id = os_part;
    dom->partition_id_parent = parent_part;

    printk(XENLOG_DEBUG "Allocated partition %d, parent %d\n",
           os_part, parent_part);
    return 0;

fail:
    return sc_err_to_posix(sci_err);
}

static int imx8qm_domain_create(struct domain *d,
                                struct xen_domctl_createdomain *config)
{
    struct imx8qm_domain *dom;
    int ret;

    /* Do nothing for the initial domain. */
    if ( d->domain_id == 0 )
        return 0;

    printk(XENLOG_DEBUG "Creating new domain, domid %d\n", d->domain_id);
    dom = xzalloc(struct imx8qm_domain);
    if ( !dom )
        return -ENOMEM;

    ret = imx8qm_alloc_partition(dom);
    if ( ret < 0 )
    {
        printk(XENLOG_ERR "Failed to allocate new partition, ret %d\n", ret);
        goto fail;
    }

    dom->domain_id = d->domain_id;

    d->arch.plat_priv = (void *)dom;
    return 0;

fail:
    xfree(dom);
    return ret;
}

static void imx8qm_keep_always_on(struct imx8qm_domain *dom)
{
    sc_err_t sci_err;
    int i;

    /*
     * Check if partition has always on resources and move those
     * to the parent, so we do not power them off now.
     */
    if ( !dom->always_on_num_rsrc )
        return;

    printk(XENLOG_DEBUG "Preserving %d power on resource(s)\n",
           dom->always_on_num_rsrc);
    for (i = 0; i < dom->always_on_num_rsrc; i++)
    {
        sci_err = sc_rm_assign_resource(mu_ipcHandle, dom->partition_id_parent,
                                        dom->always_on_rsrcs[i]);
        if ( sci_err != SC_ERR_NONE )
        {
            printk(XENLOG_ERR
                   "Failed to re-assign always on resource %d from partition %d to parent %d sci_err %d\n",
                   dom->always_on_rsrcs[i],
                   dom->partition_id, dom->partition_id_parent,
                   sci_err);
        }
    }
}

static int imx8qm_domain_destroy(struct domain *d)
{
    struct imx8qm_domain *dom = (struct imx8qm_domain *)d->arch.plat_priv;
    sc_err_t sci_err;

    printk(XENLOG_DEBUG "Destroying domain, domid %d\n", d->domain_id);

    imx8qm_keep_always_on(dom);

    printk(XENLOG_DEBUG "Powering off partition %d, parent %d\n",
           dom->partition_id, dom->partition_id_parent);
    sci_err = sc_pm_set_resource_power_mode_all(mu_ipcHandle,
                                                dom->partition_id,
                                                SC_PM_PW_MODE_OFF,
                                                SC_R_LAST);
    if ( sci_err != SC_ERR_NONE )
    {
        printk(XENLOG_ERR
               "Failed to power off partition %d, parent %d. Ignoring...\n",
               dom->partition_id, dom->partition_id_parent);
    }

    sc_rm_partition_free(mu_ipcHandle, dom->partition_id);

    xfree(dom);
    return 0;
}

/* Additional mappings for dom0 (not in the DTS) */
static int imx8qm_specific_mapping(struct domain *d)
{
    int i;
    unsigned long lpcg_array[] = LPCG_ARRAY;

    for (i = 0; i < ARRAY_SIZE(lpcg_array); i++)
    {
        map_mmio_regions(d, _gfn(lpcg_array[i]), 16,
			 _mfn(paddr_to_pfn(lpcg_array[i])));
    }

    return 0;
}

static int imx8qm_system_init(void)
{
    return imx8_mu_init();
}

static void imx8qm_system_reset(void)
{
    sc_rm_pt_t part_id;

    if ( sc_rm_get_partition(mu_ipcHandle, &part_id) == SC_ERR_NONE )
    {
        printk(XENLOG_DEBUG "Powering off and freeing partition %d\n", part_id);
        sc_pm_set_resource_power_mode_all(mu_ipcHandle, part_id,
                                          SC_PM_PW_MODE_OFF, SC_R_LAST);
        sc_rm_partition_free(mu_ipcHandle, part_id);
    }
    /* This is mainly for PSCI-0.2, which does not return if success. */
    call_psci_system_reset();
}

static void imx8qm_system_off(void)
{
  /* Add PSCI interface */
}

static bool imx8qm_smc(struct cpu_user_regs *regs)
{
    struct arm_smccc_res res;

    /*
     * IMX8 firmware is based on SMCCC 1.1. If SMCCC 1.1 is not
     * available something is wrong, don't try to handle it.
     */

    if ( !cpus_have_const_cap(ARM_SMCCC_1_1) )
    {
        printk_once(XENLOG_WARNING
                    "IMX8 firmware Error: no SMCCC 1.1 support. Disabling firmware calls\n");

        return false;
    }

    if (get_user_reg(regs, 0) ==
        ARM_SMCCC_SCMI_MBOX_TRIGGER) {
        return vscmi_handle_call(regs);
    }

    /*
     * Forward SIP directly to ATF
     */

    arm_smccc_1_1_smc(get_user_reg(regs, 0),
                      get_user_reg(regs, 1),
                      get_user_reg(regs, 2),
                      get_user_reg(regs, 3),
                      get_user_reg(regs, 4),
                      get_user_reg(regs, 5),
                      get_user_reg(regs, 6),
                      get_user_reg(regs, 7),
                      &res);

    set_user_reg(regs, 0, res.a0);
    set_user_reg(regs, 1, res.a1);
    set_user_reg(regs, 2, res.a2);
    set_user_reg(regs, 3, res.a3);

    return true;
}

#define FSL_HVC_SC     0xc6000000
extern int imx8_sc_rpc(unsigned long x1, unsigned long x2);
static bool imx8qm_handle_hvc(struct cpu_user_regs *regs)
{
    int err;

    switch (regs->x0)
    {
    case FSL_HVC_SC:
        err = imx8_sc_rpc(regs->x1, regs->x2);
        break;
    default:
        err = -ENOENT;
        break;
    }

    regs->x0 = err;

    return true;
}


int platform_deassign_dev(struct domain *d, struct dt_device_node *dev)
{
    return 0;
}

static u32 get_rsrc_from_pd(const struct dt_device_node *np,
                            struct dt_device_node **pd,
                            u32 *resource_id,
                            int res_len)
{
    const __be32 *prop;
    struct dt_device_node *pnode;
    int ret;

    *pd = NULL;
    prop = dt_get_property(np, "power-domains", NULL);
    if ( !prop )
    {
#ifdef IMX8QM_PLAT_DEBUG
        printk(XENLOG_DEBUG "Device %s has no power domains, can't get resource\n",
               np->full_name);
#endif
        return 0;
    }

    pnode = dt_find_node_by_phandle(be32_to_cpup(prop));
    if ( !pnode )
    {
#ifdef IMX8QM_PLAT_DEBUG
        printk(XENLOG_DEBUG "Device %s has no power domain node\n",
               np->full_name);
#endif
        return -EINVAL;
    }
    *pd = pnode;

    if ( !dt_property_read_u32(pnode, "reg", resource_id) )
    {
        struct dt_phandle_args masterspec;
        int i = 0;

        /*
         * It can be that this is the device tree which doesn't store the
         * resources in "reg" property, but has those in "power-domains".
         */
        ret = dt_parse_phandle_with_args(np, "power-domains",
                                         "#power-domain-cells", i,
                                         &masterspec);
        if ( ret < 0 )
        {
#ifdef IMX8QM_PLAT_DEBUG
            printk(XENLOG_DEBUG
                   "Power domain node %s has no resource assigned\n",
                   np->full_name);
#endif
            return -EINVAL;
        }
        /* Have resources defined in power-domains, grab those. */
        do {
            resource_id[i++] = masterspec.args[0];

            ret = dt_parse_phandle_with_args(np, "power-domains",
                                             "#power-domain-cells", i,
                                             &masterspec);
        } while ( (ret >= 0) && (i < res_len) );
        /* Report the number of the resources found. */
        ret = i;
    } else {
        /* Report a single resource. */
        ret = 1;
        if ( resource_id[0] == SC_R_NONE )
        {
            ret = 0;
#ifdef IMX8QM_PLAT_DEBUG
            printk(XENLOG_DEBUG
                   "Skip assigning invalid resource SC_R_NONE to power domain node %s\n",
                   np->full_name);
#endif
        }
    }

    return ret;
}

int platform_assign_dev(struct domain *d, u8 devfn, struct dt_device_node *dev,
                        u32 flag)
{
    struct dt_device_node *smmu_np, *pd;
    struct dt_phandle_args masterspec;
    const __be32 *prop;
    u32 resource_id[SC_R_SID_LAST];
    u32 len;
    int i, ret;


#ifdef IMX8QM_PLAT_DEBUG
    printk(XENLOG_ERR "Assigning device %s to domain %d\n",
           dev->full_name, d->domain_id);
#endif

    smmu_np = dt_find_compatible_node(NULL, NULL, "arm,mmu-500");
    if ( !smmu_np )
        return 0;

    /*
     * Find out the resource ID that we need to set SMMU stream ID for.
     * The device being assigned can have either resources assigned
     * in its device tree node or in its power domain:
     *   - look into "fsl,sc_rsrc_id" property and
     *     take the very first resource ID
     *   - look into the device's power domain for the resource ID
     *
     * XXX: if "fsl,sc_rsrc_id" is used then the very first resource ID
     *      must be the one which is expects SID to be assigned.
     */
    prop = dt_get_property(dev, "fsl,sc_rsrc_id", &len);
    if ( prop )
    {
        len /= sizeof(u32);
        if ( len >= ARRAY_SIZE(resource_id) )
        {
            printk(XENLOG_ERR
                   "Device %s has more than %ld resources, ignoring the rest\n",
                   dev->full_name, ARRAY_SIZE(resource_id));
            len = ARRAY_SIZE(resource_id);
        }
        if ( dt_property_read_u32_array(dev, "fsl,sc_rsrc_id",
                                         resource_id, len) )
        {
            printk(XENLOG_ERR "Failed to get resource IDs\n");
            return -EINVAL;
        }
    }
    else
    {
        /* Report single entry only. */
        ret = get_rsrc_from_pd(dev, &pd, resource_id, 1);
        if ( ret < 0 )
            return ret;
        len = 1;
    }

    i = 0;
    while (!dt_parse_phandle_with_args(smmu_np, "mmu-masters",
                                       "#stream-id-cells", i, &masterspec))
    {
        if (masterspec.np == dev)
        {
            sc_err_t sci_err;
            int j;
            u16 streamid = masterspec.args[0];

            printk(XENLOG_DEBUG "Setting master SID 0x%x for %d resource(s) of %s\n",
                   streamid, len, dev->full_name);
            for (j = 0; j < len; j++)
            {
                sci_err = sc_rm_set_master_sid(mu_ipcHandle, resource_id[j],
                                               streamid);
                if ( sci_err != SC_ERR_NONE )
                    printk(XENLOG_ERR
                           "Failed to set master SID 0x%x for resource %d, err: %d\n",
                           streamid, resource_id[j], sci_err);
            }
        }
        i++;
    }
    return 0;
}

typedef sc_err_t (*clb_passthrough)(struct imx8qm_domain *dom, int id);

static sc_err_t clb_passthrough_assign_resource(struct imx8qm_domain *dom,
                                                int id)
{
#ifdef IMX8QM_PLAT_DEBUG
    printk(XENLOG_DEBUG "Assigning resource %d domid %d\n",
           id, dom->domain_id);
#endif
    return sc_rm_assign_resource(mu_ipcHandle, dom->partition_id, id);
}

static sc_err_t clb_passthrough_assign_pad(struct imx8qm_domain *dom,
                                           int id)
{
#ifdef IMX8QM_PLAT_DEBUG
    printk(XENLOG_DEBUG "Assigning pad %d domid %d\n",
           id, dom->domain_id);
#endif
    return sc_rm_assign_pad(mu_ipcHandle, dom->partition_id, id);
}

static sc_err_t clb_passthrough_power_on_resource(struct imx8qm_domain *dom,
                                                  int id)
{
    sc_err_t sci_err;

    sci_err = clb_passthrough_assign_resource(dom, id);
    if ( sci_err != SC_ERR_NONE )
        return sci_err;

    printk(XENLOG_DEBUG "Powering on resource %d domid %d\n",
           id, dom->domain_id);
    return sc_pm_set_resource_power_mode(mu_ipcHandle, id, SC_PM_PW_MODE_ON);
}

static sc_err_t clb_passthrough_add_always_on(struct imx8qm_domain *dom,
                                              int id)
{
    int i;

    printk(XENLOG_DEBUG "Adding always on resource %d domid %d\n",
           id, dom->domain_id);

    if ( dom->always_on_num_rsrc >= ARRAY_SIZE(dom->always_on_rsrcs) )
        return SC_ERR_CONFIG;

    /* Check if we already have this resource. */
    for (i = 0; i < dom->always_on_num_rsrc; i++)
        if ( dom->always_on_rsrcs[i] == id )
            return SC_ERR_NONE;

    dom->always_on_rsrcs[dom->always_on_num_rsrc++] = id;
    return SC_ERR_NONE;
}

static int passthrough_dtdev_add_resources(struct imx8qm_domain *dom,
                                           const struct dt_device_node *np,
                                           const char *prop_name,
                                           clb_passthrough clb)
{
    u32 len;
    const __be32 *val;
    int i, ret = 0;

    /*
     * If property is not found it means that either the corresponding
     * passthrough device doesn't have any or this is a real bug
     * as device needs its resources/pads to be defined.
     * There no means to distinguish that, so do not report an error.
     */
    val = dt_get_property(np, prop_name, &len);
    if ( !val )
        return 0;

    /* len is in octets. */
    for (i = 0; i < len / sizeof(u32); i++)
    {
        int id;
        sc_err_t sci_err;

        id = be32_to_cpup(val++);
        sci_err = clb(dom, id);
        if ( sci_err != SC_ERR_NONE )
        {
            printk(XENLOG_ERR
                   "Failed to assign %d (%s %s) to domain id %d sci_err %d\n",
                   id, np->full_name, prop_name, dom->domain_id, sci_err);
            ret = sc_err_to_posix(sci_err);
            break;
        }
#ifdef IMX8QM_PLAT_DEBUG
        printk(XENLOG_DEBUG
               "Assign %d (%s %s) to domain id %d\n",
               id, np->full_name, prop_name, dom->domain_id);
#endif
    }

    return ret;
}

static int passthrough_dtdev_add_resources_pd(struct imx8qm_domain *dom,
                                              const struct dt_device_node *np)
{
    struct dt_device_node *rsrc_node, *pd = NULL;
    u32 resource_id[SC_R_POWER_DOMAIN_LAST];

    rsrc_node = (struct dt_device_node *)np;
    while ( rsrc_node )
    {
        int i, cnt;

        cnt = get_rsrc_from_pd(rsrc_node, &pd, resource_id,
                               ARRAY_SIZE(resource_id));
        ASSERT(cnt <= ARRAY_SIZE(resource_id));
        if ( cnt < 0 )
            return cnt;
        for ( i = 0; i < cnt; i++ )
        {
            int ret;

            ret = clb_passthrough_assign_resource(dom, resource_id[i]);
            if ( ret < 0 )
            {
                ret = sc_err_to_posix(ret);
                printk(XENLOG_DEBUG "Failed to assign %d (%s) to domain id %d\n",
                       resource_id[i], np->full_name, dom->domain_id);
                return ret;
            }
#ifdef IMX8QM_PLAT_DEBUG
            printk(XENLOG_DEBUG "Assign %d (%s) to domain id %d\n",
                   resource_id[i], np->full_name, dom->domain_id);
#endif
        }
        rsrc_node = pd;
    }

    return 0;
}

static int handle_passthrough_dtdev(struct imx8qm_domain *dom, struct dt_device_node *np)
{
    struct dt_device_node *child;
    int ret;

#ifdef IMX8QM_PLAT_DEBUG
    printk(XENLOG_DEBUG "Find resources from node %s for domid %d\n",
           np->full_name, dom->domain_id);
#endif

    ret = passthrough_dtdev_add_resources(dom, np, "fsl,sc_init_on_rsrc_id",
                                          clb_passthrough_power_on_resource);
    if ( ret < 0 )
        return ret;

    ret = passthrough_dtdev_add_resources(dom, np, "fsl,sc_always_on_rsrc_id",
                                          clb_passthrough_add_always_on);
    if ( ret < 0 )
        return ret;

    ret = passthrough_dtdev_add_resources(dom, np, "fsl,sc_rsrc_id",
                                          clb_passthrough_assign_resource);
    if ( ret < 0 )
        return ret;

    ret = passthrough_dtdev_add_resources_pd(dom, np);
    if ( ret < 0 )
        return ret;

    ret = passthrough_dtdev_add_resources(dom, np, "fsl,sc_pad_id",
                                          clb_passthrough_assign_pad);
    if ( ret < 0 )
        return ret;

    for ( child = np->child; child != NULL; child = child->sibling )
    {
        ret = handle_passthrough_dtdev(dom, child);
        if ( ret )
            return ret;
    }

    return 0;
}

int imx8qm_do_domctl(struct xen_domctl *domctl, struct domain *d,
                     XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    int ret = -ENOSYS;

    switch ( domctl->cmd )
    {
    case XEN_DOMCTL_platform:
        {
            struct xen_domctl_platform *op = &domctl->u.domctl_platform;

            switch ( op->cmd )
            {
            /*
             * N.B. XEN_DOMCTL_PLATFORM_OP_PASSTHROUGH_DTDEV can be called
             * multiple times for the same device tree node. This happens
             * when the toolstack re-creates domain device tree due to
             * its resize, e.g. when allocated device tree cannot hold
             * all the nodes to be copied.
             */
            case XEN_DOMCTL_PLATFORM_OP_PASSTHROUGH_DTDEV:
                {
                    char *path;
                    struct dt_device_node *np;
                    struct imx8qm_domain * dom = (struct imx8qm_domain *)d->arch.plat_priv;
                    domid_t domid = d->domain_id;

                    path = safe_copy_string_from_guest(op->u.passthrough_dtdev.path,
                                                       op->u.passthrough_dtdev.size,
                                                       PAGE_SIZE);
                    if ( IS_ERR(path) )
                    {
                        ret = PTR_ERR(path);
                        break;
                    }
                    /*
                     * Some of the devices describe their resources via 'fsl,sc_rsrc_id' arrays,
                     * but some of them rely on 'power-domains' property, which means those resource
                     * IDs are taken from '/imx8qm-pm' node. So, ideally, only relevant nodes of the
                     * '/imx8qm-pm' should be copied to the domain's device tree. But for simplicity,
                     * guests copy the node as is with the resources they do not own.
                     * Thus, parsing '/imx8qm-pm' here for resources results in some of the resources
                     * are simultaneously assigned to multiple domains. To fix that, skip parsing this
                     * node, but hold it in the device tree, so we can reference resources via
                     * 'power-domains'.
                     */
                    if ( !strcmp(path, "/imx8qm-pm") )
                    {
                        printk(XENLOG_DEBUG "Skip device %s for domid %d\n",
                               path,domid);
                        xfree(path);
                        ret = 0;
                        break;
                    }

#ifdef IMX8QM_PLAT_DEBUG
                    printk(XENLOG_DEBUG "Passthrough device %s for domid %d\n",
                           path, domid);
#endif

                    np = dt_find_node_by_path(path);
                    if ( !np )
                    {
                        printk(XENLOG_ERR "Passthrough device %s not found for domid %d\n",
                               path, domid);
                        ret = -EINVAL;
                        break;
                    }

                    ret = handle_passthrough_dtdev(dom, np);
                    xfree(path);
                    break;
                }
            default:
                ret = -EINVAL;
                break;
            }
        }
        break;

    default:
        break;
    }

    return ret;
}

PLATFORM_START(imx8qm, "i.MX 8")
    .compatible = imx8qm_dt_compat,
    .init = imx8qm_system_init,
    .specific_mapping = imx8qm_specific_mapping,
    .reset = imx8qm_system_reset,
    .poweroff = imx8qm_system_off,
    .smc = imx8qm_smc,
    .handle_hvc = imx8qm_handle_hvc,
    .domain_destroy = imx8qm_domain_destroy,
    .domain_create = imx8qm_domain_create,
    .do_domctl = imx8qm_do_domctl,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
