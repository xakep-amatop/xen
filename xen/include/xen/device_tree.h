/*
 * Device Tree
 *
 * Copyright (C) 2012 Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __XEN_DEVICE_TREE_H__
#define __XEN_DEVICE_TREE_H__

#include <xen/byteorder.h>

#include <asm/device.h>
#include <public/xen.h>
#include <public/device_tree_defs.h>
#include <xen/bug.h>
#include <xen/kernel.h>
#include <xen/string.h>
#include <xen/types.h>
#include <xen/list.h>
#include <xen/rwlock.h>

/*
 * Struct used for matching a device
 */
struct dt_device_match {
    const char *path;
    const char *type;
    const char *compatible;
    const bool not_available;
    /*
     * Property name to search for. We only search for the property's
     * existence.
     */
    const char *prop;
    const void *data;
};

#define __DT_MATCH_PATH(p)              .path = (p)
#define __DT_MATCH_TYPE(typ)            .type = (typ)
#define __DT_MATCH_COMPATIBLE(compat)   .compatible = (compat)
#define __DT_MATCH_NOT_AVAILABLE()      .not_available = 1
#define __DT_MATCH_PROP(p)              .prop = (p)

#define DT_MATCH_PATH(p)                { __DT_MATCH_PATH(p) }
#define DT_MATCH_TYPE(typ)              { __DT_MATCH_TYPE(typ) }
#define DT_MATCH_COMPATIBLE(compat)     { __DT_MATCH_COMPATIBLE(compat) }
#define DT_MATCH_NOT_AVAILABLE()        { __DT_MATCH_NOT_AVAILABLE() }
#define DT_MATCH_PROP(p)                { __DT_MATCH_PROP(p) }

typedef u32 dt_phandle;

/**
 * dt_property - describe a property for a device
 * @name: name of the property
 * @length: size of the value
 * @value: pointer to data contained in the property
 * @next: pointer to the next property of a specific node
 */
struct dt_property {
    const char *name;
    u32 length;
    void *value;
    struct dt_property *next;
};

/**
 * dt_device_node - describe a node in the device tree
 * @name: name of the node
 * @type: type of the node (ie: memory, cpu, ...)
 * @full_name: full name, it's composed of all the ascendant name separate by /
 * @used_by: who owns the node? (ie: xen, dom0...)
 * @properties: list of properties for the node
 * @child: pointer to the first child
 * @sibling: pointer to the next sibling
 * @allnext: pointer to the next in list of all nodes
 */
struct dt_device_node {
    const char *name;
    const char *type;
    char *full_name;
    dt_phandle phandle;
    domid_t used_by; /* By default it's used by dom0 */

    /* IOMMU specific fields */
    bool is_protected;

#ifdef CONFIG_STATIC_EVTCHN
    /* HACK: Remove this if there is a need of space */
    bool static_evtchn_created;
#endif

    struct dt_property *properties;
    struct dt_device_node *parent;
    struct dt_device_node *child;
    struct dt_device_node *sibling;
    struct dt_device_node *next; /* TODO: Remove it. Only use to know the last children */
    struct dt_device_node *allnext;

    /*
     * The main purpose of this list is to link the structure in the list
     * of devices assigned to domain.
     *
     * Boot code (iommu_hardware_setup) re-uses this list to link the structure
     * in the list of devices for which driver requested deferred probing.
     */
    struct list_head domain_list;

#ifdef CONFIG_HAS_DEVICE_TREE_DISCOVERY
    struct device dev;
#endif /* CONFIG_HAS_DEVICE_TREE_DISCOVERY */
};

#ifdef CONFIG_HAS_DEVICE_TREE_DISCOVERY
#define dt_to_dev(dt_node)  (&(dt_node)->dev)

static inline struct dt_device_node *dev_to_dt(struct device *dev)
{
    ASSERT(dev->type == DEV_DT);

    return container_of(dev, struct dt_device_node, dev);
}
#endif /* CONFIG_HAS_DEVICE_TREE_DISCOVERY */

#define MAX_PHANDLE_ARGS 16
struct dt_phandle_args {
    struct dt_device_node *np;
    int args_count;
    uint32_t args[MAX_PHANDLE_ARGS];
};

