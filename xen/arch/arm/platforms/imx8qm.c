/*
 * xen/arch/arm/platforms/imx8qm.c
 *
 * i.MX 8QM setup
 *
 * Copyright (c) 2016 Freescale Inc.
 * Copyright 2018 NXP
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
#include <main/imx8qm_pads.h>
#include <xen/config.h>
#include <xen/lib.h>
#include <xen/vmap.h>
#include <xen/mm.h>
#include <xen/libfdt/libfdt.h>

#ifndef SC_P_LAST
#define SC_P_LAST (SC_P_COMP_CTL_GPIO_1V8_3V3_ENET_ENETA + 1)
#endif

static const char * const imx8qm_dt_compat[] __initconst =
{
    "fsl,imx8qm",
    NULL
};

/*
 * Used for non-privileged domain, the dom_name needs to be same in dom0 dts
 * and cfg file.
 */
struct imx8qm_rsrc_sid {
	uint32_t domain_id;
	uint32_t partition_id;
	uint32_t rsrc_index;
	struct dt_device_node *node[SC_R_LAST];
	uint32_t rsrc[SC_R_LAST];
	uint32_t sid[SC_R_LAST];
};

struct imx8qm_domain {
    domid_t domain_id;
    u32 partition_id;
    u32 partition_id_parent;
    char dom_name[256];
    u32 init_on_num_rsrc;
    u32 init_on_rsrcs[32];
    u32 num_rsrc;
    u32 rsrcs[SC_R_LAST];
    u32 num_pad;
    u32 pads[512];
};

#define QM_NUM_DOMAIN	8
/*
 * 8 user domains
 * TODo: use locks to protect the data, currently we only has 2 domains,
 * so it is ok.
 */
static struct imx8qm_domain imx8qm_doms[QM_NUM_DOMAIN];
static struct imx8qm_rsrc_sid rsrc_sid[QM_NUM_DOMAIN];

static u32 imx8qm_xen_mu_res_id;

/* Get the resource ID of the messaging unit used by Xen. */
int __init imx8_get_xen_mu_rsrc_id(void)
{
    struct dt_device_node *np;
    u32 mu_id = SC_R_MU_1A;

    np = dt_find_compatible_node(NULL, NULL, "fsl,imx8-mu");
    if (!np)
    {
        printk(XENLOG_ERR "No Xen MU entry defined in device tree\n");
        goto fail;
    }

    if (!dt_property_read_u32(np, "fsl,sc_rsrc_id", &mu_id))
        printk(XENLOG_ERR
               "No resource ID defined for Xen MU, assuming SC_R_MU_1A\n");
    else
        goto fail;

    return mu_id;

fail:
    printk(XENLOG_INFO "Using resource %d as Xen MU\n", mu_id);
    return mu_id;
}

