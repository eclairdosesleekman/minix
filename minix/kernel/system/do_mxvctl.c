/*
 * MXV kernel control call.
 *
 * Phase-1 VMX controls and Phase-2 RAM/EPT controls remain restricted to RS.
 * A later mxvd capability will replace this temporary control-plane gate.
 */
#include "kernel/system.h"
#include "arch_proto.h"
#include <minix/mxvctl.h>
#include <errno.h>

int
do_mxvctl(struct proc *caller, message *m_ptr)
{
        u64_t value, generation;
        u64_t size;
        int r;

        if (caller->p_nr != RS_PROC_NR)
                return EPERM;
        if (m_ptr->m1_i1 != MXVCTL_API_VERSION)
                return EINVAL;

        value = m_ptr->m1_ull1;

        switch (m_ptr->m1_i2) {
        case MXV_QUERY_CAPS:
                m_ptr->m1_ull1 = mxv_vmx_capabilities();
                return OK;
        case MXV_VMXON:
                return mxv_vmx_cpu_enable();
        case MXV_VMXOFF:
                return mxv_vmx_cpu_disable();
        case MXV_VCPU_CREATE:
                r = mxv_vcpu_create(caller->p_endpoint, &value);
                if (r == OK)
                        m_ptr->m1_ull1 = value;
                return r;
        case MXV_VCPU_DESTROY:
                return mxv_vcpu_destroy(caller->p_endpoint, value);
        case MXV_VCPU_RUN_PROBE:
                r = mxv_vcpu_run_probe(caller->p_endpoint, value, &value);
                if (r == OK)
                        m_ptr->m1_ull1 = value;
                return r;

        case MXV_PIN_RANGE:
                if (m_ptr->m9_ull1 != (u64_t)caller->p_endpoint ||
                    m_ptr->m9_l1 <= 0)
                        return EINVAL;
                size = (u64_t)m_ptr->m9_l1;
                r = mxv_pin_range(caller->p_endpoint,
                    (vir_bytes)m_ptr->m9_ull2, size, &value);
                if (r == OK)
                        m_ptr->m9_ull1 = value;
                return r;
        case MXV_PIN_RELEASE:
                return mxv_pin_release(caller->p_endpoint, m_ptr->m9_ull1);
        case MXV_VM_CREATE:
                r = mxv_vm_create(caller->p_endpoint, &value);
                if (r == OK)
                        m_ptr->m9_ull1 = value;
                return r;
        case MXV_VM_DESTROY:
                return mxv_vm_destroy(caller->p_endpoint, m_ptr->m9_ull1);
        case MXV_MEM_ATTACH:
                if (m_ptr->m9_l2 <= 0 || m_ptr->m9_l4 < 0)
                        return EINVAL;
                size = (u64_t)m_ptr->m9_l2;
                r = mxv_mem_attach(caller->p_endpoint,
                    m_ptr->m9_ull1, m_ptr->m9_ull2,
                    (u64_t)m_ptr->m9_l1, size,
                    (u32_t)m_ptr->m9_l3, (u32_t)m_ptr->m9_l4,
                    &generation);
                if (r == OK)
                        m_ptr->m9_l5 = (long)generation;
                return r;
        case MXV_MEM_DETACH:
                if (m_ptr->m9_l4 < 0 || m_ptr->m9_l5 <= 0)
                        return EINVAL;
                return mxv_mem_detach(caller->p_endpoint,
                    m_ptr->m9_ull1, (u32_t)m_ptr->m9_l4,
                    (u64_t)m_ptr->m9_l5);
        default:
                return EINVAL;
        }
}
