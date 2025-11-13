/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * RISC-V instruction emulation for trapped guest accesses
 */

#include <xen/bug.h>
#include <xen/errno.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/cpufeature.h>
#include <asm/csr.h>
#include <asm/current.h>
#include <asm/emulate.h>
#include <asm/guest_access.h>
#include <asm/mmio.h>
#include <asm/processor.h>
#include <asm/regs.h>
#include <asm/riscv_encoding.h>
#include <asm/traps.h>

/*
 * Determine the trapped load or store instruction which caused a guest MMIO
 * trap.
 */
struct decoded_insn {
    /*
     * The trapped instruction: as read from guest memory, or the 32-bit
     * equivalent the hardware transformed it into (see insn_fetch_faulted()).
     * None of the extensions exposed to guests has instructions wider than
     * 32 bits, and insn_fetch_faulted() rejects anything longer.
     */
    uint32_t insn;
    /*
     * Length in bytes of the trapped instruction in guest memory: 2 or 4.
     * This need not be the length of the encoding in insn, as a compressed
     * instruction may have been transformed into its 32-bit equivalent.
     */
    uint8_t insn_len;
    /* Width of the memory access, in bytes. */
    uint8_t len;
    /* Number of the register operand: rd for a load, rs2 for a store. */
    uint8_t reg;
    /* The access is a store rather than a load. */
    bool is_write:1;
    /* The load zero-extends its result rather than sign-extending it. */
    bool is_unsigned:1;
};

/*
 * The hardware-reported details of a guest page fault, gathered once by
 * handle_guest_page_fault() and passed down to the emulation of the faulted
 * access.
 */
struct guest_fault {
    /* The guest register state. */
    struct cpu_user_regs *regs;
    /* scause: a fetch, a load or a store/AMO guest page fault. */
    unsigned long cause;
    /*
     * htinst: the trapped instruction in its transformed form, or one of the
     * special values (zero, or a pseudoinstruction).
     */
    unsigned long htinst;
    /* htval: as written by hardware; see resolve_faulting_gpa(). */
    unsigned long htval;
    /* stval: the guest virtual address of the faulting access. */
    unsigned long stval;
    /* The faulting guest physical address, filled by resolve_faulting_gpa(). */
    paddr_t gpa;
};

static void advance_pc(struct cpu_user_regs *regs, unsigned int step)
{
    regs->sepc += step;
}

/*
 * The effective XLEN of the guest at the point of the trap: hstatus.VSXL for a
 * trap taken from VS-mode, vsstatus.UXL for one taken from VU-mode.
 *
 * VSXL is consulted whichever mode the trap came from: if VSXL says 32,
 * vsstatus has no UXL field, so VU-mode is 32-bit as well and there is
 * nothing to configure.
 *
 * It is needed to decode a trapped instruction: the encodings which exist only
 * for XLEN=64 must not be recognized for a 32-bit guest. Besides those simply
 * being reserved there, the compressed ones are ambiguous: C.LD and C.FLW
 * share the encoding 0x6000 (mask 0xe003), and likewise C.SD/C.FSW,
 * C.LDSP/C.FLWSP and C.SDSP/C.FSWSP.
 *
 * IS_ENABLED() can't be used here as HSTATUS_VSXL is defined for
 * __riscv_xlen == 64 only, the field not existing on RV32 in the first place.
 */
static unsigned int guest_xlen(const struct cpu_user_regs *regs)
{
#ifdef CONFIG_RISCV_32
    return 32;
#else
    unsigned int xl = MASK_EXTR(regs->hstatus, HSTATUS_VSXL);

    if ( (xl > XLEN_FIELD_32) && !(regs->sstatus & SSTATUS_SPP) )
        xl = MASK_EXTR(csr_read(CSR_VSSTATUS), SSTATUS64_UXL);

    switch ( xl )
    {
    case XLEN_FIELD_32:
        return 32;

    case XLEN_FIELD_64:
        return 64;

    default:
        /*
         * The field holds nothing else in practice: XLEN_FIELD_128 would mean
         * RV128, which no implementation provides, and the only value left is
         * reserved. ASSERT_UNREACHABLE() being debug-only, a width still has
         * to be answered in release builds.
         *
         * Answer 32, that being the safe way to be wrong: the decoder then
         * fails to recognize the RV64-only encodings and emulation gives up.
         * Answering 64 for what may well be a 32-bit guest would instead have
         * it take C.FLW for C.LD and C.FSW for C.SD (see above), i.e. quietly
         * emulate an access of the wrong width against the wrong register.
         */
        ASSERT_UNREACHABLE();
        return 32;
    }
#endif
}