static int imx8qm_system_init(void)
{
    struct dt_device_node *np = NULL;
    unsigned int i, rsrc_size;
    int ret;

    while ((np = dt_find_compatible_node(np, NULL, "xen,domu")))
    {
        for (i = 0; i < QM_NUM_DOMAIN; i++)
        {
	    /* 0 means unused, we ignore dom0 */
            if (!imx8qm_doms[i].domain_id)
                break;
	}
        if (i < QM_NUM_DOMAIN)
        {
            const __be32 *prop;
	    const char *name_str;
            prop = dt_get_property(np, "reg", NULL);
            if ( !prop )
                printk("No u-boot partition ID provided\n");
            else
            {
                imx8qm_doms[i].partition_id = fdt32_to_cpu(*prop);
                printk("partition id %d\n", fdt32_to_cpu(*prop));
            }
            ret = dt_property_read_string(np, "domain_name", &name_str);
	    if (ret)
            {
                printk("No name property\n");
                continue;
            }
            safe_strcpy(imx8qm_doms[i].dom_name, name_str);
	    printk("Domain name %s\n", imx8qm_doms[i].dom_name);

	    prop = dt_get_property(np, "init_on_rsrcs", &rsrc_size);
	    if (prop)
            {
                if (!dt_property_read_u32_array(np, "init_on_rsrcs",
                                                imx8qm_doms[i].init_on_rsrcs,
                                                rsrc_size >> 2))
                    panic("Reading init_on_rsrcs Error\n");
                imx8qm_doms[i].init_on_num_rsrc = rsrc_size >> 2;
	    }
	    prop = dt_get_property(np, "rsrcs", &rsrc_size);
	    if (prop)
            {
                if (!dt_property_read_u32_array(np, "rsrcs",
                                                imx8qm_doms[i].rsrcs,
                                                rsrc_size >> 2))
                    panic("Reading rsrcs Error\n");
                imx8qm_doms[i].num_rsrc = rsrc_size >> 2;
	    }

            if ( imx8qm_doms[i].rsrcs[0] == SC_R_ALL )
            {
                int k;

                for (k = 0; k < SC_R_LAST; k++)
                    imx8qm_doms[i].rsrcs[k] = k;
                imx8qm_doms[i].num_rsrc = k;
            }

	    prop = dt_get_property(np, "pads", &rsrc_size);
	    if (prop)
            {
                if (!dt_property_read_u32_array(np, "pads",
                                                imx8qm_doms[i].pads,
                                                rsrc_size >> 2))
                    panic("Reading rsrcs Error\n");
                imx8qm_doms[i].num_pad = rsrc_size >> 2;
            }

            if ( imx8qm_doms[i].pads[0] == SC_P_ALL )
            {
                int k;
                for (k = 0; k < SC_P_LAST; k++)
                    imx8qm_doms[i].pads[k] = k;
                imx8qm_doms[i].num_pad = k;
            }

            /* Mark this slot as occupied */
            imx8qm_doms[i].domain_id = DOMID_INVALID;
        }
    }

    imx8_mu_init();

    imx8qm_xen_mu_res_id = imx8_get_xen_mu_rsrc_id();

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

static void imx8qm_system_reset(void)
{
    int i;
    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        if (imx8qm_doms[i].partition_id)
            sc_rm_partition_free(mu_ipcHandle, imx8qm_doms[i].partition_id);
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

static int imx8qm_domd_move_resources(int domd_idx)
{
    sc_rm_pt_t parent_part, os_part;
    int i, j;
    sc_err_t sci_err;

    /*
     * DomD initially owns all the resources/pads, but we want it only
     * have the resources not assigned to other guest domains.
     * On DomD start, after its partition created, go over all guests'
     * resources and remove those from DomD in advance.
     */
    os_part = imx8qm_doms[domd_idx].partition_id;
    parent_part = imx8qm_doms[domd_idx].partition_id_parent;

    sci_err = sc_rm_set_resource_movable(mu_ipcHandle, SC_R_ALL, SC_R_ALL,
                                         SC_TRUE);
    if ( sci_err != SC_ERR_NONE )
    {
        printk(XENLOG_ERR "Failed to set all resources movable, err: %d\n",
               sci_err);
        return sci_err;
    }

    sci_err = sc_rm_set_pad_movable(mu_ipcHandle, SC_P_ALL, SC_P_ALL, SC_TRUE);
    if ( sci_err != SC_ERR_NONE )
    {
        printk(XENLOG_ERR "Failed to set all pads movable, err: %d\n", sci_err);
        return sci_err;
    }

    sci_err = sc_rm_set_resource_movable(mu_ipcHandle,
                                         imx8qm_xen_mu_res_id,
                                         imx8qm_xen_mu_res_id,
                                         SC_FALSE);
    if ( sci_err != SC_ERR_NONE )
    {
        printk(XENLOG_ERR
               "Failed to set Xen MU resource as not movable, err: %d\n",
               sci_err);
        return sci_err;
    }

    /* Do not move resources assigned for other domains */
    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        if ( i == domd_idx )
            continue;

        for (j = 0; j < imx8qm_doms[i].init_on_num_rsrc; j++)
        {
            sci_err = sc_rm_set_resource_movable(mu_ipcHandle,
                                                 imx8qm_doms[i].init_on_rsrcs[j],
                                                 imx8qm_doms[i].init_on_rsrcs[j],
                                                 SC_FALSE);
            if ( sci_err != SC_ERR_NONE )
            {
                printk(XENLOG_ERR
                       "Failed to set resource %d as not movable, err: %d\n",
                       imx8qm_doms[i].rsrcs[j], sci_err);
                return sci_err;
	    }
        }

        for (j = 0; j < imx8qm_doms[i].num_rsrc; j++)
        {
            sci_err = sc_rm_set_resource_movable(mu_ipcHandle,
                                                 imx8qm_doms[i].rsrcs[j],
                                                 imx8qm_doms[i].rsrcs[j],
                                                 SC_FALSE);
            if ( sci_err != SC_ERR_NONE )
            {
                printk(XENLOG_ERR
                       "Failed to set resource %d as not movable, err: %d\n",
                       imx8qm_doms[i].rsrcs[j], sci_err);
                return sci_err;
	    }
        }

        for (j = 0; j < imx8qm_doms[i].num_pad; j++)
        {
            sci_err = sc_rm_set_pad_movable(mu_ipcHandle,
                                            imx8qm_doms[i].pads[j],
                                            imx8qm_doms[i].pads[j],
                                            SC_FALSE);
            if ( sci_err != SC_ERR_NONE )
            {
                printk(XENLOG_ERR
                       "Failed to set pad %d as not movable, err: %d\n",
                       imx8qm_doms[i].pads[j], sci_err);
                return sci_err;
	    }
        }

    }

    /* Move all the resources and pads left to DomD */
    sci_err = sc_rm_move_all(mu_ipcHandle, parent_part, os_part,
                             SC_TRUE, SC_TRUE);
    if ( sci_err != SC_ERR_NONE )
        printk(XENLOG_ERR
               "Failed to move move all resources/pads, err: %d\n",
               sci_err);

    return sci_err;
}

static int imx8qm_domu_move_resources(int domu_idx)
{
    sc_rm_pt_t parent_part, os_part;
    int j;
    sc_err_t sci_err;

    os_part = imx8qm_doms[domu_idx].partition_id;
    parent_part = imx8qm_doms[domu_idx].partition_id_parent;

    for (j = 0; j < imx8qm_doms[domu_idx].init_on_num_rsrc; j++)
    {
        sci_err = sc_rm_set_resource_movable(mu_ipcHandle,
                                             imx8qm_doms[domu_idx].init_on_rsrcs[j],
                                             imx8qm_doms[domu_idx].init_on_rsrcs[j],
                                             SC_FALSE);
        if ( sci_err != SC_ERR_NONE )
        {
            printk(XENLOG_ERR
                   "Failed to set resource %d as not movable, err: %d\n",
                   imx8qm_doms[domu_idx].rsrcs[j], sci_err);
            return sci_err;
        }
    }

    for (j = 0; j < imx8qm_doms[domu_idx].num_rsrc; j++)
    {
        sci_err = sc_rm_set_resource_movable(mu_ipcHandle,
                                             imx8qm_doms[domu_idx].rsrcs[j],
                                             imx8qm_doms[domu_idx].rsrcs[j],
                                             SC_FALSE);
        if ( sci_err != SC_ERR_NONE )
        {
            printk(XENLOG_ERR
                   "Failed to set resource %d as not movable, err: %d\n",
                   imx8qm_doms[domu_idx].rsrcs[j], sci_err);
            return sci_err;
        }
    }

    for (j = 0; j < imx8qm_doms[domu_idx].num_pad; j++)
    {
        sci_err = sc_rm_set_pad_movable(mu_ipcHandle,
                                        imx8qm_doms[domu_idx].pads[j],
                                        imx8qm_doms[domu_idx].pads[j],
                                        SC_FALSE);
        if ( sci_err != SC_ERR_NONE )
        {
            printk(XENLOG_ERR
                   "Failed to set pad %d as not movable, err: %d\n",
                   imx8qm_doms[domu_idx].pads[j], sci_err);
            return sci_err;
        }
    }

    /* Move all the resources and pads left to DomD */
    sci_err = sc_rm_move_all(mu_ipcHandle, parent_part, os_part,
                             SC_TRUE, SC_TRUE);
    if ( sci_err != SC_ERR_NONE )
        printk(XENLOG_ERR
               "Failed to move move all resources/pads, err: %d\n",
               sci_err);

    return sci_err;
}

static int imx8qm_domain_create(struct domain *d,
                                struct xen_domctl_createdomain *config)
{
    unsigned int i, j;
    sc_err_t sci_err;
    int ret;

    /* No need for control domain */
    if (d->domain_id == 0)
        return 0;

    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        if (!strncmp(imx8qm_doms[i].dom_name, config->arch.dom_name, 256))
        {
            imx8qm_doms[i].domain_id = d->domain_id;
	    break;
	}
    }

    if (i == QM_NUM_DOMAIN)
    {
        printk("****************************************************\n");
        printk("NOT FOUND A entry to power off passthrough resources\n");
        printk("The dts node name needs to be same as name = \"xxx\"\n");
        printk("in vm configuration file\n");
        printk("****************************************************\n");

	return 0;
    }

    for (j = 0; j < imx8qm_doms[i].init_on_num_rsrc; j++)
    {
        if (is_control_domain(current->domain))
        {
            printk("Power on resource %d\n", imx8qm_doms[i].init_on_rsrcs[j]);
            sci_err = sc_pm_set_resource_power_mode(mu_ipcHandle, imx8qm_doms[i].init_on_rsrcs[j], SC_PM_PW_MODE_ON);
            if (sci_err != SC_ERR_NONE)
                printk("power on resource %d err: %d\n", imx8qm_doms[i].init_on_rsrcs[j], sci_err);

        }
    }

    /* Not control domain */
    if (dt_machine_is_compatible("fsl,imx8qm"))
    {
	sc_rm_pt_t parent_part, os_part;

	sci_err = sc_rm_get_partition(mu_ipcHandle, &parent_part);
	sci_err = sc_rm_partition_alloc(mu_ipcHandle, &os_part, false, false,
                                                    false, true, false);

	/* Overwrite uboot partition id */
	imx8qm_doms[i].partition_id = os_part;
	imx8qm_doms[i].partition_id_parent = parent_part;

	sci_err = sc_rm_set_parent(mu_ipcHandle, os_part, parent_part);

        printk(XENLOG_INFO
               "Assigning resources and pads for %s's partition %d, parent %d\n",
               imx8qm_doms[i].dom_name, os_part, parent_part);

        /* TODO: Find better way to understand that this is the driver domain. */
        if ( !strncmp(config->arch.dom_name, "DomD", 5) )
            ret = imx8qm_domd_move_resources(i);
        else
            ret = imx8qm_domu_move_resources(i);

        if ( ret )
            return ret;

        for (j = 0; j < imx8qm_doms[i].num_rsrc; j++)
        {
            if ( is_control_domain(current->domain) &&
                 sc_rm_is_resource_owned(mu_ipcHandle, imx8qm_doms[i].rsrcs[j]) &&
                 (imx8qm_doms[i].rsrcs[j] != imx8qm_xen_mu_res_id ) )
            {
                sci_err = sc_rm_assign_resource(mu_ipcHandle, os_part, imx8qm_doms[i].rsrcs[j]);
                if (sci_err != SC_ERR_NONE)
                        printk("assign resource error %d %d\n", imx8qm_doms[i].rsrcs[j], sci_err);
	    }
        }

        for (j = 0; j < imx8qm_doms[i].num_pad; j++)
        {
            if ( is_control_domain(current->domain) &&
                 sc_rm_is_pad_owned(mu_ipcHandle, imx8qm_doms[i].pads[j]) )
            {
                sci_err = sc_rm_assign_pad(mu_ipcHandle, os_part, imx8qm_doms[i].pads[j]);
		if (sci_err != SC_ERR_NONE)
			printk("assign pad error %d %d owned %d\n",
                               imx8qm_doms[i].pads[j], sci_err,
                               sc_rm_is_pad_owned(mu_ipcHandle,
                                                  imx8qm_doms[i].pads[j]));

	    }
        }
    }

    return 0;
}