/**
 * dt_irq - describe an IRQ in the device tree
 * @irq: IRQ number
 * @type: IRQ type (see DT_IRQ_TYPE_*)
 *
 * This structure is returned when an interrupt is mapped.
 */
struct dt_irq {
    unsigned int irq;
    unsigned int type;
};

/* If type == DT_IRQ_TYPE_NONE, assume we use level triggered */
static inline bool dt_irq_is_level_triggered(const struct dt_irq *irq)
{
    unsigned int type = irq->type;

    return (type & DT_IRQ_TYPE_LEVEL_MASK) || (type == DT_IRQ_TYPE_NONE);
}

/**
 * dt_raw_irq - container for device_node/irq_specifier for an irq controller
 * @controller: pointer to interrupt controller deivce tree node
 * @size: size of interrupt specifier
 * @specifier: array of cells @size long specifying the specific interrupt
 *
 * This structure is returned when an interrupt is mapped but not translated.
 */
#define DT_MAX_IRQ_SPEC     4 /* We handle specifiers of at most 4 cells */
struct dt_raw_irq {
    const struct dt_device_node *controller;
    u32 size;
    u32 specifier[DT_MAX_IRQ_SPEC];
};

extern const void *device_tree_flattened;

/**
 * dt_unflatten_host_device_tree - Unflatten the host device tree
 *
 * Create a hierarchical device tree for the host DTB to be able
 * to retrieve parents.
 */
void dt_unflatten_host_device_tree(void);

/**
 * unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens a device-tree, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 * @fdt: The fdt to expand
 * @mynodes: The device_node tree created by the call
 *
 * Returns 0 on success and a negative number on error
 */
int unflatten_device_tree(const void *fdt, struct dt_device_node **mynodes);

/**
 * IRQ translation callback
 * TODO: For the moment we assume that we only have ONE
 * interrupt-controller.
 */
typedef int (*dt_irq_xlate_func)(const u32 *intspec, unsigned int intsize,
                                 unsigned int *out_hwirq,
                                 unsigned int *out_type);
extern dt_irq_xlate_func dt_irq_xlate;

/**
 * Host device tree
 * DO NOT modify it!
 */
extern struct dt_device_node *dt_host;

/**
 * Primary interrupt controller
 * Exynos SOC has an interrupt combiner, interrupt has no physical
 * meaning when it's not connected to the primary controller.
 * We will only map interrupt whose parent controller is
 * dt_interrupt_controller. It should always be a GIC.
 * TODO: Handle multiple GIC
 */
extern const struct dt_device_node *dt_interrupt_controller;

/*
 * Lock that protects r/w updates to unflattened device tree i.e. dt_host during
 * runtime. Lock may not be taken for boot only code.
 */
extern rwlock_t dt_host_lock;

/**
 * Find the interrupt controller
 * For the moment we handle only one interrupt controller: the first
 * one without parent which is compatible with the string "compat".
 *
 * If found, return the interrupt controller device node.
 */
struct dt_device_node *
dt_find_interrupt_controller(const struct dt_device_match *matches);

void intc_dt_preinit(void);

#define dt_prop_cmp(s1, s2) strcmp((s1), (s2))
#define dt_node_cmp(s1, s2) strcasecmp((s1), (s2))
#define dt_compat_cmp(s1, s2) strcasecmp((s1), (s2))

#define dt_for_each_property_node(dn, pp)                   \
    for ( pp = (dn)->properties; (pp) != NULL; pp = (pp)->next )

#define dt_for_each_device_node(dt, dn)                     \
    for ( dn = (dt); (dn) != NULL; dn = (dn)->allnext )

#define dt_for_each_child_node(dt, dn)                      \
    for ( dn = (dt)->child; (dn) != NULL; dn = (dn)->sibling )

/* Helper to convert a number of cells to bytes */
static inline int dt_cells_to_size(int size)
{
    return (size * sizeof (u32));
}

/* Helper to convert a number of bytes to cells, rounds down */
static inline int dt_size_to_cells(int bytes)
{
    return (bytes / sizeof(u32));
}

static inline const char *dt_node_full_name(const struct dt_device_node *np)
{
    return (np && np->full_name) ? np->full_name : "<no-node>";
}