/*
 * Is @htinst one of the special pseudoinstruction values, reported for a guest
 * page fault taken on an implicit memory access done for VS-stage address
 * translation?
 *
 * It is enough to check only bits[1:0] as according to the spec:
 *
 * The value is one of the special pseudoinstructions defined later, all of
 * which have bits 1:0 equal to 00.
 */
static bool htinst_is_pseudo(unsigned long htinst)
{
    return htinst && !(htinst & 3);
}

/*
 * Reconstruct the guest physical address the access faulted on into @gf->gpa.
 */
static int resolve_faulting_gpa(struct guest_fault *gf)
{
    /*
     * A zero htval is either a genuine fault on guest physical address 0-3, or
     * an implementation which does not report the address at all; only Shtvala
     * tells the two apart.
     *
     * TODO: where it is absent, recover the address in software rather than
     * giving up.
     */
    if ( !gf->htval &&
         !riscv_isa_extension_available(NULL, RISCV_ISA_EXT_shtvala) )
    {
        printk_once(XENLOG_WARNING
                    "Shtvala isn't supported by h/w; s/w VS-stage walk required\n");
        return -EOPNOTSUPP;
    }

    /*
     * htval does not carry the two low bits of the address: for an explicit
     * access they are those of the faulting guest virtual address in stval,
     * and for an implicit access made for VS-stage translation they are zero.
     */
    gf->gpa = ((paddr_t)gf->htval << 2) |
              (htinst_is_pseudo(gf->htinst) ? 0 : (gf->stval & 3));

    return 0;
}

/*
 * Where the value of a decoded instruction's register operand is held.
 *
 * The register is held at its full width, whatever the guest's XLEN is (see
 * guest_xlen()). Where XLEN is narrower, the bits above it are not guaranteed
 * to be zero, nor even a sign extension: hardware only ignores them in source
 * operands, so they may have been left there by more privileged code running
 * at a wider XLEN. Hence callers must not rely on those bits when reading a
 * value, and must sign-extend what they write from bit XLEN-1, as hardware
 * does for the result of an operation.
 */
static unsigned long *guest_gpr(struct cpu_user_regs *regs,
                                const struct decoded_insn *di)
{
    /*
     * decode_ldst_insn() is what fills @di in, and it rejects an access
     * wider than a guest register.
     */
    ASSERT(di->len <= (guest_xlen(regs) / BITS_PER_BYTE));

    return regs_gpr_ptr(regs, di->reg);
}

/*
 * Obtain the instruction which caused a guest MMIO trap, filling in
 * @di->insn and @di->insn_len. It either comes transformed in htinst, or has
 * to be fetched from guest memory.
 *
 * Returns true if the fetch faulted in turn; the resulting trap has then
 * already been redirected to the guest and there is nothing further for the
 * caller to do. Where it returns false, @di has been filled in and emulation
 * is to continue.
 */
static bool insn_fetch_faulted(const struct guest_fault *gf,
                               struct decoded_insn *di)
{
    unsigned long htinst = gf->htinst;

    /*
     * A pseudoinstruction says nothing about the instruction the guest was
     * executing, and comes with a guest physical address which isn't the one
     * that instruction accessed. handle_guest_page_fault() deals with such a
     * fault on its own, so no emulation can ever start for one.
     */
    ASSERT(!htinst_is_pseudo(htinst));

