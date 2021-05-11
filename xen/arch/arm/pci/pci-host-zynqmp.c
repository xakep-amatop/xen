/*
 * Based on Linux drivers/pci/controller/pci-host-common.c
 * Based on Linux drivers/pci/controller/pci-host-generic.c
 * Based on xen/arch/arm/pci/pci-host-generic.c
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/init.h>
#include <xen/pci.h>
#include <asm/device.h>
#include <asm/pci.h>

#include "pci-emul-8139.h"

static int __init nwl_cfg_reg_index(struct dt_device_node *np)
{
    return dt_property_match_string(np, "reg-names", "cfg");
}

static int emul_config_read(struct pci_host_bridge *bridge, pci_sbdf_t sbdf,
                            uint32_t reg, uint32_t len, uint32_t *value)
{
    if ( r8139_conf_read(sbdf, reg, len, value) )
        return 0;

    return pci_generic_config_read(bridge, sbdf, reg, len, value);
}

static int emul_config_write(struct pci_host_bridge *bridge, pci_sbdf_t sbdf,
                             uint32_t reg, uint32_t len, uint32_t value)
{
    if ( r8139_conf_write(sbdf, reg, len, value) )
        return 0;

    return pci_generic_config_write(bridge, sbdf, reg, len, value);
}

/* ECAM ops */
const struct pci_ecam_ops nwl_pcie_ops = {
    .bus_shift  = 20,
    .cfg_reg_index = nwl_cfg_reg_index,
    .pci_ops    = {
        .map_bus                = pci_ecam_map_bus,
        .read                   = emul_config_read,
        .write                  = emul_config_write,
        .need_p2m_hwdom_mapping = pci_ecam_need_p2m_hwdom_mapping,
    }
};

static const struct dt_device_match __initconstrel nwl_pcie_dt_match[] =
{
    { .compatible = "xlnx,nwl-pcie-2.11" },
    { },
};

static int __init pci_host_generic_probe(struct dt_device_node *dev,
                                         const void *data)
{
    r8139_init(PCI_BDF(4, 0, 0));

    return PTR_RET(pci_host_common_probe(dev, &nwl_pcie_ops, NULL, 0));
}

DT_DEVICE_START(pci_gen, "PCI HOST ZYNQMP", DEVICE_PCI_HOSTBRIDGE)
.dt_match = nwl_pcie_dt_match,
.init = pci_host_generic_probe,
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