static inline const char *dt_node_name(const struct dt_device_node *np)
{
    return (np && np->name) ? np->name : "<no-node>";
}

static inline bool dt_node_name_is_equal(const struct dt_device_node *np,
                                         const char *name)
{
    return !dt_node_cmp(np->name, name);
}

static inline bool dt_node_path_is_equal(const struct dt_device_node *np,
                                         const char *path)
{
    return !dt_node_cmp(np->full_name, path);
}

static inline bool
dt_device_type_is_equal(const struct dt_device_node *device,
                        const char *type)
{
    return !dt_node_cmp(device->type, type);
}

static inline void dt_device_set_used_by(struct dt_device_node *device,
                                         domid_t used_by)
{
    /* TODO: children must inherit to the used_by thing */
    device->used_by = used_by;
}

static inline domid_t dt_device_used_by(const struct dt_device_node *device)
{
    return device->used_by;
}

static inline void dt_device_set_protected(struct dt_device_node *device)
{
    device->is_protected = true;
}

static inline bool dt_device_is_protected(const struct dt_device_node *device)
{
    return device->is_protected;
}

static inline bool dt_property_name_is_equal(const struct dt_property *pp,
                                             const char *name)
{
    return !dt_prop_cmp(pp->name, name);
}

#ifdef CONFIG_STATIC_EVTCHN
static inline void
dt_device_set_static_evtchn_created(struct dt_device_node *device)
{
    device->static_evtchn_created = true;
}

static inline bool
dt_device_static_evtchn_created(const struct dt_device_node *device)
{
    return device->static_evtchn_created;
}
#endif /* CONFIG_STATIC_EVTCHN */

/**
 * dt_find_compatible_node - Find a node based on type and one of the
 *                           tokens in its "compatible" property
 * @from: The node to start searching from or NULL, the node
 *          you pass will not be searched, only the next one
 *          will; typically, you pass what the previous call
 *          returned.
 * @type: The type string to match "device_type" or NULL to ignore
 * @compatible: The string to match to one of the tokens in the device
 *          "compatible" list.
 *
 * Returns a node pointer.
 */
struct dt_device_node *dt_find_compatible_node(struct dt_device_node *from,
                                               const char *type,
                                               const char *compatible);

/**
 * Find a property with a given name for a given node
 * and return the value.
 */
const void *dt_get_property(const struct dt_device_node *np,
                            const char *name, u32 *lenp);

const struct dt_property *dt_find_property(const struct dt_device_node *np,
                                           const char *name, u32 *lenp);


/**
 * dt_property_read_u32 - Helper to read a u32 property.
 * @np: node to get the value
 * @name: name of the property
 * @out_value: pointer to return value
 *
 * Return true if get the desired value.
 */
bool dt_property_read_u32(const struct dt_device_node *np,
                          const char *name, u32 *out_value);
/**
 * dt_property_read_u64 - Helper to read a u64 property.
 * @np: node to get the value
 * @name: name of the property
 * @out_value: pointer to return value
 *
 * Return true if get the desired value.
 */
bool dt_property_read_u64(const struct dt_device_node *np,
                          const char *name, u64 *out_value);


/**
 * dt_property_read_variable_u32_array - Find and read an array of 32 bit
 * integers from a property, with bounds on the minimum and maximum array size.
 *
 * @np:     device node from which the property value is to be read.
 * @propname:   name of the property to be searched.
 * @out_values: pointer to return found values.
 * @sz_min: minimum number of array elements to read
 * @sz_max: maximum number of array elements to read, if zero there is no
 *      upper limit on the number of elements in the dts entry but only
 *      sz_min will be read.
 *
 * Search for a property in a device node and read 32-bit value(s) from
 * it.
 *
 * Return: The number of elements read on success, -EINVAL if the property
 * does not exist, -ENODATA if property does not have a value, and -EOVERFLOW
 * if the property data is smaller than sz_min or longer than sz_max.
 *
 * The out_values is modified only if a valid u32 value can be decoded.
 */
int dt_property_read_variable_u32_array(const struct dt_device_node *np,
                                        const char *propname, u32 *out_values,
                                        size_t sz_min, size_t sz_max);

