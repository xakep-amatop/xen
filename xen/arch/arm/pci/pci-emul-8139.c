// SPDX-License-Identifier: GPL-2.0
/*
 * Based on Linux drivers/pci/pci-device-emul.c
 */

#include <xen/kernel.h>
#include <xen/pci.h>

#include "pci-emul-8139.h"

/* Type 0 Configuration Space Header. */
struct r8139_emul_conf {
    __le16 vendor;
    __le16 device;
    __le16 command;
    __le16 status;
    __le32 class_revision;
    u8 cache_line_size;
    u8 latency_timer;
    u8 header_type;
    u8 bist;
    __le32 bar[6];
    __le32 cardbus_cis_ptr;
    __le16 subsystem_vendor_id;
    __le16 subsystem_id;
    __le32 romaddr;
    u8 capabilities_pointer;
    u8 reserved0[3];
    u8 reserved1[4];
    u8 intline;
    u8 intpin;
    u8 min_gnt;
    u8 max_lat;
};

struct r8139_emul;

typedef enum {
    R8139_EMUL_HANDLED,
    R8139_EMUL_NOT_HANDLED
} r8139_emul_read_status_t;

struct r8139_emul_ops {
    /*
     * Called when reading from the regular PCI device
     * configuration space. Return R8139_EMUL_HANDLED when the
     * operation has handled the read operation and filled in the
     * *value, or R8139_EMUL_NOT_HANDLED when the read should
     * be emulated by the common code by reading from the
     * in-memory copy of the configuration space.
     */
    r8139_emul_read_status_t (*read_base)(struct r8139_emul *device,
                                        int reg, u32 *value);

    /*
     * Same as ->read_base(), except it is for reading from the
     * PCIe capability configuration space.
     */
    r8139_emul_read_status_t (*read_pcie)(struct r8139_emul *device,
                                        int reg, u32 *value);
    /*
     * Called when writing to the regular PCI device configuration
     * space. old is the current value, new is the new value being
     * written, and mask indicates which parts of the value are
     * being changed.
     */
    void (*write_base)(struct r8139_emul *device, int reg,
                       u32 old, u32 new, u32 mask);

    /*
     * Same as ->write_base(), except it is for writing from the
     * PCIe capability configuration space.
     */
    void (*write_pcie)(struct r8139_emul *device, int reg,
                       u32 old, u32 new, u32 mask);
};

struct pci_device_reg_behavior;

struct r8139_emul {
    struct r8139_emul_conf conf;
    struct r8139_emul_ops *ops;
    struct pci_device_reg_behavior *pci_regs_behavior;
    struct pci_device_reg_behavior *pcie_cap_regs_behavior;
    void *data;
};

enum {
    R8139_EMUL_NO_PREFETCHABLE_BAR = BIT(0, UL),
};

#define PCI_STD_HEADER_SIZEOF    64

/* Error values that may be returned by PCI functions */
#define PCIBIOS_SUCCESSFUL              0x00
#define PCIBIOS_FUNC_NOT_SUPPORTED      0x81
#define PCIBIOS_BAD_VENDOR_ID           0x83
#define PCIBIOS_DEVICE_NOT_FOUND        0x86
#define PCIBIOS_BAD_REGISTER_NUMBER     0x87
#define PCIBIOS_SET_FAILED              0x88
#define PCIBIOS_BUFFER_TOO_SMALL        0x89

/**
 * lower_32_bits - return bits 0-31 of a number
 * @n: the number we're accessing
 */
#define lower_32_bits(n)        ((u32)((n) & 0xffffffff))

#define PCI_CLASS_NETWORK_ETHERNET      0x0200

#define PCI_DEVICE_CONF_END    PCI_STD_HEADER_SIZEOF

#define PCI_STATUS_ERROR_BITS (PCI_STATUS_DETECTED_PARITY | \
                   PCI_STATUS_SIG_SYSTEM_ERROR | \
                   PCI_STATUS_REC_MASTER_ABORT | \
                   PCI_STATUS_REC_TARGET_ABORT | \
                   PCI_STATUS_SIG_TARGET_ABORT | \
                   PCI_STATUS_PARITY)

/**
 * struct pci_device_reg_behavior - register bits behaviors
 * @ro:        Read-Only bits
 * @rw:        Read-Write bits
 * @w1c:    Write-1-to-Clear bits
 *
 * Reads and Writes will be filtered by specified behavior. All other bits not
 * declared are assumed 'Reserved' and will return 0 on reads, per PCIe 5.0:
 * "Reserved register fields must be read only and must return 0 (all 0's for
 * multi-bit fields) when read".
 */
