
#ifndef _EXPORT_FUN_H_ // 检查这个宏是否还没有被定义
#define _EXPORT_FUN_H_ // 如果没有，就立刻定义它


#include <linux/version.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>



#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0) || LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)



static unsigned long (*kallsyms_lookup_name_sym)(const char *name);

static inline int _kallsyms_lookup_kprobe(struct kprobe *p, struct pt_regs *regs)
{
        return 0;
}

static inline unsigned long get_kallsyms_func(void)
{
        struct kprobe probe;
        int ret;
        unsigned long addr;

        memset(&probe, 0, sizeof(probe));
        probe.pre_handler = _kallsyms_lookup_kprobe;
        probe.symbol_name = "kallsyms_lookup_name";
        ret = register_kprobe(&probe);
        if (ret)
                return 0;
        addr = (unsigned long)probe.addr;
        unregister_kprobe(&probe);
        return addr;
}

static inline unsigned long generic_kallsyms_lookup_name(const char *name)
{
        /* singleton */
        if (!kallsyms_lookup_name_sym) {
                kallsyms_lookup_name_sym = (void *)get_kallsyms_func();
                if(!kallsyms_lookup_name_sym)
                        return 0;
        }
        return kallsyms_lookup_name_sym(name);
}

#else

static inline unsigned long generic_kallsyms_lookup_name(const char *name)
{
    return kallsyms_lookup_name(name);
}

#endif 





#endif /* _EXPORT_FUN_H_ */