/**
 * dt_property_read_u32_array - Find and read an array of 32 bit integers
 * from a property.
 *
 * @np:     device node from which the property value is to be read.
 * @propname:   name of the property to be searched.
 * @out_values: pointer to return value, modified only if return value is 0.
 * @sz:     number of array elements to read
 *
 * Search for a property in a device node and read 32-bit value(s) from
 * it.
 *
 * Return: 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_values is modified only if a valid u32 value can be decoded.
 */
static inline int dt_property_read_u32_array(const struct dt_device_node *np,
                                             const char *propname,
                                             u32 *out_values, size_t sz)
{
    int ret = dt_property_read_variable_u32_array(np, propname, out_values,
                              sz, 0);
    if ( ret >= 0 )
        return 0;
    else
        return ret;
}

/**
 * dt_property_read_bool - Check if a property exists
 * @np: node to get the value
 * @name: name of the property
 *
 * Search for a property in a device node.
 * Return true if the property exists false otherwise.
 */
static inline bool dt_property_read_bool(const struct dt_device_node *np,
                                         const char *name)
{
    const struct dt_property *prop = dt_find_property(np, name, NULL);

    return prop ? true : false;
}

/**
 * dt_property_read_string - Find and read a string from a property
 * @np:         Device node from which the property value is to be read
 * @propname:   Name of the property to be searched
 * @out_string: Pointer to null terminated return string, modified only
 *              if return value if 0.
 *
 * Search for a property in a device tree node and retrieve a null
 * terminated string value (pointer to data, not a copy). Returns 0 on
 * success, -EINVAL if the property does not exist, -ENODATA if property
 * doest not have value, and -EILSEQ if the string is not
 * null-terminated with the length of the property data.
 *
 * Note that the empty string "" has length of 1, thus -ENODATA cannot
 * be interpreted as an empty string.
 *
 * The out_string pointer is modified only if a valid string can be decoded.
 */
int dt_property_read_string(const struct dt_device_node *np,
                            const char *propname, const char **out_string);

/**
 * dt_property_match_string() - Find string in a list and return index
 * @np: pointer to node containing string list property
 * @propname: string list property name
 * @string: pointer to string to search for in string list
 *
 * This function searches a string list property and returns the index
 * of a specific string value.
 */
int dt_property_match_string(const struct dt_device_node *np,
                             const char *propname, const char *string);

/**
 * Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
bool dt_device_is_compatible(const struct dt_device_node *device,
                             const char *compat);

/**
 * dt_machine_is_compatible - Test root of device tree for a given compatible value
 * @compat: compatible string to look for in root node's compatible property.
 *
 * Returns true if the root node has the given value in its
 * compatible property.
 */
bool dt_machine_is_compatible(const char *compat);

/**
 * dt_find_node_by_name - Find a node by its "name" property
 * @from: The node to start searching from or NULL, the node
 * you pass will not be searched, only the next one
 *  will; typically, you pass what the previous call
 *  returned. of_node_put() will be called on it
 * @name: The name string to match against
 *
 * Returns a node pointer with refcount incremented, use
 * of_node_put() on it when done.
 */
struct dt_device_node *dt_find_node_by_name(struct dt_device_node *from,
                                            const char *name);

/**
 * dt_find_node_by_type - Find a node by its "type" property
 */
struct dt_device_node *dt_find_node_by_type(struct dt_device_node *from,
                                            const char *type);

/**
 * df_find_node_by_alias - Find a node matching an alias
 * @alias: The alias to match
 *
 * Returns a node pointer.
 */
struct dt_device_node *dt_find_node_by_alias(const char *alias);

/**
 * dt_find_node_by_path_from - Generic function to find a node matching the
 * full DT path for any given unflatten device tree
 * @from: The device tree node to start searching from
 * @path: The full path to match
 *
 * Returns a node pointer.
 */
struct dt_device_node *dt_find_node_by_path_from(struct dt_device_node *from,
                                                 const char *path);

/**
 * dt_find_node_by_path - Find a node matching a full DT path in dt_host
 * @path: The full path to match
 *
 * Returns a node pointer.
 */
static inline struct dt_device_node *dt_find_node_by_path(const char *path)
{
    return dt_find_node_by_path_from(dt_host, path);
}