struct pci_device_reg_behavior {
    /* Read-only bits */
    u32 ro;

    /* Read-write bits */
    u32 rw;

    /* Write-1-to-clear bits */
    u32 w1c;
};

static const
struct pci_device_reg_behavior pci_regs_behavior[PCI_STD_HEADER_SIZEOF / 4] = {
    [PCI_VENDOR_ID / 4] = { .ro = ~0 },
    [PCI_COMMAND / 4] = {
        .rw = (PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
               PCI_COMMAND_MASTER | PCI_COMMAND_PARITY |
               PCI_COMMAND_SERR),
        .ro = ((PCI_COMMAND_SPECIAL | PCI_COMMAND_INVALIDATE |
            PCI_COMMAND_VGA_PALETTE | PCI_COMMAND_WAIT |
            PCI_COMMAND_FAST_BACK) |
               (PCI_STATUS_CAP_LIST | PCI_STATUS_66MHZ |
            PCI_STATUS_FAST_BACK | PCI_STATUS_DEVSEL_MASK) << 16),
        .w1c = PCI_STATUS_ERROR_BITS << 16,
    },
    [PCI_CLASS_REVISION / 4] = { .ro = ~0 },

    /*
     * Cache Line Size register: implement as read-only, we do not
     * pretend implementing "Memory Write and Invalidate"
     * transactions"
     *
     * Latency Timer Register: implemented as read-only, as "A
     * device that is not capable of a burst transfer of more than
     * two data phases on its primary interface is permitted to
     * hardwire the Latency Timer to a value of 16 or less"
     *
     * Header Type: always read-only
     *
     * BIST register: implemented as read-only, as "A device that
     * does not support BIST must implement this register as a
     * read-only register that returns 0 when read"
     */
    [PCI_CACHE_LINE_SIZE / 4] = { .ro = ~0 },

    [PCI_BASE_ADDRESS_0 / 4] = {
        .rw = GENMASK(31, 8) | BIT(0, U),
    },

    [PCI_CAPABILITY_LIST / 4] = {
        .ro = GENMASK(7, 0),
    },

    [PCI_SUBSYSTEM_VENDOR_ID / 4] = { .ro = ~0 },
    [PCI_SUBSYSTEM_ID / 4]        = { .ro = ~0 },

    /*
     * Interrupt line (bits 7:0) are RW, interrupt pin (bits 15:8)
     * are RO, and device control (31:16) are a mix of RW, RO,
     * reserved and W1C bits
     */
    [PCI_INTERRUPT_LINE / 4] = {
        /* Interrupt line is RW */
        .rw = GENMASK(7, 0),

        /* Interrupt pin is RO */
        .ro = GENMASK(15, 8),

        .w1c = BIT(10, U) << 16,
    },
};

/*
 * Initialize a r8139_emul structure to represent a fake PCI
 * device configuration space. The caller needs to have initialized
 * the PCI configuration space with whatever values make sense
 * (typically at least vendor, device, revision), the ->ops pointer,
 * and optionally ->data and ->has_pcie.
 */
int r8139_emul_init(struct r8139_emul *device, unsigned int flags)
{
    BUILD_BUG_ON(sizeof(device->conf) != PCI_DEVICE_CONF_END);

    device->conf.class_revision |= cpu_to_le32(PCI_CLASS_NETWORK_ETHERNET << 16);
    device->conf.header_type = PCI_HEADER_TYPE_NORMAL;
    device->conf.cache_line_size = 0x10;
    device->conf.status = cpu_to_le16(PCI_STATUS_CAP_LIST);
    device->pci_regs_behavior = xmemdup_bytes(&pci_regs_behavior,
                                              sizeof(pci_regs_behavior));
    if (!device->pci_regs_behavior)
        return -ENOMEM;

    if (flags & R8139_EMUL_NO_PREFETCHABLE_BAR) {
        device->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].ro = ~0;
        device->pci_regs_behavior[PCI_PREF_MEMORY_BASE / 4].rw = 0;
    }

    return 0;
}

/*
 * Cleanup a r8139_emul structure that was previously initialized
 * using r8139_emul_init().
 */
void r8139_emul_cleanup(struct r8139_emul *device)
{
    xfree(device->pci_regs_behavior);
}

/*
 * Should be called by the PCI controller driver when reading the PCI
 * configuration space of the fake device. It will call back the
 * ->ops->read_base or ->ops->read_pcie operations.
 */
