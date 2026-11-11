/*
 * MXV Phase 2: kernel-owned RAM pins and EPT memslots.
 *
 * The VM server owns the allocation. RS requests a pin by passing the VM
 * server virtual address; this file verifies the VM server page tables and
 * stores the HPA privately. EPT pages are kernel-owned and never mapped into
 * a user process. EPTP is not connected to a live VMCS until Phase 3.
 */
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <minix/mxvctl.h>

#include "kernel/kernel.h"
#include "kernel/proc.h"
#include "arch_proto.h"
#include "archconst.h"

#define MXV_PAGE_SIZE           4096ULL
#define MXV_MAX_VMS             8
#define MXV_MAX_PINS            32
#define MXV_EPT_TABLES          256
#define MXV_EPT_ADDR_MASK       0x000ffffffffff000ULL
#define MXV_EPT_PRESENT         0x7ULL
#define MXV_EPT_LARGE           (1ULL << 7)
#define MXV_EPT_WB              (6ULL << 3)
#define MXV_EPT_2M_SIZE         (2ULL * 1024 * 1024)
#define MXV_EPT_2M_MASK         (MXV_EPT_2M_SIZE - 1)
#define MXV_PHYS_DIRECTMAP_BASE 0xFFFFFF8000000000ULL

struct mxv_pin {
        int in_use;
        int attached;
        endpoint_t owner;
        u32_t generation;
        vir_bytes vm_virt;
        phys_bytes hpa;
        u64_t size;
};

struct mxv_memslot {
        int in_use;
        u32_t generation;
        u32_t flags;
        u64_t gpa;
        u64_t size;
        u64_t pin_token;
};

struct mxv_vm {
        int in_use;
        endpoint_t owner;
        u32_t generation;
        u32_t ept_pages_used;
        u64_t eptp;
        struct mxv_memslot slots[MXV_MAX_MEMSLOTS];
};

/* Kernel BSS is physically stable and page-aligned for the kernel lifetime. */
static u64_t mxv_ept_pages[MXV_MAX_VMS][MXV_EPT_TABLES][512]
        __aligned(4096);
static struct mxv_pin mxv_pins[MXV_MAX_PINS];
static struct mxv_vm mxv_vms[MXV_MAX_VMS];
static u32_t mxv_next_generation = 1;

static u32_t
mxv_generation(void)
{
        u32_t generation = mxv_next_generation++;

        if (generation == 0)
                generation = mxv_next_generation++;
        if (mxv_next_generation == 0)
                mxv_next_generation = 1;
        return generation;
}

static int
mxv_endpoint_live(endpoint_t endpoint)
{
        int proc_nr;

        if (!isokendpt(endpoint, &proc_nr))
                return 0;
        return !isemptyp(proc_addr(proc_nr));
}

static u64_t
mxv_handle(unsigned int slot, u32_t generation)
{
        return ((u64_t)generation << 32) | (u64_t)(slot + 1);
}

static struct mxv_pin *
mxv_pin_lookup(endpoint_t owner, u64_t token, unsigned int *index)
{
        u32_t slot = (u32_t)token;
        u32_t generation = (u32_t)(token >> 32);
        struct mxv_pin *pin;

        if (slot == 0 || slot > MXV_MAX_PINS)
                return NULL;
        pin = &mxv_pins[slot - 1];
        if (!pin->in_use || pin->owner != owner ||
            pin->generation != generation)
                return NULL;
        if (index != NULL)
                *index = slot - 1;
        return pin;
}

static struct mxv_vm *
mxv_vm_lookup(endpoint_t owner, u64_t handle, unsigned int *index)
{
        u32_t slot = (u32_t)handle;
        u32_t generation = (u32_t)(handle >> 32);
        struct mxv_vm *vm;

        if (slot == 0 || slot > MXV_MAX_VMS)
                return NULL;
        vm = &mxv_vms[slot - 1];
        if (!vm->in_use || vm->owner != owner ||
            vm->generation != generation)
                return NULL;
        if (index != NULL)
                *index = slot - 1;
        return vm;
}