static int imx8qm_domain_destroy(struct domain *d)
{
    unsigned int i;
    sc_err_t sci_err;
    /* No need for control domain */
    if (is_control_domain(d))
        return 0;

    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        /* Find out the related resources */
        if (imx8qm_doms[i].domain_id == d->domain_id)
        {
		printk("let's shutdown the domain resources\n");
	        printk("partition id %d; domain_id %d\n", imx8qm_doms[i].partition_id, d->domain_id);
		break;
	}
    }

    if (i == QM_NUM_DOMAIN)
        return 0;

    if (imx8qm_doms[i].partition_id)
    {
        sci_err = sc_pm_set_resource_power_mode_all(mu_ipcHandle, imx8qm_doms[i].partition_id, SC_PM_PW_MODE_OFF, SC_R_LAST);
        if (sci_err != SC_ERR_NONE)
    	    printk("off partition %d err %d\n", imx8qm_doms[i].partition_id, sci_err);

        sci_err = sc_rm_partition_free(mu_ipcHandle, imx8qm_doms[i].partition_id);
        if (sci_err != SC_ERR_NONE)
            printk("sc_rm_partition_free, err %d\n", sci_err);

    }

    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        if (rsrc_sid[i].domain_id == d->domain_id)
		memset(&rsrc_sid[i], 0, sizeof(rsrc_sid[0]));
    }

    return 0;
}