/**
 * dt_find_node_by_gpath - Same as dt_find_node_by_path but retrieve the
 * path from the guest
 *
 * @u_path: Xen Guest handle to the buffer containing the path
 * @u_plen: Length of the buffer
 * @node: TODO
 *
 * Return 0 if succeed otherwise -errno
 */
int dt_find_node_by_gpath(XEN_GUEST_HANDLE(char) u_path, uint32_t u_plen,
                          struct dt_device_node **node);

/**
 * dt_get_parent - Get a node's parent if any
 * @node: Node to get parent
 *
 * Returns a node pointer.
 */
const struct dt_device_node *dt_get_parent(const struct dt_device_node *node);

/**
 * dt_device_get_paddr - Resolve an address for a device
 * @device: the device whose address is to be resolved
 * @index: index of the address to resolve
 * @addr: address filled by this function
 * @size: size filled by this function
 *
 * This function resolves an address, walking the tree, for a given
 * device-tree node. It returns 0 on success.
 */
int dt_device_get_paddr(const struct dt_device_node *dev, unsigned int index,
                        paddr_t *addr, paddr_t *size);

/**
 * dt_device_get_address - Resolve an address for a device
 * @device: the device whose address is to be resolved
 * @index: index of the address to resolve
 * @addr: address filled by this function
 * @size: size filled by this function
 *
 * This function resolves an address, walking the tree, for a give
 * device-tree node. It returns 0 on success.
 */
int dt_device_get_address(const struct dt_device_node *dev, unsigned int index,
                          u64 *addr, u64 *size);

/**
 * dt_number_of_irq - Get the number of IRQ for a device
 * @device: the device whose number of interrupt is to be retrieved
 *
 * Return the number of irq for this device or 0 if there is no
 * interrupt or an error occurred.
 */
unsigned int dt_number_of_irq(const struct dt_device_node *device);

/**
 * dt_number_of_address - Get the number of addresses for a device
 * @dev: the device whose number of address is to be retrieved
 *
 * Return the number of address for this device or 0 if there is no
 * address or an error occurred.
 */
unsigned int dt_number_of_address(const struct dt_device_node *dev);

/**
 * dt_device_get_irq - Resolve an interrupt for a device
 * @device: the device whose interrupt is to be resolved
 * @index: index of the interrupt to resolve
 * @out_irq: structure dt_irq filled by this function
 *
 * This function resolves an interrupt, walking the tree, for a given
 * device-tree node. It's the high level pendant to dt_device_get_raw_irq().
 */
int dt_device_get_irq(const struct dt_device_node *device, unsigned int index,
                      struct dt_irq *out_irq);

/**
 * dt_device_get_raw_irq - Resolve an interrupt for a device without translation
 * @device: the device whose interrupt is to be resolved
 * @index: index of the interrupt to resolve
 * @out_irq: structure dt_raw_irq filled by this function
 *
 * This function resolves an interrupt for a device, no translation is
 * made. dt_irq_translate can be called after.
 */
int dt_device_get_raw_irq(const struct dt_device_node *device,
                          unsigned int index,
                          struct dt_raw_irq *out_irq);

/**
 * dt_irq_translate - Translate an irq
 * @raw: IRQ to translate (raw format)
 * @out_irq: structure dt_irq filled by this function
 */
int dt_irq_translate(const struct dt_raw_irq *raw, struct dt_irq *out_irq);

/**
 * dt_for_each_irq_map - Iterate over a nodes interrupt-map property
 * @dev: The node whose interrupt-map property should be iterated over
 * @cb: Call back to call for each entry
 * @data: Caller data passed to callback
 */
int dt_for_each_irq_map(const struct dt_device_node *dev,
                        int (*cb)(const struct dt_device_node *dev,
                                  const struct dt_irq *dt_irq,
                                  void *data),
                        void *data);

/**
 * dt_for_each_range - Iterate over a nodes ranges property
 * @dev: The node whose interrupt-map property should be iterated over
 * @cb: Call back to call for each entry
 * @data: Caller data passed to callback
 */
int dt_for_each_range(const struct dt_device_node *dev,
                      int (*cb)(const struct dt_device_node *dev,
                                uint64_t addr, uint64_t length,
                                void *data),
                      void *data);