    /*
     * Bit[0] == 1 implies trapped instruction value is transformed instruction
     * or custom value. A transformed instruction is a 32-bit encoding, so a
     * value with any bit above bit[31] set is a custom one, which is of no use
     * for decoding: fetch the instruction from guest memory then, as for zero.
     */
    if ( (htinst & BIT(0, UL)) && (htinst == (uint32_t)htinst) )
    {
        /*
         * A compressed instruction is reported already expanded to its 32-bit
         * equivalent, so htinst always holds a 32-bit encoding, except for
         * bits[1:0], which hold a marker: bit[0] is set to flag the
         * transformation and bit[1] is clear if the trapped instruction was
         * compressed. Setting both bits gives a 32-bit encoding
         * decode_ldst_insn() can decode. (Its handling of compressed
         * encodings is for the branch below, which reads the instruction from
         * guest memory as is.)
         *
         * The length then cannot come from the value anymore, only from
         * bit[1]. And only a 16- or a 32-bit instruction is ever reported
         * this way: the standard load and store instructions the hardware
         * transforms are all of one of these two lengths, anything else comes
         * as the zero special value handled below.
         */
        di->insn = htinst | 3;
        di->insn_len = (htinst & BIT(1, UL)) ? 4 : 2;
    }
    else
    {
        const struct cpu_user_regs *regs = gf->regs;
        struct trap_info utrap = {};

        /*
         * Bit[0] == 0 implies trapped instruction value is zero or special
         * value. Pseudoinstructions never get here (see the comment ahead of
         * the ASSERT() above), so only zero (or a custom value, see above) is
         * left: the instruction has to be read from guest memory.
         */

        di->insn = read_guest(regs->sepc, true, &utrap);
        if ( utrap.scause )
        {
            /*
             * If a G-stage translation fault happens while fetching the
             * trapped instruction, CAUSE_LOAD_GUEST_PAGE_FAULT is generated,
             * hlvx being a load. Such a fault is reported to the guest as an
             * instruction access fault.
             */
            if ( utrap.scause == CAUSE_LOAD_GUEST_PAGE_FAULT )
                utrap.scause = CAUSE_FETCH_ACCESS;

            /*
             * Not set in the initializer: on a fault read_guest() leaves in
             * utrap.sepc the address of its own faulting access, whereas the
             * trap is to be reported at the guest instruction.
             */
            utrap.sepc = regs->sepc;

            trap_redirect(&utrap);

            return true;
        }

        di->insn_len = INSN_LEN(di->insn);

        /*
         * read_guest() fetches at most two halfwords, so a wider encoding has
         * been read in part only and cannot be decoded here.
         *
         * Report an illegal instruction: none of the extensions exposed to
         * guests has instructions wider than 32 bits, so such an encoding is
         * not a valid instruction for the guest in the first place.
         *
         * The same goes for a compressed encoding where C isn't exposed to the
         * guest. The instruction in guest memory may have changed since the
         * trap, so what is read back must not be assumed to be what trapped,
         * nor to be valid for the guest.
         */
        if ( !di->insn_len ||
             (di->insn_len == 2 &&
              !riscv_isa_extension_available(current->domain->arch.isa,
                                             RISCV_ISA_EXT_c)) )
        {
            utrap.sepc = regs->sepc;
            utrap.scause = CAUSE_ILLEGAL_INSTRUCTION;
            /*
             * stval is left zero, which the spec allows for an illegal
             * instruction: only part of the instruction is in hand, and stval,
             * being only XLEN bits wide, may not be able to hold all of it
             * anyway.
             */

            trap_redirect(&utrap);

            return true;
        }
    }

    return false;
}

/*
 * Decode the load or store instruction fetched into @di, filling in the
 * remaining fields of it (@di->insn and @di->insn_len are filled by
 * insn_fetch_faulted()). Fields which don't apply to the instruction are left
 * alone, so @di is expected to start out zeroed.
 *
 * @xlen is the effective XLEN of the guest, needed as the encodings which
 * exist for XLEN=64 only must not be recognized for a 32-bit guest.
 *
 * Returns false if the instruction is not a load or store, or cannot be
 * emulated here.
 */
