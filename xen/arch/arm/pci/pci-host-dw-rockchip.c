/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/init.h>
#include <xen/pci.h>

#include <asm/device.h>
#include <asm/io.h>
#include <asm/pci.h>

#include "pci-designware.h"

struct rk_dw_pcie_priv {
    bool init_done;
};

static bool __init rk_dw_need_p2m_hwdom_mapping(struct domain *d,
                                                struct pci_host_bridge *bridge,
                                                uint64_t addr)
{
    struct pci_config_window *cfg = bridge->cfg;

    if ( addr == cfg->phys_addr )
        return true;

    return pci_ecam_need_p2m_hwdom_mapping(d, bridge, addr);
}

/*
 * PCI host bridges often have different ways to access the root and child
 * bus config spaces:
 *   "dbi"   : the aperture where root port's own configuration registers
 *             are available.
 *   "config": child's configuration space
 *   "atu"   : iATU registers for DWC version 4.80 or later
 */
static int __init rk_dw_cfg_reg_index(struct dt_device_node *np)
{
    return dt_property_match_string(np, "reg-names", "dbi");
}

static int __init rk_dw_child_cfg_reg_index(struct dt_device_node *np)
{
    return dt_property_match_string(np, "reg-names", "config");
}

/* ECAM ops */
const struct pci_ecam_ops rk_dw_pcie_ops = {
    .bus_shift  = 20,
    .cfg_reg_index = rk_dw_cfg_reg_index,
    .pci_ops    = {
        .map_bus                = pci_ecam_map_bus,
        .read                   = pci_generic_config_read,
        .write                  = pci_generic_config_write,
        .need_p2m_hwdom_mapping = rk_dw_need_p2m_hwdom_mapping,
        .init_bus_range         = pci_generic_init_bus_range,
    }
};

const struct pci_ecam_ops rk_dw_pcie_child_ops = {
    .bus_shift  = 20,
    .cfg_reg_index = rk_dw_child_cfg_reg_index,
    .pci_ops    = {
        .map_bus                = dw_pcie_child_map_bus,
        .read                   = dw_pcie_child_config_read,
        .write                  = dw_pcie_child_config_write,
        .need_p2m_hwdom_mapping = dw_pcie_child_need_p2m_hwdom_mapping,
        .init_bus_range         = pci_generic_init_bus_range_child,
    }
};

static const struct dt_device_match __initconstrel rk_dw_pcie_dt_match[] = {
    { .compatible = "rockchip,rk3588-pcie" },
    {},
};

static int __init rockchip_pcie_probe(struct dt_device_node *dev,
                                       const void *data)
{
    struct pci_host_bridge *bridge;

    struct rk_dw_pcie_priv *priv = xzalloc(struct rk_dw_pcie_priv);
    if ( !priv )
        return -ENOMEM;

    bridge = dw_pcie_host_probe(dev, data, &rk_dw_pcie_ops,
                                &rk_dw_pcie_child_ops);

    dw_pcie_set_priv(bridge, priv);

    return 0;
}

DT_DEVICE_START(pci_gen, "PCI HOST DW Rockchip", DEVICE_PCI_HOSTBRIDGE)
    .dt_match = rk_dw_pcie_dt_match,
    .init = rockchip_pcie_probe,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