static u64_t *
mxv_ept_page(int vm_index, u64_t physical)
{
        (void)vm_index;
        return (u64_t *)(vir_bytes)(MXV_PHYS_DIRECTMAP_BASE + physical);
}

static int
mxv_ept_alloc_page(int vm_index, u64_t *physical)
{
        struct mxv_vm *vm = &mxv_vms[vm_index];
        unsigned int page;

        if (vm->ept_pages_used >= MXV_EPT_TABLES)
                return ENOMEM;
        page = vm->ept_pages_used++;
        memset(mxv_ept_pages[vm_index][page], 0, 4096);
        *physical = vir2phys(mxv_ept_pages[vm_index][page]);
        if (*physical & (MXV_PAGE_SIZE - 1))
                return EFAULT;
        return OK;
}

static int
mxv_ept_get_pd(int vm_index, u64_t gpa, int create, u64_t **pd_out)
{
        u64_t *pml4, *pdpt, *pd;
        u64_t entry, physical;
        unsigned int pml4i, pdpti;
        int r;

        pml4i = (unsigned int)((gpa >> 39) & 0x1ff);
        pdpti = (unsigned int)((gpa >> 30) & 0x1ff);
        pml4 = mxv_ept_page(vm_index,
            mxv_vms[vm_index].eptp & MXV_EPT_ADDR_MASK);
        entry = pml4[pml4i];
        if (!(entry & MXV_EPT_PRESENT)) {
                if (!create)
                        return ENOENT;
                r = mxv_ept_alloc_page(vm_index, &physical);
                if (r != OK)
                        return r;
                pml4[pml4i] = physical | MXV_EPT_PRESENT;
                entry = pml4[pml4i];
        }
        pdpt = mxv_ept_page(vm_index, entry & MXV_EPT_ADDR_MASK);
        entry = pdpt[pdpti];
        if (!(entry & MXV_EPT_PRESENT)) {
                if (!create)
                        return ENOENT;
                r = mxv_ept_alloc_page(vm_index, &physical);
                if (r != OK)
                        return r;
                pdpt[pdpti] = physical | MXV_EPT_PRESENT;
                entry = pdpt[pdpti];
        } else if (entry & MXV_EPT_LARGE) {
                return EEXIST; /* 1 GiB leaves are not used by MXV-PC1. */
        }
        pd = mxv_ept_page(vm_index, entry & MXV_EPT_ADDR_MASK);
        *pd_out = pd;
        return OK;
}

static int
mxv_ept_map_2m(int vm_index, u64_t gpa, u64_t hpa, u32_t flags)
{
        u64_t *pd;
        unsigned int pdi = (unsigned int)((gpa >> 21) & 0x1ff);
        int r;

        r = mxv_ept_get_pd(vm_index, gpa, 1, &pd);
        if (r != OK)
                return r;
        if (pd[pdi] != 0)
                return EEXIST;
        pd[pdi] = (hpa & ~MXV_EPT_2M_MASK) |
            (u64_t)(flags & 7) | MXV_EPT_WB | MXV_EPT_LARGE;
        return OK;
}

static int
mxv_ept_map_4k(int vm_index, u64_t gpa, u64_t hpa, u32_t flags)
{
        u64_t *pd, *pt, physical;
        unsigned int pdi = (unsigned int)((gpa >> 21) & 0x1ff);
        unsigned int pti = (unsigned int)((gpa >> 12) & 0x1ff);
        int r;

        r = mxv_ept_get_pd(vm_index, gpa, 1, &pd);
        if (r != OK)
                return r;
        if (pd[pdi] == 0) {
                r = mxv_ept_alloc_page(vm_index, &physical);
                if (r != OK)
                        return r;
                pd[pdi] = physical | MXV_EPT_PRESENT;
        } else if (pd[pdi] & MXV_EPT_LARGE) {
                return EEXIST;
        }
        pt = mxv_ept_page(vm_index, pd[pdi] & MXV_EPT_ADDR_MASK);
        if (pt[pti] != 0)
                return EEXIST;
        pt[pti] = (hpa & MXV_EPT_ADDR_MASK) |
            (u64_t)(flags & 7) | MXV_EPT_WB;
        return OK;
}