static bool decode_ldst_insn(struct decoded_insn *di, unsigned int xlen)
{
    uint32_t insn = di->insn;
    unsigned int funct3, width_log2;

    if ( INSN_IS_16BIT(insn) )
    {
        /*
         * C.LW, C.LD, C.SW and C.SD (bits[1:0] == 00), and their sp-relative
         * C.*SP forms (bits[1:0] == 10), have bits[15:13] of the form x1y:
         * x is set for a store, and y selects a width of 4 or 8 bytes.
         */
        funct3 = RV_X(insn, 13, 3);

        if ( (insn & 1) || !(funct3 & 2) )
            return false;

        di->is_write = funct3 & 4;
        width_log2 = 2 + (funct3 & 1);

        if ( !(insn & 2) )
            /* Quadrant 0: bits[4:2] encode rd' (load) or rs2' (store) */
            di->reg = RVC_RS2S(insn);
        else if ( di->is_write )
            /* Quadrant 2 (CSS): bits[6:2] encode rs2 for C.SWSP / C.SDSP */
            di->reg = RVC_RS2(insn);
        else
        {
            /* Quadrant 2 (CI): bits[11:7] encode rd for C.LWSP / C.LDSP */
            di->reg = RV_RD(insn);

            /* C.LWSP and C.LDSP are reserved with rd being x0. */
            if ( !di->reg )
                return false;
        }
    }
    else
    {
        /*
         * LOAD and STORE (I/S-type) have funct3 in bits[14:12]: funct3[1:0]
         * is log2 of the width in bytes, and funct3[2] selects zero-extension
         * for a load, while being reserved for a store.
         */
        funct3 = RV_X(insn, 12, 3);
        width_log2 = funct3 & 3;

        switch ( insn & INSN_OPCODE_MASK )
        {
        case INSN_OPCODE_LOAD:
            di->is_unsigned = funct3 & 4;
            di->reg = RV_RD(insn);
            break;

        case INSN_OPCODE_STORE:
            if ( funct3 & 4 )
                return false;
            di->is_write = true;
            di->reg = RV_RS2(insn);
            break;

        default:
            return false;
        }
    }

    di->len = 1U << width_log2;

    /*
     * No access is wider than XLEN, and one as wide as XLEN exists only in
     * its sign-extending form: this rules out the encodings which exist for
     * XLEN=64 only on a 32-bit guest, including C.FLW for C.LD (and alike).
     */
    if ( (di->len * BITS_PER_BYTE > xlen) ||
         (di->is_unsigned && di->len * BITS_PER_BYTE == xlen) )
        return false;

    return true;
}

static int emulate_load(const struct guest_fault *gf)
{
    struct cpu_user_regs *regs = gf->regs;
    mmio_info_t info = { .is_write = false };
    struct decoded_insn di = {};
    unsigned int shift;
    int rc;

    /* A fault taken re-reading the instruction is redirected to the guest. */
    if ( insn_fetch_faulted(gf, &di) )
        return 0;

    if ( !decode_ldst_insn(&di, guest_xlen(regs)) || di.is_write )
        return -EOPNOTSUPP;

    shift = BITS_PER_BYTE * (sizeof(info.data) - di.len);

#ifdef EMULATE_LOAD_DEBUG
    gdprintk(XENLOG_DEBUG, "pc=%#lx, addr=%#"PRIpaddr", len=%u, shift=%u\n",
             regs->sepc, gf->gpa, di.len, shift);
#endif

    rc = do_mmio(&info, gf->gpa, di.len);
    if ( rc )
        return rc;

    /* A load into x0 discards its result. */
    if ( di.reg )
        *guest_gpr(regs, &di) = di.is_unsigned
                                ? (info.data << shift) >> shift
                                : (long)(info.data << shift) >> shift;

    advance_pc(regs, di.insn_len);

    return 0;
}