/**
 * dt_n_size_cells - Helper to retrieve the number of cell for the size
 * @np: node to get the value
 *
 * This function retrieves for a give device-tree node the number of
 * cell for the size field.
 */
int dt_n_size_cells(const struct dt_device_node *np);

/**
 * dt_n_addr_cells - Helper to retrieve the number of cell for the address
 * @np: node to get the value
 *
 * This function retrieves for a give device-tree node the number of
 * cell for the address field.
 */
int dt_n_addr_cells(const struct dt_device_node *np);

/**
 * dt_child_n_size_cells - Helper to retrieve the number of cell for the size
 * @parent: parent of the child to get the value
 *
 * This function retrieves for a given device-tree node the number of
 * cell for the size field of there child
 */
int dt_child_n_size_cells(const struct dt_device_node *parent);

/**
 * dt_child_n_addr_cells - Helper to retrieve the number of cell for the
 * address
 * @parent: parent of the child to get the value
 *
 * This function retrieves for a given device-tree node the number of
 * cell for the address field of there child
 */
int dt_child_n_addr_cells(const struct dt_device_node *parent);

/**
 * dt_device_is_available - Check if a device is available for use
 *
 * @device: Node to check for availability
 *
 * Returns true if the status property is absent or set to "okay" or "ok",
 * false otherwise.
 */
bool dt_device_is_available(const struct dt_device_node *device);

/**
 * dt_device_for_passthrough - Check if a device will be used for
 * passthrough later
 *
 * @device: Node to check
 *
 * Return true if the property "xen,passthrough" is present in the node,
 * false otherwise.
 */
bool dt_device_for_passthrough(const struct dt_device_node *device);

/**
 * dt_match_node - Tell if a device_node has a matching of dt_device_match
 * @matches: array of dt_device_match structures to search in
 * @node: the dt_device_node structure to match against
 *
 * Returns true if the device node match one of dt_device_match.
 */
const struct dt_device_match *
dt_match_node(const struct dt_device_match *matches,
              const struct dt_device_node *node);

/**
 * dt_find_matching_node - Find a node based on an dt_device_match match table
 * @from: The node to start searching from or NULL, the node you pass
 *        will not be searched, only the next one will; typically, you pass
 *        what the returned call returned
 * @matches: array of dt_device_match structures to search in
 *
 * Returns a node pointer.
 */
struct dt_device_node *
dt_find_matching_node(struct dt_device_node *from,
                      const struct dt_device_match *matches);

/**
 * dt_set_cell - Write a value into a series of cells
 *
 * @cellp: Pointer to cells
 * @size: number of cells to write the value
 * @value: number to write
 *
 * Write a value into a series of cells and update cellp to point to the
 * cell just after.
 */
void dt_set_cell(__be32 **cellp, int size, u64 val);

/**
 * dt_set_range - Write range into a series of cells
 *
 * @cellp: Pointer to cells
 * @np: Node which contains the encoding for the address and the size
 * @address: Start of range
 * @size: Size of the range
 *
 * Write a range into a series of cells and update cellp to point to the
 * cell just after.
 */
void dt_set_range(__be32 **cellp, const struct dt_device_node *np,
                  u64 address, u64 size);

/**
 * dt_child_set_range - Write range into a series of cells
 *
 * @cellp: Pointer to cells
 * @parent: Parent node which contains the encode for the address and the size
 * @address: Start of range
 * @size: Size of the range
 *
 * Write a range into a series of cells and update cellp to point to the
 * cell just after.
 */
void dt_child_set_range(__be32 **cellp, int addrcells, int sizecells,
                        u64 address, u64 size);

/**
 * dt_get_range - Read a range (address/size) from a series of cells
 *
 * @cellp: Pointer to cells
 * @np Node which  contains the encoding for the addresss and the size
 * @address: Address filled by this function
 * @size: Size filled by this function
 *
 * WARNING: This function should not be used to decode an address
 * This function reads a range (address/size) from a series of cells and
 * update cellp to point to the cell just after.
 */
void dt_get_range(const __be32 **cellp, const struct dt_device_node *np,
                  u64 *address, u64 *size);

/**
 * dt_parse_phandle - Resolve a phandle property to a device_node pointer
 * @np: Pointer to device node holding phandle property
 * @phandle_name: Name of property holding a phandle value
 * @index: For properties holding a table of phandles, this is the index into
 *         the table
 *
 * Returns the device_node pointer.
 */
