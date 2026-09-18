/* This file is intended to be included multiple times. */
/*#ifndef __XEN_PERFC_DEFN_H__*/
/*#define __XEN_PERFC_DEFN_H__*/

/*
 * Keep this file lightweight: avoid pulling in GIC headers through perfc.h.
 * ARM GIC has 16 SGIs.
 */
#ifndef NR_GIC_SGI
#define NR_GIC_SGI 16
#endif

PERFCOUNTER(invalid_hypercalls, "invalid hypercalls")

PERFCOUNTER(trap_wfi,      "trap: wfi")
PERFCOUNTER(trap_wfe,      "trap: wfe")
PERFCOUNTER(trap_cp15_32,  "trap: cp15 32-bit access")
PERFCOUNTER(trap_cp15_64,  "trap: cp15 64-bit access")
PERFCOUNTER(trap_cp14_32,  "trap: cp14 32-bit access")
PERFCOUNTER(trap_cp14_64,  "trap: cp14 64-bit access")
PERFCOUNTER(trap_cp14_dbg, "trap: cp14 dbg access")
PERFCOUNTER(trap_cp10,     "trap: cp10 access")
PERFCOUNTER(trap_cp,       "trap: cp access")
PERFCOUNTER(trap_smc32,    "trap: 32-bit smc")
PERFCOUNTER(trap_hvc32,    "trap: 32-bit hvc")
#ifdef CONFIG_ARM_64
PERFCOUNTER(trap_smc64,    "trap: 64-bit smc")
PERFCOUNTER(trap_hvc64,    "trap: 64-bit hvc")
PERFCOUNTER(trap_sysreg,   "trap: sysreg access")
#endif
PERFCOUNTER(trap_iabt,     "trap: guest instr abort")
PERFCOUNTER(trap_dabt,     "trap: guest data abort")
PERFCOUNTER(trap_uncond,   "trap: condition failed")

PERFCOUNTER(vpsci_cpu_on,              "vpsci: cpu_on")
PERFCOUNTER(vpsci_cpu_off,             "vpsci: cpu_off")
PERFCOUNTER(vpsci_version,             "vpsci: version")
PERFCOUNTER(vpsci_migrate_info_type,   "vpsci: migrate_info_type")
PERFCOUNTER(vpsci_system_off,          "vpsci: system_off")
PERFCOUNTER(vpsci_system_reset,        "vpsci: system_reset")
PERFCOUNTER(vpsci_cpu_suspend,         "vpsci: cpu_suspend")
PERFCOUNTER(vpsci_cpu_affinity_info,   "vpsci: cpu_affinity_info")
PERFCOUNTER(vpsci_features,            "vpsci: features")
PERFCOUNTER(vpsci_system_suspend,      "vpsci: system_suspend")

PERFCOUNTER(vcpu_kick,                 "vcpu: notify other vcpu")

PERFCOUNTER(vgicd_reads,                "vgicd: read")
PERFCOUNTER(vgicd_writes,               "vgicd: write")
PERFCOUNTER(vgicr_reads,                "vgicr: read")
PERFCOUNTER(vgicr_writes,               "vgicr: write")
PERFCOUNTER(vgic_cp64_reads,            "vgic: cp64 read")
PERFCOUNTER(vgic_cp64_writes,           "vgic: cp64 write")
PERFCOUNTER(vgic_sysreg_reads,          "vgic: sysreg read")
PERFCOUNTER(vgic_sysreg_writes,         "vgic: sysreg write")
PERFCOUNTER(vgic_sgi_list  ,            "vgic: SGI send to list")
PERFCOUNTER(vgic_sgi_others,            "vgic: SGI send to others")
PERFCOUNTER(vgic_sgi_self,              "vgic: SGI send to self")
PERFCOUNTER(vgic_irq_migrates,          "vgic: irq migration")
PERFCOUNTER_ARRAY(vgic_sgi_its,         "vgic: vSGI inject via ITS", NR_GIC_SGI)

PERFCOUNTER(vuart_reads,  "vuart: read")
PERFCOUNTER(vuart_writes, "vuart: write")

