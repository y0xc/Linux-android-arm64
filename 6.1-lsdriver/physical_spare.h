#ifndef READ_PHYSICAL_H
#define READ_PHYSICAL_H


#include <linux/kernel.h> 
#include <linux/module.h> 
#include <linux/init.h>   
#include <linux/types.h> 
#include <linux/errno.h>  


#include <linux/mm.h>     
#include <linux/sched/mm.h> 
#include <linux/io.h>     
#include <linux/slab.h>    
#include <linux/uaccess.h> 
#include <asm/pgtable.h>   


#include <linux/sched.h> 
#include <linux/pid.h> 


#include <linux/fs.h>   
#include <linux/rculist.h> 
#include <linux/string.h> 
#include <linux/printk.h>



static int read_physical_memory_safe(phys_addr_t paddr, void *buffer, size_t size)
{
    //检查物理地址是否有效
    unsigned long pfn = __phys_to_pfn(paddr);
    if (unlikely(!pfn_valid(pfn)))
    {
        return -EINVAL;
    }

    // 直接利用内核线性映射获取虚拟地址
    // phys_to_virt 只是简单的数学运算: paddr + PAGE_OFFSET
    void *kernel_vaddr = (void *)phys_to_virt(paddr);

    //直接拷贝
    memcpy(buffer, kernel_vaddr, size);

    return 0;
}
static int write_physical_memory_safe(phys_addr_t paddr, void *buffer, size_t size)
{
    if (unlikely(!pfn_valid(__phys_to_pfn(paddr))))
    {
        return -EINVAL;
    }
    void *kernel_vaddr = (void *)phys_to_virt(paddr);
    memcpy(kernel_vaddr, buffer, size);

    return 0;
}


static inline int manual_va_to_pa_arm64(pid_t pid, unsigned long long vaddr, phys_addr_t *paddr)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct pid *pid_struct;
    pgd_t *pgd;
    p4d_t *p4d; 
    pud_t *pud;
    pmd_t *pmd;
    pte_t *ptep;
    int ret = 0;

    if (!paddr)
        return -EINVAL;
    *paddr = 0;

    pid_struct = find_get_pid(pid);
    if (!pid_struct)
    {
        pr_debug("manual_va_to_pa_arm64: 找不到 PID %d 的 pid_struct\n", pid);
        return -ESRCH;
    }
    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if (!task)
    {
        pr_debug("manual_va_to_pa_arm64: 找不到 PID %d 的 task_struct\n", pid);
        return -ESRCH;
    }
    mm = get_task_mm(task);
    put_task_struct(task);
    if (!mm)
    {
        pr_debug("manual_va_to_pa_arm64: PID %d 是一个没有 mm_struct 的内核线程\n", pid);
        return -EINVAL;
    }

    mmap_read_lock(mm);

    pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
    {
        ret = -ENOENT;
        goto out_unlock;
    }

    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
    {
        ret = -ENOENT;
        goto out_unlock;
    }

    pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud) || pud_bad(*pud))
    {
        ret = -ENOENT;
        goto out_unlock;
    }

    if (pud_leaf(*pud))
    {
        *paddr = (pud_pfn(*pud) << PAGE_SHIFT) + (vaddr & ~PUD_MASK);
        goto out_unlock;
    }

    pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
    {
        ret = -ENOENT;
        goto out_unlock;
    }

    if (pmd_leaf(*pmd))
    {
        *paddr = (pmd_pfn(*pmd) << PAGE_SHIFT) + (vaddr & ~PMD_MASK);
        goto out_unlock;
    }

    ptep = pte_offset_map(pmd, vaddr);
    if (!ptep)
    {
        ret = -ENOENT;
        goto out_unlock;
    }
    if (!pte_present(*ptep))
    {
        ret = -ENOENT;
        pte_unmap(ptep);
        goto out_unlock;
    }

    *paddr = (pte_pfn(*ptep) << PAGE_SHIFT) + (vaddr & ~PAGE_MASK);
    pte_unmap(ptep);

out_unlock:
    mmap_read_unlock(mm);
    mmput(mm);
    return ret;
}

static inline int ReadProcessMemory(pid_t pid, unsigned long long vaddr, void *buffer, size_t size)
{
    phys_addr_t current_paddr = 0;
    unsigned long long current_vaddr = vaddr;
    size_t bytes_remaining = size;
    size_t bytes_copied = 0;
    int status = 0;

    if (!buffer || size == 0)
        return -EINVAL;

    while (bytes_remaining > 0)
    {
        size_t page_offset = current_vaddr & ~PAGE_MASK;
        size_t bytes_to_read_this_page = PAGE_SIZE - page_offset;

        status = manual_va_to_pa_arm64(pid, current_vaddr, &current_paddr);
        if (status != 0)
            return status;

        if (bytes_to_read_this_page > bytes_remaining)
        {
            bytes_to_read_this_page = bytes_remaining;
        }

        status = read_physical_memory_safe(current_paddr, (char *)buffer + bytes_copied, bytes_to_read_this_page);
        if (status != 0)
            return status;

        bytes_remaining -= bytes_to_read_this_page;
        bytes_copied += bytes_to_read_this_page;
        current_vaddr += bytes_to_read_this_page;
    }
    return 0;
}

static inline int WriteProcessMemory(pid_t pid, unsigned long long vaddr, void *buffer, size_t size)
{
    phys_addr_t current_paddr = 0;
    unsigned long long current_vaddr = vaddr;
    size_t bytes_remaining = size;
    size_t bytes_written = 0;
    int status = 0;

    if (!buffer || size == 0)
        return -EINVAL;

    while (bytes_remaining > 0)
    {
        size_t page_offset = current_vaddr & ~PAGE_MASK;
        size_t bytes_to_write_this_page = PAGE_SIZE - page_offset;

        status = manual_va_to_pa_arm64(pid, current_vaddr, &current_paddr);
        if (status != 0)
            return status;

        if (bytes_to_write_this_page > bytes_remaining)
        {
            bytes_to_write_this_page = bytes_remaining;
        }

        status = write_physical_memory_safe(current_paddr, (char *)buffer + bytes_written, bytes_to_write_this_page);
        if (status != 0)
            return status;

        bytes_remaining -= bytes_to_write_this_page;
        bytes_written += bytes_to_write_this_page;
        current_vaddr += bytes_to_write_this_page;
    }
    return 0;
}


#endif // READ_PHYSICAL_H