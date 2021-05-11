/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PCI_EMUL_H__
#define __PCI_EMUL_H__

#include <xen/pci.h>

int r8139_conf_read(pci_sbdf_t sbdf, int where, int size, u32 *value);
int r8139_conf_write(pci_sbdf_t sbdf, int where, int size, u32 value);

int r8139_init(uint16_t bdf);

#endif /* __PCI_EMUL_H__ */