static int emulate_store(const struct guest_fault *gf)
{
    struct cpu_user_regs *regs = gf->regs;
    mmio_info_t info = { .is_write = true };
    struct decoded_insn di = {};
    int rc;

    if ( insn_fetch_faulted(gf, &di) )
        return 0;

    if ( !decode_ldst_insn(&di, guest_xlen(regs)) || !di.is_write )
        return -EOPNOTSUPP;

    info.data = *guest_gpr(regs, &di);

    rc = do_mmio(&info, gf->gpa, di.len);
    if ( rc )
        return rc;

    advance_pc(regs, di.insn_len);

    return 0;
}

static void inject_access_fault(const struct guest_fault *gf)
{
    struct trap_info utrap = {
        .sepc = gf->regs->sepc,
        .stval = gf->stval,
    };

    switch ( gf->cause )
    {
    case CAUSE_FETCH_GUEST_PAGE_FAULT:
        utrap.scause = CAUSE_FETCH_ACCESS;
        break;

    case CAUSE_LOAD_GUEST_PAGE_FAULT:
        utrap.scause = CAUSE_LOAD_ACCESS;
        break;

    case CAUSE_STORE_GUEST_PAGE_FAULT:
        utrap.scause = CAUSE_STORE_ACCESS;
        break;

    default:
        domain_crash(current->domain, "Impossible cause (%#lx)?\n", gf->cause);
        return;
    }

    trap_redirect(&utrap);
}

void handle_guest_page_fault(struct cpu_user_regs *regs, unsigned long cause)
{
    struct guest_fault gf = {
        .regs = regs,
        .cause = cause,
        .gpa = INVALID_PADDR,
    };
    int rc;

    gf.htinst = csr_read(CSR_HTINST);
    gf.htval = csr_read(CSR_HTVAL);
    gf.stval = csr_read(CSR_STVAL);

    /*
     * htinst reports one of the special pseudoinstruction values when both:
     *   (a) the fault is caused by an implicit memory access for VS-stage
     *       address translation
     *   (b) htval holds a nonzero value: the guest physical address of that
     *       VS-stage PTE, not of the guest's original access, which is
     *       unknown as VS-stage translation didn't complete.
     *
     * No access to emulate then: to match bare hardware behavior, inject an
     * access fault of the ORIGINAL access type (Instruction, Load, or
     * Store/AMO) that initiated the address translation.
     *
     * FIXME: This assumes that guest page tables never reside in an emulated
     * MMIO region, i.e. that an implicit access faulting at G-stage always
     * targets an unpopulated GPA. Guests may (even transiently) place page
     * tables in MMIO-backed memory, e.g. a video frame buffer. Supporting
     * that would require walking the VS-stage page tables in software,
     * accessing the PTEs (including A/D updates) through the MMIO handlers,
     * and then emulating the original access, instead of injecting a fault.
     */
    if ( htinst_is_pseudo(gf.htinst) )
    {
        inject_access_fault(&gf);

        return;
    }

    rc = resolve_faulting_gpa(&gf);
    if ( rc )
        goto out;

    switch ( cause )
    {
    case CAUSE_LOAD_GUEST_PAGE_FAULT:
        rc = emulate_load(&gf);
        break;

    case CAUSE_STORE_GUEST_PAGE_FAULT:
        rc = emulate_store(&gf);
        break;

    case CAUSE_FETCH_GUEST_PAGE_FAULT:
        /*
         * Guest is trying to reach unmapped/unpopulated, or G-stage PTE doesn't
         * allow execution (X=0). Generate fetch fault in this case.
         */
        inject_access_fault(&gf);
        return;

    default:
        ASSERT_UNREACHABLE();
        rc = -EOPNOTSUPP;

        break;
    }

 out:
    if ( rc )
        domain_crash(current->domain,
                     "unable to handle guest page fault (cause=%#lx) at gpa %#"PRIpaddr"\n",
                     cause, gf.gpa);
}