int r8139_emul_conf_read(struct r8139_emul *device, int where,
                  int size, u32 *value)
{
    int ret;
    int reg = where & ~3;
    r8139_emul_read_status_t (*read_op)(struct r8139_emul *device,
                         int reg, u32 *value);
    __le32 *cfgspace;
    const struct pci_device_reg_behavior *behavior;

    if (reg >= PCI_DEVICE_CONF_END) {
        *value = 0;
        return PCIBIOS_SUCCESSFUL;
    }

    read_op = device->ops->read_base;
    cfgspace = (__le32 *) &device->conf;
    behavior = device->pci_regs_behavior;

    if (read_op)
        ret = read_op(device, reg, value);
    else
        ret = R8139_EMUL_NOT_HANDLED;

    if (ret == R8139_EMUL_NOT_HANDLED)
        *value = le32_to_cpu(cfgspace[reg / 4]);

    /*
     * Make sure we never return any reserved bit with a value
     * different from 0.
     */
    *value &= behavior[reg / 4].ro | behavior[reg / 4].rw |
          behavior[reg / 4].w1c;

    if (size == 1)
        *value = (*value >> (8 * (where & 3))) & 0xff;
    else if (size == 2)
        *value = (*value >> (8 * (where & 3))) & 0xffff;
    else if (size != 4)
        return PCIBIOS_BAD_REGISTER_NUMBER;

    return PCIBIOS_SUCCESSFUL;
}

/*
 * Should be called by the PCI controller driver when writing the PCI
 * configuration space of the fake device. It will call back the
 * ->ops->write_base or ->ops->write_pcie operations.
 */
int r8139_emul_conf_write(struct r8139_emul *device, int where,
                   int size, u32 value)
{
    int reg = where & ~3;
    uint32_t mask, ret, old, new, shift;
    void (*write_op)(struct r8139_emul *device, int reg,
             u32 old, u32 new, u32 mask);
    __le32 *cfgspace;
    const struct pci_device_reg_behavior *behavior;

    shift = (where & 0x3) * 8;

    if (size == 4)
        mask = 0xffffffff;
    else if (size == 2)
        mask = 0xffff << shift;
    else if (size == 1)
        mask = 0xff << shift;
    else
        return PCIBIOS_BAD_REGISTER_NUMBER;

    ret = r8139_emul_conf_read(device, reg, 4, &old);
    if (ret != PCIBIOS_SUCCESSFUL)
        return ret;

    write_op = device->ops->write_base;
    cfgspace = (__le32 *) &device->conf;
    behavior = device->pci_regs_behavior;

    /* Keep all bits, except the RW bits */
    new = old & (~mask | ~behavior[reg / 4].rw);

    /* Update the value of the RW bits */
    new |= (value << shift) & (behavior[reg / 4].rw & mask);

    /* Clear the W1C bits */
    new &= ~((value << shift) & (behavior[reg / 4].w1c & mask));

    cfgspace[reg / 4] = cpu_to_le32(new);

    if (write_op)
        write_op(device, reg, old, new, mask);

    return PCIBIOS_SUCCESSFUL;
}

static struct r8139_emul_ops r8139_emul_ops = {
};

static struct r8139_emul emul_device;
static uint16_t emul_bdf;

int r8139_init(uint16_t bdf)
{
    struct r8139_emul *device = &emul_device;

    emul_bdf = bdf;
    /*
     * Realtek Semiconductor Co., Ltd.
     * RTL-8100/8101L/8139 PCI Fast Ethernet Adapter.
     */
    device->conf.vendor = 0x10ec;
    device->conf.device = 0x8139;

    /* Subsystem: Red Hat, Inc. QEMU Virtual Machine. */
    device->conf.subsystem_vendor_id = 0x1af4;
    device->conf.subsystem_id = 0x1100;

    device->ops = &r8139_emul_ops;

    r8139_emul_init(device, 0);
    return 0;
}

int r8139_conf_read(pci_sbdf_t sbdf, int where, int size, u32 *value)
{
    if ( emul_bdf != sbdf.bdf )
        return 0;

    r8139_emul_conf_read(&emul_device, where, size, value);
    return 1;
}

int r8139_conf_write(pci_sbdf_t sbdf, int where, int size, u32 value)
{
    if ( emul_bdf != sbdf.bdf )
        return 0;

    r8139_emul_conf_write(&emul_device, where, size, value);
    return 1;
}
