/* SPDX-License-Identifier: (GPL-2.0-or-later) */
/*
 * Copyright (c) 2021-2023 Vates SAS.
 *
 * Taken from xvisor, modified by Bobby Eshleman (bobby.eshleman@gmail.com).
 *
 * Taken/modified from Xvisor project with the following copyright:
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 */

#ifndef RISCV_SBI_H
#define RISCV_SBI_H

#include <xen/cpumask.h>
#include <xen/sections.h>

/* SBI-defined implementation ID */
#define SBI_XEN_IMPID 7

#define SBI_EXT_0_1_SET_TIMER               0x0
#define SBI_EXT_0_1_CONSOLE_PUTCHAR         0x1
#define SBI_EXT_0_1_CONSOLE_GETCHAR         0x2
#define SBI_EXT_0_1_CLEAR_IPI               0x3
#define SBI_EXT_0_1_SEND_IPI                0x4
#define SBI_EXT_0_1_REMOTE_FENCE_I          0x5
#define SBI_EXT_0_1_REMOTE_SFENCE_VMA       0x6
#define SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID  0x7
#define SBI_EXT_0_1_SHUTDOWN                0x8

#define SBI_EXT_BASE                    0x10
#define SBI_EXT_RFENCE                  0x52464E43
#define SBI_EXT_TIME                    0x54494D45
#define SBI_EXT_HSM                     0x48534D
#define SBI_EXT_IPI                     0x735049

/* SBI function IDs for BASE extension */
#define SBI_EXT_BASE_GET_SPEC_VERSION   0x0
#define SBI_EXT_BASE_GET_IMP_ID         0x1
#define SBI_EXT_BASE_GET_IMP_VERSION    0x2
#define SBI_EXT_BASE_PROBE_EXT          0x3
#define SBI_EXT_BASE_GET_MVENDORID      0x4
#define SBI_EXT_BASE_GET_MARCHID        0x5
#define SBI_EXT_BASE_GET_MIMPID         0x6

/* SBI function IDs for RFENCE extension */
#define SBI_EXT_RFENCE_REMOTE_FENCE_I           0x0
#define SBI_EXT_RFENCE_REMOTE_SFENCE_VMA        0x1
#define SBI_EXT_RFENCE_REMOTE_SFENCE_VMA_ASID   0x2
#define SBI_EXT_RFENCE_REMOTE_HFENCE_GVMA       0x3
#define SBI_EXT_RFENCE_REMOTE_HFENCE_GVMA_VMID  0x4
#define SBI_EXT_RFENCE_REMOTE_HFENCE_VVMA       0x5
#define SBI_EXT_RFENCE_REMOTE_HFENCE_VVMA_ASID  0x6

/* SBI function IDs for TIME extension */
#define SBI_EXT_TIME_SET_TIMER          0x0

/* SBI function IDs for IPI extension */
#define SBI_EXT_IPI_SEND_IPI            0x0

#define SBI_SPEC_VERSION_MAJOR_MASK     0x7f000000
#define SBI_SPEC_VERSION_MINOR_MASK     0x00ffffff

/* SBI return error codes */
#define SBI_SUCCESS             0
#define SBI_ERR_FAILURE         (-1)
#define SBI_ERR_NOT_SUPPORTED   (-2)
#define SBI_ERR_INVALID_PARAM   (-3)
#define SBI_ERR_DENIED          (-4)
#define SBI_ERR_INVALID_ADDRESS (-5)

#define SBI_SPEC_VERSION_DEFAULT 0x1

enum sbi_ext_hsm_fid {
    SBI_EXT_HSM_HART_START = 0,
    SBI_EXT_HSM_HART_STOP,
    SBI_EXT_HSM_HART_STATUS,
    SBI_EXT_HSM_HART_SUSPEND,
};

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_ecall(unsigned long ext, unsigned long fid,
                        unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3,
                        unsigned long arg4, unsigned long arg5);

/**
 * Writes given character to the console device.
 *
 * @param ch The data to be written to the console.
 */
void sbi_console_putchar(int ch);

void sbi_shutdown(void);

/*
 * Check underlying SBI implementation has RFENCE
 *
 * @return true for supported AND false for not-supported
 */
bool sbi_has_rfence(void);

/*
 * Instructs the remote harts to execute one or more SFENCE.VMA
 * instructions, covering the range of virtual addresses between
 * [start_addr, start_addr + size).
 *
 * Returns 0 if IPI was sent to all the targeted harts successfully
 * or negative value if start_addr or size is not valid.
 *
 * @param cpu_mask a cpu mask containing all the target CPUs (in Xen space).
 * @param start virtual address start
 * @param size virtual address range size
 */
int sbi_remote_sfence_vma(const cpumask_t *cpu_mask, vaddr_t start,
                          size_t size);

