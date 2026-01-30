/* SPDX-License-Identifier: MIT */

/*
 * xen/arch/riscv/asm/include/aplic.h
 *
 * RISC-V Advanced Platform-Level Interrupt Controller support
 *
 * Copyright (c) Microchip.
 */

#ifndef ASM_RISCV_APLIC_H
#define ASM_RISCV_APLIC_H

#include <xen/types.h>

#include <asm/imsic.h>

/*
 * APLIC register offsets and, immediately following each of them, the
 * definitions of the fields of the respective register:
 *
 * #define APLIC_$NAME                      0x$OFFSET
 * #define  APLIC_$NAME_$FIELD1             ...
 * #define  APLIC_$NAME_$FIELD2             ...
 *
 * Blocks of related constants are sorted by register offset.
 */

#define APLIC_REG_OFFSET_MASK               0x3fff

#define APLIC_DOMAINCFG                     0x0000
/*
 * domaincfg read-only fields (AIA spec):
 *  - bits [31:24] -> read-only 0x80
 *  - bit 7        -> read-only 0
 */
#define  APLIC_DOMAINCFG_RO             (0x80U << 24)
#define  APLIC_DOMAINCFG_IE             BIT(8, U)
#define  APLIC_DOMAINCFG_DM             BIT(2, U)
#define  APLIC_DOMAINCFG_BE             BIT(0, U)

#define APLIC_SOURCECFG_BASE            0x0004
#define APLIC_SOURCECFG_LAST            0x0ffc
#define APLIC_SOURCECFG_SM_INACTIVE     0x0
#define APLIC_SOURCECFG_SM_DETACH       0x1
#define APLIC_SOURCECFG_SM_EDGE_RISE    0x4
#define APLIC_SOURCECFG_SM_EDGE_FALL    0x5
#define APLIC_SOURCECFG_SM_LEVEL_HIGH   0x6
#define APLIC_SOURCECFG_SM_LEVEL_LOW    0x7
#define APLIC_SOURCECFG_D               BIT(10, U)

#define APLIC_SMSICFGADDR               0x1bc8
#define APLIC_SMSICFGADDRH              0x1bcc

#define APLIC_SETIP_BASE                0x1c00
#define APLIC_SETIP_LAST                0x1c7c
#define APLIC_SETIPNUM                  0x1cdc

#define APLIC_CLRIP_BASE                0x1d00
#define APLIC_CLRIP_LAST                0x1d7c
#define APLIC_CLRIPNUM                  0x1ddc

#define APLIC_SETIE_BASE                0x1e00
#define APLIC_SETIE_LAST                0x1e7c
#define APLIC_SETIENUM                  0x1edc

#define APLIC_CLRIE_BASE                0x1f00
#define APLIC_CLRIE_LAST                0x1f7c
#define APLIC_CLRIENUM                  0x1fdc

#define APLIC_SETIPNUM_LE               0x2000

#define APLIC_GENMSI                    0x3000

#define APLIC_TARGET_BASE               0x3004
#define APLIC_TARGET_LAST               0x3ffc
#define APLIC_TARGET_HART_IDX_SHIFT     18
#define APLIC_TARGET_EIID_MASK          0x7ff
#define APLIC_TARGET_HART_IDX_MASK      0xfffc0000
#define APLIC_TARGET_GUEST_IDX_MASK     0x3f000

#define APLIC_IDC_SIZE                  32

#define APLIC_MIN_SIZE                  0x4000
#define APLIC_SIZE_ALIGN(x)             ROUNDUP(x, APLIC_MIN_SIZE)

#define APLIC_SIZE(nr_cpus) \
    (APLIC_MIN_SIZE + APLIC_SIZE_ALIGN(APLIC_IDC_SIZE * (nr_cpus)))

/*
 * Using setip is fine here, as all SET* and CLR* register groups consist of 32
 * registers and therefore have identical sizes.
 *
 * Lowest 2 bits are always zero for SET* and CLR* registers.
 */
#define APLIC_SETCLR_OFFSET_MASK \
    (sizeof_field(struct aplic_regs, setip) - sizeof(uint32_t))

#define APLIC_xMSICFGADDR_PPN_SHIFT IMSIC_MMIO_PAGE_SHIFT

#define APLIC_xMSICFGADDR_PPN_HHX_MASK(hhxw) \
    (BIT(hhxw, UL) - 1)
#define APLIC_xMSICFGADDR_PPN_HHX_SHIFT(hhxs) \
    ((hhxs) + APLIC_xMSICFGADDR_PPN_SHIFT)

struct aplic_regs {
    uint32_t domaincfg;         /* 0x0000 */
    uint32_t sourcecfg[1023];   /* 0x0004 */
    uint8_t _reserved1[3008];   /* 0x1000 */

    uint32_t mmsiaddrcfg;       /* 0x1BC0 */
    uint32_t mmsiaddrcfgh;      /* 0x1BC4 */
    uint32_t smsiaddrcfg;       /* 0x1BC8 */
    uint32_t smsiaddrcfgh;      /* 0x1BCC */
    uint8_t _reserved2[48];     /* 0x1BD0 */

    uint32_t setip[32];         /* 0x1C00 */
    uint8_t _reserved3[92];     /* 0x1C80 */

    uint32_t setipnum;          /* 0x1CDC */
    uint8_t _reserved4[32];     /* 0x1CE0 */

    uint32_t in_clrip[32];      /* 0x1D00 */
    uint8_t _reserved5[92];     /* 0x1D80 */

    uint32_t clripnum;          /* 0x1DDC */
    uint8_t _reserved6[32];     /* 0x1DE0 */

    uint32_t setie[32];         /* 0x1E00 */
    uint8_t _reserved7[92];     /* 0x1E80 */

    uint32_t setienum;          /* 0x1EDC */
    uint8_t _reserved8[32];     /* 0x1EE0 */

    uint32_t clrie[32];         /* 0x1F00 */
    uint8_t _reserved9[92];     /* 0x1F80 */

    uint32_t clrienum;          /* 0x1FDC */
    uint8_t _reserved10[32];    /* 0x1FF0 */

    uint32_t setipnum_le;       /* 0x2000 */
    uint32_t setipnum_be;       /* 0x2004 */
    uint8_t _reserved11[4088];  /* 0x2008 */

    uint32_t genmsi;            /* 0x3000 */
    uint32_t target[1023];      /* 0x3004 */
};

uint32_t aplic_hw_read_reg(unsigned int offset, uint32_t mask);
void aplic_hw_write_reg(unsigned int offset, uint32_t value);

bool has_msi_support(void);

#endif /* ASM_RISCV_APLIC_H */