static int
mxv_ept_map_range(int vm_index, u64_t gpa, u64_t hpa,
        u64_t size, u32_t flags)
{
        u64_t offset = 0;
        int r;

        while (offset < size) {
                if (((gpa + offset) & MXV_EPT_2M_MASK) == 0 &&
                    ((hpa + offset) & MXV_EPT_2M_MASK) == 0 &&
                    size - offset >= MXV_EPT_2M_SIZE) {
                        r = mxv_ept_map_2m(vm_index, gpa + offset,
                            hpa + offset, flags);
                        if (r != OK)
                                return r;
                        offset += MXV_EPT_2M_SIZE;
                } else {
                        r = mxv_ept_map_4k(vm_index, gpa + offset,
                            hpa + offset, flags);
                        if (r != OK)
                                return r;
                        offset += MXV_PAGE_SIZE;
                }
        }
        return OK;
}

static int
mxv_ept_rebuild(int vm_index)
{
        struct mxv_vm *vm = &mxv_vms[vm_index];
        u64_t root;
        unsigned int i;
        int r;

        memset(mxv_ept_pages[vm_index], 0, sizeof(mxv_ept_pages[vm_index]));
        vm->ept_pages_used = 0;
        vm->eptp = 0;
        r = mxv_ept_alloc_page(vm_index, &root);
        if (r != OK)
                return r;
        /* EPTP: WB memory type, four-level page walk (encoded as 3). */
        vm->eptp = root | 6ULL | (3ULL << 3);

        for (i = 0; i < MXV_MAX_MEMSLOTS; i++) {
                struct mxv_memslot *slot = &vm->slots[i];
                struct mxv_pin *pin;

                if (!slot->in_use)
                        continue;
                pin = mxv_pin_lookup(vm->owner, slot->pin_token, NULL);
                if (pin == NULL)
                        return ESTALE;
                r = mxv_ept_map_range(vm_index, slot->gpa, pin->hpa,
                    slot->size, slot->flags);
                if (r != OK)
                        return r;
        }
        return OK;
}