PERFCOUNTER(vtimer_cp32_reads,   "vtimer: cp32 read")
PERFCOUNTER(vtimer_cp32_writes,  "vtimer: cp32 write")

PERFCOUNTER(vtimer_cp64_reads,   "vtimer: cp64 read")
PERFCOUNTER(vtimer_cp64_writes,  "vtimer: cp64 write")

PERFCOUNTER(vtimer_sysreg_reads,  "vtimer: sysreg read")
PERFCOUNTER(vtimer_sysreg_writes, "vtimer: sysreg write")

PERFCOUNTER(vtimer_phys_inject,   "vtimer: phys expired, injected")
PERFCOUNTER(vtimer_phys_masked,   "vtimer: phys expired, masked")
PERFCOUNTER(vtimer_virt_inject,   "vtimer: virt expired, injected")

PERFCOUNTER(ppis,                 "#PPIs")
PERFCOUNTER(spis,                 "#SPIs")
PERFCOUNTER(guest_irqs,           "#GUEST-IRQS")
PERFCOUNTER(lpi_traps,            "LPI: traps")
PERFCOUNTER(lpi_doorbells,        "LPI: doorbell")

PERFCOUNTER(hyp_timer_irqs,   "Hypervisor timer interrupts")
PERFCOUNTER(virt_timer_irqs,  "Virtual timer interrupts")
PERFCOUNTER(maintenance_irqs, "Maintenance interrupts")

PERFCOUNTER(atomics_guest,    "atomics: guest access")
PERFCOUNTER(atomics_guest_paused,   "atomics: guest paused")

/* Optional diagnostics, gated by gic_bench_counters (default false). */
PERFCOUNTER(gb_sw_lpi_inject,      "GB: software LPI injection attempts")
PERFCOUNTER(gb_db_target_blocked,  "GB: doorbell target observed blocked")
PERFCOUNTER(gb_db_target_running,  "GB: doorbell target observed running")
PERFCOUNTER(gb_db_target_other,    "GB: doorbell target observed other")
PERFCOUNTER(gb_db_invalid_target,  "GB: doorbell invalid target")
PERFCOUNTER(gb_its_full_polls,     "GB: ITS queue full observations")
PERFCOUNTER(gb_its_full_timeout,   "GB: ITS queue full timeouts")
PERFCOUNTER(gb_its_drain_polls,    "GB: ITS queue nonempty observations")
PERFCOUNTER(gb_its_drain_timeout,  "GB: ITS queue drain timeouts")
PERFCOUNTER(gb_vpe_load_ok,        "GB: vPE resident commits")
PERFCOUNTER(gb_vpe_load_err,       "GB: vPE load errors")
PERFCOUNTER(gb_vpe_put_db,         "GB: vPE nonresident need_db true")
PERFCOUNTER(gb_vpe_put_nodb,       "GB: vPE nonresident need_db false")
PERFCOUNTER(gb_vpe_put_err,        "GB: vPE put errors")
PERFCOUNTER(gb_pendinglast_kick,   "GB: PendingLast kick calls")
PERFCOUNTER(gb_vpe_move_ok,        "GB: vPE VMOVP submissions")
PERFCOUNTER(gb_vpe_move_err,       "GB: vPE VMOVP failures")
PERFCOUNTER(gb_db_move_err,        "GB: doorbell retarget failures")
PERFCOUNTER(gb_dirty_busy_polls,   "GB: vPE Dirty busy observations")
PERFCOUNTER(gb_dirty_timeout,      "GB: vPE Dirty timeouts")
PERFCOUNTER(gb_valid_busy_polls,   "GB: vPE Valid busy observations")
PERFCOUNTER(gb_valid_timeout,      "GB: vPE Valid timeouts")
PERFCOUNTER(gb_syncr_busy_polls,   "GB: GICR SYNCR busy observations")
PERFCOUNTER(gb_syncr_timeout,      "GB: GICR SYNCR timeouts")

/*#endif*/ /* __XEN_PERFC_DEFN_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