static int truly_illegal_insn(const struct vcpu *v, const unsigned long insn)
{
    const struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(v);
    struct trap_info utrap = {};

    /* Redirect trap to Guest vCPU */
    utrap.sepc = regs->sepc;
    utrap.scause = CAUSE_ILLEGAL_INSTRUCTION;
    utrap.stval = insn;
    trap_redirect(&utrap);

    return 0;
}

/*
 * Positive return values of an insn_func handler tell system_opcode_insn()
 * what to do once the instruction has been emulated; a negative value is an
 * error which aborts the emulation.
 */
#define INSN_ILLEGAL_TRAP       1
#define INSN_CONTINUE_NEXT_SEPC 2

struct insn_func {
    unsigned long mask;
    unsigned long match;
    int (*func)(struct vcpu *v, unsigned long insn);
};

static int wfi_insn(struct vcpu *v, unsigned long insn)
{
    /* Emulate wait for interrupt (WFI) behaviour. */
    if ( vcpu_runnable(v) && (v->runstate.state == RUNSTATE_running) &&
         !vcpu_has_interrupts(v, ~0ULL) )
    {
        vcpu_block();
    }

    return INSN_CONTINUE_NEXT_SEPC;
}

static const struct insn_func system_opcode_funcs[] = {
    {
        .mask  = INSN_MASK_WFI,
        .match = INSN_MATCH_WFI,
        .func  = wfi_insn,
    },
};

static int system_opcode_insn(struct vcpu *v, unsigned long insn)
{
    struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(v);
    int rc = INSN_ILLEGAL_TRAP;

    for ( unsigned int i = 0; i < ARRAY_SIZE(system_opcode_funcs); i++ )
    {
        const struct insn_func *ifn = &system_opcode_funcs[i];

        if ( (insn & ifn->mask) == ifn->match )
        {
            rc = ifn->func(v, insn);
            break;
        }
    }

    switch ( rc )
    {
    case INSN_ILLEGAL_TRAP:
        return truly_illegal_insn(v, insn);

    case INSN_CONTINUE_NEXT_SEPC:
        regs->sepc += INSN_LEN(insn);
        break;

    default:
        break;
    }

    return rc <= 0 ? rc : 0;
}

int handle_virt_instruction_fault(struct vcpu *v)
{
    const struct cpu_user_regs *regs = vcpu_guest_cpu_user_regs(v);
    unsigned long insn = csr_read(CSR_STVAL);

    /*
     * According to the RISC-V Privileged Architecture spec:
     *  The stval register can optionally also be used to return the
     *  faulting instruction bits on an illegal-instruction exception
     *  (sepc points to the faulting instruction in memory). If stval is
     *   written with a nonzero value when an illegal-instruction exception
     *   occurs, then stval will contain the shortest of:
     *    - the actual faulting instruction
     *    - the first ILEN bits of the faulting instruction
     *    - the first SXLEN bits of the faulting instruction
     *
     * So a value of zero in stval signifies either that the feature is not
     * supported, or an illegal zero instruction was fetched. To verify
     * that it is needed to read an instruction directly from memory.
     */
    if ( !insn )
    {
        struct trap_info utrap = {};

        insn = read_guest(regs->sepc, true, &utrap);
        if ( utrap.scause )
        {
            if ( utrap.scause == CAUSE_LOAD_GUEST_PAGE_FAULT )
                return 0;

            trap_redirect(&utrap);

            return 0;
        }
    }

    /*
     * The 16-bit Compressed ISA (RVC) primarily consists of unprivileged
     * operations such as basic arithmetic, register moves, and simplified
     * load/store or jump instructions (e.g., C.ADDI, C.LWSP, C.J). These are
     * not relevant to the hypervisor and should be handled by the guest itself.
     */
    if ( INSN_IS_16BIT(insn) )
        return truly_illegal_insn(v, insn);

    switch ( insn & INSN_OPCODE_MASK )
    {
    case INSN_OPCODE_SYSTEM:
        return system_opcode_insn(v, insn);

    default:
        return truly_illegal_insn(v, insn);
    }
}