struct dt_device_node *dt_parse_phandle(const struct dt_device_node *np,
				                        const char *phandle_name,
                                        int index);

/**
 * dt_parse_phandle_with_args() - Find a node pointed by phandle in a list
 * @np:	pointer to a device tree node containing a list
 * @list_name: property name that contains a list
 * @cells_name: property name that specifies phandles' arguments count
 * @index: index of a phandle to parse out
 * @out_args: optional pointer to output arguments structure (will be filled)
 *
 * This function is useful to parse lists of phandles and their arguments.
 * Returns 0 on success and fills out_args, on error returns appropriate
 * errno value.
 *
 * Example:
 *
 * phandle1: node1 {
 * 	#list-cells = <2>;
 * }
 *
 * phandle2: node2 {
 * 	#list-cells = <1>;
 * }
 *
 * node3 {
 * 	list = <&phandle1 1 2 &phandle2 3>;
 * }
 *
 * To get a device_node of the `node2' node you may call this:
 * dt_parse_phandle_with_args(node3, "list", "#list-cells", 1, &args);
 */
int dt_parse_phandle_with_args(const struct dt_device_node *np,
                               const char *list_name,
                               const char *cells_name, int index,
                               struct dt_phandle_args *out_args);

/**
 * dt_count_phandle_with_args() - Find the number of phandles references in a property
 * @np: pointer to a device tree node containing a list
 * @list_name: property name that contains a list
 * @cells_name: property name that specifies phandles' arguments count
 *
 * Returns the number of phandle + argument tuples within a property. It
 * is a typical pattern to encode a list of phandle and variable
 * arguments into a single property. The number of arguments is encoded
 * by a property in the phandle-target node. For example, a gpios
 * property would contain a list of GPIO specifies consisting of a
 * phandle and 1 or more arguments. The number of arguments are
 * determined by the #gpio-cells property in the node pointed to by the
 * phandle.
 */
int dt_count_phandle_with_args(const struct dt_device_node *np,
                               const char *list_name,
                               const char *cells_name);

/**
 * dt_get_pci_domain_nr - Find the host bridge domain number
 *            of the given device node.
 * @node: Device tree node with the domain information.
 *
 * This function will try to obtain the host bridge domain number by finding
 * a property called "linux,pci-domain" of the given device node.
 *
 * Return:
 * * > 0    - On success, an associated domain number.
 * * -EINVAL    - The property "linux,pci-domain" does not exist.
 *
 * Returns the associated domain number from DT in the range [0-0xffff], or
 * a negative value if the required property is not found.
 */
int dt_get_pci_domain_nr(struct dt_device_node *node);

/**
 * dt_map_id - Translate an ID through a downstream mapping.
 * @np: root complex device node.
 * @id: device ID to map.
 * @map_name: property name of the map to use.
 * @map_mask_name: optional property name of the mask to use.
 * @target: optional pointer to a target device node.
 * @id_out: optional pointer to receive the translated ID.
 *
 * Given a device ID, look up the appropriate implementation-defined
 * platform ID and/or the target device which receives transactions on that
 * ID, as per the "iommu-map" and "msi-map" bindings. Either of @target or
 * @id_out may be NULL if only the other is required. If @target points to
 * a non-NULL device node pointer, only entries targeting that node will be
 * matched; if it points to a NULL value, it will receive the device node of
 * the first matching target phandle, with a reference held.
 *
 * Return: 0 on success or a standard error code on failure.
 */
int dt_map_id(const struct dt_device_node *np, uint32_t id,
              const char *map_name, const char *map_mask_name,
              struct dt_device_node **target, uint32_t *id_out);

struct dt_device_node *dt_find_node_by_phandle(dt_phandle handle);

#ifdef CONFIG_DEVICE_TREE_DEBUG
#define dt_dprintk(fmt, args...)  \
    printk(XENLOG_DEBUG fmt, ## args)
#else
static inline void
__attribute__ ((__format__ (__printf__, 1, 2)))
dt_dprintk(const char *fmt, ...) {}
#endif

#endif /* __XEN_DEVICE_TREE_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
