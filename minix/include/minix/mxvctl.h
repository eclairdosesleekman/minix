/*
 * MXV kernel control ABI.
 *
 * Phase 2 adds opaque RAM pin tokens and kernel-owned EPT memory slots.
 * Userland may provide a VM-server virtual address to mint a pin token, but
 * physical addresses are never accepted or returned by this ABI.
 */
#ifndef _MINIX_MXVCTL_H
#define _MINIX_MXVCTL_H

#define MXVCTL_API_VERSION       1

#define MXV_QUERY_CAPS           0
#define MXV_VMXON                1
#define MXV_VMXOFF               2
#define MXV_VCPU_CREATE          3
#define MXV_VCPU_DESTROY         4
#define MXV_VCPU_RUN_PROBE       5
#define MXV_PIN_RANGE            6
#define MXV_PIN_RELEASE           7
#define MXV_VM_CREATE             8
#define MXV_VM_DESTROY            9
#define MXV_MEM_ATTACH            10
#define MXV_MEM_DETACH            11

#define MXV_MEM_READ              0x01
#define MXV_MEM_WRITE             0x02
#define MXV_MEM_EXEC              0x04
#define MXV_MEM_DIRTY_LOG         0x08 /* reserved; Phase 2 rejects it */

#define MXV_MAX_MEMSLOTS          8
#define MXV_MAX_RAM_BYTES         (16ULL * 1024 * 1024 * 1024)

#define MXV_CAP_VMX              (1ULL << 0)
#define MXV_CAP_FEATURE_LOCKED   (1ULL << 1)
#define MXV_CAP_VMX_OUTSIDE_SMX  (1ULL << 2)
#define MXV_CAP_VMCS_WB          (1ULL << 3)
#define MXV_CAP_EPT              (1ULL << 4)
#define MXV_CAP_UNRESTRICTED     (1ULL << 5)
#define MXV_CAP_EPT_WB           (1ULL << 6)
#define MXV_CAP_EPT_4LEVEL       (1ULL << 7)
#define MXV_CAP_SECONDARY        (1ULL << 8)
#define MXV_CAP_INVEPT           (1ULL << 9)
#define MXV_CAP_REQUIRED         (MXV_CAP_VMX | MXV_CAP_FEATURE_LOCKED | \
                                  MXV_CAP_VMX_OUTSIDE_SMX | MXV_CAP_VMCS_WB | \
                                  MXV_CAP_EPT | MXV_CAP_UNRESTRICTED | \
                                  MXV_CAP_EPT_WB | MXV_CAP_EPT_4LEVEL | \
                                  MXV_CAP_SECONDARY | MXV_CAP_INVEPT)

/* Intel VM-exit basic reason for VMCALL. */
#define MXV_EXIT_VMCALL          18

/*
 * SYS_MXVCTL common fields: m1_i1 = API version, m1_i2 = operation.
 * Phase-1 operations use m1_ull1. Phase-2 operations use the fixed-size m9
 * overlay: m9_ull1/object_handle, m9_ull2/arg0, m9_l1/arg1,
 * m9_l2/size, m9_l3/flags, m9_l4/slot_id, m9_l5/generation.
 *
 * All handles and pin tokens are opaque and endpoint/generation-bound.
 * MXV_PIN_RANGE accepts only a VM-server virtual address. The kernel validates
 * its page tables and retains HPA privately. SYS_MXVCTL remains RS-only until
 * mxvd capability delegation is implemented.
 */
#include <stdint.h>

struct mxvctl_req {
        uint32_t version;
        uint32_t operation;
        uint64_t object_handle;
        uint64_t arg0;
        uint64_t arg1;
        uint32_t grant_id;
        uint32_t flags;
        uint64_t generation;
        uint64_t reserved[2];
};

struct mxv_memslot_req {
        uint32_t version;
        uint32_t slot_id;
        uint32_t flags;
        uint32_t reserved;
        uint64_t guest_phys;
        uint64_t size;
        uint64_t pin_token;
        uint64_t generation;
};

#endif /* _MINIX_MXVCTL_H */