int platform_deassign_dev(struct domain *d, struct dt_device_node *dev)
{
    int i, j;
    sc_err_t sci_err;

    if (!dt_machine_is_compatible("fsl,imx8qm"))
    {
        return 0;
    }
    if (d->domain_id == 0)
	    return 0;

    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        /* Find out the related resources */
        if (imx8qm_doms[i].domain_id == d->domain_id)
        {
		break;
	}
    }

    if (i == QM_NUM_DOMAIN)
	    return 0;
    if (!imx8qm_doms[i].partition_id)
	    return 0;

    sci_err = sc_pm_set_resource_power_mode_all(mu_ipcHandle, imx8qm_doms[i].partition_id, SC_PM_PW_MODE_OFF, SC_R_LAST);
    if (sci_err != SC_ERR_NONE)
	    printk("off partition %d err %d\n", imx8qm_doms[i].partition_id, sci_err);

    printk("let's shutdown the domain resources\n");
    printk("partition id %d; domain_id %d\n", imx8qm_doms[i].partition_id, d->domain_id);

    imx8qm_doms[i].domain_id = 0xFFFF;

    if (imx8qm_doms[i].partition_id)
    {
        for (j = 0; j < imx8qm_doms[i].num_rsrc; j++)
        {
                sci_err = sc_rm_assign_resource(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, imx8qm_doms[i].rsrcs[j]);
		if (sci_err != SC_ERR_NONE)
			printk("assign resource error parent %d %d\n", imx8qm_doms[i].rsrcs[j], sci_err);
        }
	/*
	 * The following is only to remove SID for M4, in future need to develop new method
	 * to differetiate case without M4.
	 */
        sci_err = sc_rm_assign_resource(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, SC_R_M4_1_PID0);
	if (sci_err != SC_ERR_NONE)
		printk("assign resource error parent %d %d\n", SC_R_M4_1_PID0, sci_err);
        sci_err = sc_rm_assign_resource(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, SC_R_M4_1_PID1);
	if (sci_err != SC_ERR_NONE)
		printk("assign resource error parent %d %d\n", SC_R_M4_1_PID1, sci_err);
        sci_err = sc_rm_assign_resource(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, SC_R_M4_1_PID2);
	if (sci_err != SC_ERR_NONE)
		printk("assign resource error parent %d %d\n", SC_R_M4_1_PID2, sci_err);
        sci_err = sc_rm_assign_resource(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, SC_R_M4_1_PID3);
	if (sci_err != SC_ERR_NONE)
		printk("assign resource error parent %d %d\n", SC_R_M4_1_PID3, sci_err);
        sci_err = sc_rm_assign_resource(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, SC_R_M4_1_PID4);
	if (sci_err != SC_ERR_NONE)
		printk("assign resource error parent %d %d\n", SC_R_M4_1_PID4, sci_err);

        for (j = 0; j < imx8qm_doms[i].num_pad; j++)
        {
                sci_err = sc_rm_assign_pad(mu_ipcHandle, imx8qm_doms[i].partition_id_parent, imx8qm_doms[i].pads[j]);
		if (sci_err != SC_ERR_NONE)
			printk("assign pad error parent %d %d\n", imx8qm_doms[i].pads[j], sci_err);
        }

        sc_rm_partition_free(mu_ipcHandle, imx8qm_doms[i].partition_id);

        imx8qm_doms[i].partition_id = 0;
    }


    for (i = 0; i < QM_NUM_DOMAIN; i++)
    {
        if (rsrc_sid[i].domain_id == d->domain_id)
		memset(&rsrc_sid[i], 0, sizeof(rsrc_sid[0]));
    }

    return 0;
}