int
mxv_pin_range(endpoint_t owner, vir_bytes vm_virt, u64_t size,
        u64_t *token)
{
        struct proc *vmproc;
        struct mxv_pin *pin = NULL;
        phys_bytes hpa;
        u32_t pte;
        u64_t end;
        unsigned int i, page, pin_index;
        int owner_proc, r;

        if (token == NULL || size == 0 || size > MXV_MAX_RAM_BYTES ||
            (vm_virt & (MXV_PAGE_SIZE - 1)) != 0 ||
            (size & (MXV_PAGE_SIZE - 1)) != 0)
                return EINVAL;
        if ((u64_t)vm_virt + size < (u64_t)vm_virt ||
            (u64_t)vm_virt + size > 0x0000800000000000ULL)
                return EINVAL;
        if (!isokendpt(owner, &owner_proc) || isemptyp(proc_addr(owner_proc)))
                return ESRCH;

        vmproc = proc_addr(VM_PROC_NR);
        if (isemptyp(vmproc) || !HASPT(vmproc))
                return ESRCH;
        if (vm_lookup_range(vmproc, vm_virt, &hpa, (size_t)size) != size)
                return EFAULT;
        if ((hpa & (MXV_PAGE_SIZE - 1)) != 0 ||
            hpa + size < hpa || hpa + size > (1ULL << 52))
                return EINVAL;

        /* A contiguous range alone does not prove every leaf is writable RAM. */
        for (page = 0; page < size / MXV_PAGE_SIZE; page++) {
                phys_bytes this_hpa;
                vir_bytes this_virt = vm_virt +
                    (vir_bytes)((u64_t)page * MXV_PAGE_SIZE);

                r = vm_lookup(vmproc, this_virt, &this_hpa, &pte);
                if (r != OK || this_hpa != hpa +
                    (phys_bytes)((u64_t)page * MXV_PAGE_SIZE) ||
                    !(pte & AMD64_VM_PRESENT) ||
                    !(pte & AMD64_VM_USER) ||
                    !(pte & AMD64_VM_WRITE))
                        return EACCES;
        }

        end = (u64_t)hpa + size;
        for (i = 0; i < MXV_MAX_PINS; i++) {
                u64_t old_end;

                if (!mxv_pins[i].in_use) {
                        if (pin == NULL) {
                                pin = &mxv_pins[i];
                                pin_index = i;
                        }
                        continue;
                }
                old_end = (u64_t)mxv_pins[i].hpa + mxv_pins[i].size;
                if ((u64_t)hpa < old_end &&
                    (u64_t)mxv_pins[i].hpa < end)
                        return EBUSY;
        }
        if (pin == NULL)
                return ENOSPC;

        memset(pin, 0, sizeof(*pin));
        pin->in_use = 1;
        pin->owner = owner;
        pin->generation = mxv_generation();
        pin->vm_virt = vm_virt;
        pin->hpa = hpa;
        pin->size = size;
        *token = mxv_handle(pin_index, pin->generation);
        return OK;
}

int
mxv_pin_release(endpoint_t owner, u64_t token)
{
        struct mxv_pin *pin = mxv_pin_lookup(owner, token, NULL);

        if (pin == NULL)
                return ENOENT;
        if (pin->attached)
                return EBUSY;
        memset(pin, 0, sizeof(*pin));
        return OK;
}

int
mxv_vm_create(endpoint_t owner, u64_t *handle)
{
        unsigned int i;
        struct mxv_vm *vm;

        if (handle == NULL || !mxv_endpoint_live(owner))
                return EINVAL;
        for (i = 0; i < MXV_MAX_VMS; i++)
                if (!mxv_vms[i].in_use)
                        break;
        if (i == MXV_MAX_VMS)
                return ENOSPC;

        vm = &mxv_vms[i];
        memset(vm, 0, sizeof(*vm));
        vm->in_use = 1;
        vm->owner = owner;
        vm->generation = mxv_generation();
        *handle = mxv_handle(i, vm->generation);
        return OK;
}

int
mxv_vm_destroy(endpoint_t owner, u64_t handle)
{
        struct mxv_vm *vm;
        unsigned int i;

        vm = mxv_vm_lookup(owner, handle, &i);
        if (vm == NULL)
                return ENOENT;
        for (i = 0; i < MXV_MAX_MEMSLOTS; i++)
                if (vm->slots[i].in_use)
                        return EBUSY;
        memset(vm, 0, sizeof(*vm));
        return OK;
}

