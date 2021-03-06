#ifndef XNUSPY_STRUCTS
#define XNUSPY_STRUCTS

#include <sys/queue.h>

struct stailq_entry {
    void *elem;
    STAILQ_ENTRY(stailq_entry) link;
};

struct xnuspy_reflector_page {
    struct xnuspy_reflector_page *next;
    void *page;
    int used;
};

struct orphan_mapping {
    uint64_t mapping_addr;
    uint64_t mapping_size;
    void *memory_object;
    struct xnuspy_reflector_page *first_reflector_page;
    uint64_t used_reflector_pages;
};

/* This structure represents a shared __TEXT and __DATA mapping. There is
 * one xnuspy_mapping_metadata struct per process. */
struct xnuspy_mapping_metadata {
    /* Reference count for metadata, NOT the xnuspy_tramp */
    _Atomic uint64_t refcnt;
    /* Process which owns this mapping (p_uniqueid) */
    uint64_t owner;
    /* Pointer to the first reflector page used for this mapping */
    struct xnuspy_reflector_page *first_reflector_page;
    /* How many reflector pages are used ^ */
    uint64_t used_reflector_pages;
    /* Memory object for this shared mapping, ipc_port_t */
    void *memory_object;
    /* Address of the start of this mapping */
    uint64_t mapping_addr;
    /* Size of this mapping */
    uint64_t mapping_size;
    /* Death callback */
    void (*death_callback)(void);
};

/* This structure contains information for an xnuspy_tramp that isn't
 * necessary to keep in the struct itself. I do this to save space. These are
 * not reference counted because they're per-hook. */
struct xnuspy_tramp_metadata {
    /* Hooked kernel function */
    uint64_t hooked;
    /* Overwritten instruction */
    uint32_t orig_instr;
};

/* This structure represents a function hook. Every xnuspy_tramp struct resides
 * on writeable, executable memory. */
struct xnuspy_tramp {
    /* Kernel virtual address of reflected userland replacement */
    uint64_t replacement;
    /* The trampoline for a hooked function. When the user installs a hook
     * on a function, the first instruction of that function is replaced
     * with a branch to here. An xnuspy trampoline looks like this:
     *  tramp[0]    LDR X16, #-0x8      (replacement)
     *  tramp[1]    BR X16
     */
    uint32_t tramp[2];
    /* An abstraction that represents the original function. It's just another
     * trampoline, but it can take on one of seven forms. The most common
     * form is this:
     *  orig[0]     <original first instruction of the hooked function>
     *  orig[1]     LDR X16, #0x8
     *  orig[2]     BR X16
     *  orig[3]     <address of second instruction of the hooked function>[31:0]
     *  orig[4]     <address of second instruction of the hooked function>[63:32]
     *
     * The above form is taken when the original first instruction of the hooked
     * function is not an immediate conditional branch (b.cond), an immediate
     * compare and branch (cbz/cbnz), an immediate test and branch (tbz/tbnz),
     * an immediate unconditional branch (b), an immediate unconditional
     * branch with link (bl), load register (literal), or an ADR. These are
     * special cases because the immediates do not contain enough bits for me
     * to just "fix up" or assume we'll always be in range once we do, so I
     * need to emit an equivalent sequence of instructions.
     *
     * If the first instruction was B.cond <label>
     *  orig[0]     LDR X16, #0x10
     *  orig[1]     LDR X17, #0x14
     *  orig[2]     CSEL X16, X16, X17, <cond>
     *  orig[3]     BR X16
     *  orig[4]     <destination if condition holds>[31:0]
     *  orig[5]     <destination if condition holds>[63:32]
     *  orig[6]     <address of second instruction of the hooked function>[31:0]
     *  orig[7]     <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was CBZ Rn, <label> or CBNZ Rn, <label>
     *  orig[0]     LDR X16, #0x14
     *  orig[1]     LDR X17, #0x18
     *  orig[2]     CMP Rn, #0
     *  orig[3]     CSEL X16, X16, X17, <if CBZ, eq, if CBNZ, ne>
     *  orig[4]     BR X16
     *  orig[5]     <destination if condition holds>[31:0]
     *  orig[6]     <destination if condition holds>[63:32]
     *  orig[7]     <address of second instruction of the hooked function>[31:0]
     *  orig[8]     <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was TBZ Rn, #n, <label> or TBNZ Rn, #n, <label>
     *  orig[0]     LDR X16, #0x14
     *  orig[1]     LDR X17, #0x18
     *  orig[2]     TST Rn, #(1 << n)
     *  orig[3]     CSEL X16, X16, X17, <if TBZ, eq, if TBNZ, ne>
     *  orig[4]     BR X16
     *  orig[5]     <destination if condition holds>[31:0]
     *  orig[6]     <destination if condition holds>[63:32]
     *  orig[7]     <address of second instruction of the hooked function>[31:0]
     *  orig[8]     <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was ADR Rn, #n
     *  orig[0]     ADRP Rn, #n@PAGE
     *  orig[1]     ADD Rn, Rn, #n@PAGEOFF
     *  orig[2]     LDR X16, #0x8
     *  orig[3]     BR X16
     *  orig[4]     <address of second instruction of the hooked function>[31:0]
     *  orig[5]     <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction was B <label>
     *  orig[0]     LDR X16, 0x8
     *  orig[1]     BR X16
     *  orig[2]     <address of branch destination>[31:0]
     *  orig[3]     <address of branch destination>[63:32]
     *
     * If the first instruction was BL <label>
     *  orig[0]     MOV X17, X30
     *  orig[1]     LDR X16, #0x14
     *  orig[2]     BLR X16
     *  orig[3]     MOV X30, X17
     *  orig[4]     LDR X16, #0x10
     *  orig[5]     BR X16
     *  orig[6]     <address of branch destination>[31:0]
     *  orig[7]     <address of branch destination>[63:32]
     *  orig[8]     <address of second instruction of the hooked function>[31:0]
     *  orig[9]     <address of second instruction of the hooked function>[63:32]
     *
     * If the first instruction belongs to the "Load register (literal)" class
     *  orig[0]     ADRP X16, <label>@PAGE
     *  orig[1]     ADD X16, X16, <label>@PAGEOFF
     *  orig[2]     LDR{SW} Rn, [X16] or PRFM <prfop>, [X16]
     *  orig[3]     LDR X16, 0x8
     *  orig[4]     BR X16
     *  orig[5]     <address of second instruction of the hooked function>[31:0]
     *  orig[6]     <address of second instruction of the hooked function>[63:32]
     */
    uint32_t orig[10];
    struct xnuspy_tramp_metadata *tramp_metadata;
    struct xnuspy_mapping_metadata *mapping_metadata;
};

typedef struct __lck_rw_t__ {
    uint64_t word;
    void *owner;
} lck_rw_t;

#define vme_prev		links.prev
#define vme_next		links.next
#define vme_start		links.start
#define vme_end			links.end

struct _vm_map {
    lck_rw_t lck;
    struct {
        struct {
            void *prev;
            void *next;
            void *start;
            void *end;
        } links;
    } hdr;
};

struct sysent {
    uint64_t sy_call;
    void *sy_arg_munge32;
    int32_t sy_return_type;
    int16_t sy_narg;
    uint16_t sy_arg_bytes;
};

#endif