int platform_assign_dev(struct domain *d, u8 devfn, struct dt_device_node *dev, u32 flag)
{
    const __be32 *prop;
    uint32_t rsrcs[32]; 
    uint32_t rsrc_size;
    struct dt_device_node *smmu_np;
    struct dt_phandle_args masterspec;
    uint32_t i;
    sc_err_t sci_err;
    uint32_t index, rsrc_index;

    if (!dt_machine_is_compatible("fsl,imx8qm"))
    {
        return 0;
    }

    for (index = 0; index < QM_NUM_DOMAIN; index++)
    {
        if (rsrc_sid[index].domain_id == d->domain_id)
		break;
    }

    if (index == QM_NUM_DOMAIN)
    {
        for (index = 0; index < QM_NUM_DOMAIN; index++)
        {
            if (rsrc_sid[index].domain_id == 0)
            	break;
        }
    }

    if (index == QM_NUM_DOMAIN)
        return -1;

    rsrc_sid[index].domain_id = d->domain_id;
    rsrc_index = rsrc_sid[index].rsrc_index;

    smmu_np = dt_find_compatible_node(NULL, NULL, "arm,mmu-500");
    if (!smmu_np)
	    return 0;

    prop = dt_get_property(dev, "fsl,sc_rsrc_id", &rsrc_size);
    if (prop)
    {
        if (!dt_property_read_u32_array(dev, "fsl,sc_rsrc_id", rsrcs, rsrc_size >> 2))
        {
            printk("%s failed to get resource list\n", __func__);
	    return -1;
	}
    }
    else
    {
        struct dt_device_node *pnode = NULL;

        prop = dt_get_property(dev, "power-domains", NULL);
	if (!prop)
        {
            printk("%s no power domains\n", __func__);
            return -1;
	}
        pnode = dt_find_node_by_phandle(be32_to_cpup(prop));
	if (!pnode)
        {
            printk("%s no power domain node\n", __func__);
            return -1;
	}
	if (!dt_property_read_u32(pnode, "reg", rsrcs))
        {
            printk("%s no reg node\n", __func__);
            return -1;
	}
	rsrc_size = 4;
    }

    i = 0;
    while (!dt_parse_phandle_with_args(smmu_np, "mmu-masters",
                                       "#stream-id-cells", i, &masterspec))
    {
        if (masterspec.np == dev)
        {
            u16 streamid = masterspec.args[0];
	    int j;
            /* Only 1 SID supported on i.MX8QM */
	    for (j = 0; j < (rsrc_size >> 2); j++)
            {
                rsrc_sid[index].rsrc[rsrc_index] = rsrcs[j];
                rsrc_sid[index].sid[rsrc_index] = streamid;
                rsrc_sid[index].node[rsrc_index] = dev;
		rsrc_index++;
                sci_err = sc_rm_set_master_sid(mu_ipcHandle, rsrcs[j], streamid);
                if (sci_err != SC_ERR_NONE)
                    printk("set_master_sid resource %d sid 0x%x, err: %d\n", rsrcs[j], streamid, sci_err);
	    }
	}
        i++;
    }

    rsrc_sid[index].rsrc_index = rsrc_index;

    return 0;
}

PLATFORM_START(imx8qm, "i.MX 8")
    .compatible = imx8qm_dt_compat,
    .init = imx8qm_system_init,
    .specific_mapping = imx8qm_specific_mapping,
    .reset = imx8qm_system_reset,
    .poweroff = imx8qm_system_off,
    .smc = imx8qm_smc,
    .domain_destroy = imx8qm_domain_destroy,
    .domain_create = imx8qm_domain_create,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