/*
 * Instruct the remote harts to execute one or more HFENCE.GVMA
 * instructions, covering the range of guest physical addresses
 * in [start_addr, start_addr + size) for all VMIDs.
 *
 * Returns 0 if IPI was sent to all the targeted harts successfully
 * or negative value if start_addr or size is not valid.
 *
 * The remote fence operation applies to the entire address space if either:
 *  - start_addr and size are both 0, or
 *  - size is equal to 2^XLEN-1.
 *
 * @param cpu_mask a cpu mask containing all the target CPUs (in Xen space).
 * @param start virtual address start
 * @param size virtual address range size
 */
int sbi_remote_hfence_gvma(const cpumask_t *cpu_mask, vaddr_t start,
                           size_t size);

/*
 * Instruct the remote harts to execute one or more HFENCE.GVMA
 * instructions, covering the range of guest physical addresses
 * in [start_addr, start_addr + size) only for the given VMID.
 *
 * @param cpu_mask a cpu mask containing all the target CPUs (in Xen space).
 * @param start virtual address start
 * @param size virtual address range size
 * @param vmid virtual machine id
 */
int sbi_remote_hfence_gvma_vmid(const cpumask_t *cpu_mask, vaddr_t start,
                                size_t size, unsigned long vmid);

/*
 * Instruct the remote harts to execute FENCE.I.
 *
 * @param cpu_mask a cpu mask containing all the target CPUs (in Xen space).
 */
int sbi_remote_fence_i(const cpumask_t *cpu_mask);

/*
 * Instruct the remote harts to execute one or more HFENCE.VVMA
 * instructions, covering the range of guest virtual addresses
 * in [start_addr, start_addr + size).
 *
 * @param cpu_mask a cpu mask containing all the target CPUs (in Xen space).
 * @param start guest virtual address start
 * @param size guest virtual address range size
 */
int sbi_remote_hfence_vvma(const cpumask_t *cpu_mask, vaddr_t start,
                           size_t size);

/*
 * Instruct the remote harts to execute one or more HFENCE.VVMA
 * instructions, covering the range of guest virtual addresses
 * in [start_addr, start_addr + size) only for the given ASID.
 *
 * @param cpu_mask a cpu mask containing all the target CPUs (in Xen space).
 * @param start guest virtual address start
 * @param size guest virtual address range size
 * @param asid guest address space id
 */
int sbi_remote_hfence_vvma_asid(const cpumask_t *cpu_mask, vaddr_t start,
                                size_t size, unsigned long asid);

/*
 * Request the SBI implementation to stop executing the calling hart in
 * supervisor-mode and return its ownership to the SBI implementation.
 * This call is not expected to return under normal conditions.
 *
 * The sbi_hart_stop() must be called with supervisor-mode interrupts disabled.
 * The possible error codes returned in sbiret.error are are:
 * SBI_ERR_FAILED -> Failed to stop execution of the current hart
 */
int sbi_hsm_hart_stop(void);

/*
 * Request the SBI implementation to start executing the target hart in
 * supervisor-mode, at the address specified by start_addr, with the specific
 * register values described in Table "HSM Hart Start Register State":
 *   Register Name  Register Value
 *     satp            0
 *     sstatus.SIE     0
 *     a0              hartid
 *     a1              opaque parameter
 *     All other registers remain in an undefined state.
 * A single unsigned long parameter is sufficient as start_addr, because the
 * hart will start execution in supervisor-mode with the MMU off, hence
 * start_addr must be less than XLEN bits wide.
 *
 * This call is asynchronous — more specifically, the sbi_hart_start() may
 * return before the target hart starts executing as long as the SBI
 * implementation is capable of ensuring the return code is accurate. If the
 * SBI implementation is a platform runtime firmware executing in machine-mode
 * (M-mode), then it MUST configure any physical memory protection it supports,
 * such as that defined by PMP, and other M-mode state, before transferring
 * control to supervisor-mode software.
 *
 * @hartid parameter specifies the target hart which is to be started.
 * @tart_addr parameter points to a runtime-specified physical address, where
 *            the hart can start executing in supervisor-mode.
 * @opaque parameter is an XLEN-bit value which will be set in the a1 register
 *         when the hart starts executing at start_addr.
 */
int sbi_hsm_hart_start(unsigned long hartid, unsigned long saddr,
                       unsigned long opaque);

/*
 * Send an inter-processor interrupt to all the harts defined in hart mask.
 * Interprocessor interrupts manifest at the receiving harts as Supervisor
 * Software Interrupts.
 *
 * This SBI call returns 0 upon success or an implementation specific negative
 * error code.
 *
 * @mask represents a set of Xen's CPU_IDs
 */
extern int (* __ro_after_init sbi_send_ipi)(const cpumask_t *mask);

/*
 * Initialize SBI library
 *
 * @return 0 on success, otherwise negative errno on failure
 */
int sbi_init(void);

#endif /* RISCV_SBI_H */