int
mxv_mem_attach(endpoint_t owner, u64_t vm_handle, u64_t pin_token,
        u64_t gpa, u64_t size, u32_t flags, u32_t slot_id,
        u64_t *generation)
{
        struct mxv_vm *vm;
        struct mxv_pin *pin;
        struct mxv_memslot *slot;
        unsigned int vm_index, i;
        u64_t end;
        int r;

        if (generation == NULL || slot_id >= MXV_MAX_MEMSLOTS ||
            size == 0 || size > MXV_MAX_RAM_BYTES ||
            (gpa & (MXV_PAGE_SIZE - 1)) != 0 ||
            (size & (MXV_PAGE_SIZE - 1)) != 0 ||
            gpa + size < gpa || gpa + size > (1ULL << 48) ||
            (flags & ~(MXV_MEM_READ | MXV_MEM_WRITE | MXV_MEM_EXEC |
                MXV_MEM_DIRTY_LOG)) != 0 ||
            (flags & MXV_MEM_DIRTY_LOG) != 0 ||
            !(flags & MXV_MEM_READ))
                return EINVAL;

        vm = mxv_vm_lookup(owner, vm_handle, &vm_index);
        if (vm == NULL)
                return ENOENT;
        slot = &vm->slots[slot_id];
        if (slot->in_use)
                return EEXIST;
        pin = mxv_pin_lookup(owner, pin_token, NULL);
        if (pin == NULL || pin->attached || pin->size != size)
                return EINVAL;

        end = gpa + size;
        for (i = 0; i < MXV_MAX_MEMSLOTS; i++) {
                u64_t old_end;
                struct mxv_memslot *old = &vm->slots[i];

                if (!old->in_use)
                        continue;
                old_end = old->gpa + old->size;
                if (gpa < old_end && old->gpa < end)
                        return EEXIST;
        }

        memset(slot, 0, sizeof(*slot));
        slot->in_use = 1;
        slot->generation = mxv_generation();
        slot->flags = flags & (MXV_MEM_READ | MXV_MEM_WRITE | MXV_MEM_EXEC);
        slot->gpa = gpa;
        slot->size = size;
        slot->pin_token = pin_token;

        r = mxv_ept_rebuild(vm_index);
        if (r != OK) {
                memset(slot, 0, sizeof(*slot));
                (void)mxv_ept_rebuild(vm_index);
                return r;
        }
        pin->attached = 1;
        *generation = slot->generation;
        return OK;
}

int
mxv_mem_detach(endpoint_t owner, u64_t vm_handle, u32_t slot_id,
        u64_t generation)
{
        struct mxv_vm *vm;
        struct mxv_memslot saved;
        struct mxv_pin *pin;
        unsigned int vm_index;
        int r;

        if (slot_id >= MXV_MAX_MEMSLOTS)
                return EINVAL;
        vm = mxv_vm_lookup(owner, vm_handle, &vm_index);
        if (vm == NULL)
                return ENOENT;
        if (!vm->slots[slot_id].in_use ||
            vm->slots[slot_id].generation != (u32_t)generation)
                return ESTALE;

        saved = vm->slots[slot_id];
        memset(&vm->slots[slot_id], 0, sizeof(vm->slots[slot_id]));
        r = mxv_ept_rebuild(vm_index);
        if (r != OK) {
                vm->slots[slot_id] = saved;
                (void)mxv_ept_rebuild(vm_index);
                return r;
        }
        pin = mxv_pin_lookup(owner, saved.pin_token, NULL);
        if (pin != NULL)
                pin->attached = 0;
        return OK;
}

u64_t
mxv_vm_eptp(endpoint_t owner, u64_t vm_handle)
{
        struct mxv_vm *vm = mxv_vm_lookup(owner, vm_handle, NULL);

        return vm == NULL ? 0 : vm->eptp;
}

void
mxv_owner_died(endpoint_t owner)
{
        unsigned int i;

        if (owner == VM_PROC_NR) {
                memset(mxv_vms, 0, sizeof(mxv_vms));
                memset(mxv_pins, 0, sizeof(mxv_pins));
                memset(mxv_ept_pages, 0, sizeof(mxv_ept_pages));
                return;
        }

        for (i = 0; i < MXV_MAX_VMS; i++) {
                if (mxv_vms[i].in_use && mxv_vms[i].owner == owner) {
                        memset(&mxv_vms[i], 0, sizeof(mxv_vms[i]));
                        memset(mxv_ept_pages[i], 0,
                            sizeof(mxv_ept_pages[i]));
                }
        }
        for (i = 0; i < MXV_MAX_PINS; i++) {
                if (mxv_pins[i].in_use && mxv_pins[i].owner == owner)
                        memset(&mxv_pins[i], 0, sizeof(mxv_pins[i]));
        }
}
