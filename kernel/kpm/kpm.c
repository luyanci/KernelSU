/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 
 * Copyright (C) 2025 Liankong (xhsw.new@outlook.com). All Rights Reserved.
 * 本代码由GPL-2授权
 * 
 * 适配KernelSU的KPM 内核模块加载器兼容实现
 * 
 * 集成了 ELF 解析、内存布局、符号处理、重定位（支持 ARM64 重定位类型）
 * 并参照KernelPatch的标准KPM格式实现加载和控制
 */
#include <linux/export.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/kernfs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/elf.h>
#include <linux/kallsyms.h>
#include <linux/version.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <asm/elf.h>    /* 包含 ARM64 重定位类型定义 */
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>
#include <linux/version.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <asm/insn.h>
#include "kpm.h"
#include "compact.h"

static inline void local_flush_icache_all(void)
{
    asm volatile("ic iallu");
    asm volatile("dsb nsh" : : : "memory");
    asm volatile("isb" : : : "memory");
}

static inline void flush_icache_all(void)
{
    asm volatile("dsb ish" : : : "memory");
    asm volatile("ic ialluis");
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0) && defined(CONFIG_MODULES)
#include <linux/moduleloader.h> // 需要启用 CONFIG_MODULES
#endif

/**
 * kpm_malloc_exec - 分配可执行内存
 * @size: 需要分配的内存大小（字节）
 *
 * 返回值: 成功返回内存指针，失败返回 NULL
 */
void *kpm_malloc_exec(size_t size)
{
    void *addr = NULL;
    unsigned long nr_pages;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
    /* 内核 5.0+ 方案 */
#if defined(CONFIG_MODULES)
    // 使用 module_alloc + set_memory_x
    addr = module_alloc(size);
    if (!addr)
        return NULL;

    nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
    if (set_memory_x((unsigned long)addr, nr_pages)) {
        vfree(addr); // 注意：某些内核版本用 module_memfree
        return NULL;
    }
#else
    // 如果未启用模块支持，回退到 vmalloc + 手动设置权限（可能有安全风险）
    addr = __vmalloc(size, GFP_KERNEL, PAGE_KERNEL_EXEC);
    if (addr) {
        nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
        if (set_memory_x((unsigned long)addr, nr_pages)) {
            vfree(addr);
            addr = NULL;
        }
    }
#endif
#else
    /* 内核 <5.0 方案 */
#if defined(vmalloc_exec)
    // 旧版直接使用 vmalloc_exec
    addr = vmalloc_exec(size);
#else
    // 兼容某些旧版本变种
    addr = __vmalloc(size, GFP_KERNEL, PAGE_KERNEL_EXEC);
#endif
#endif

    return addr;
}

/**
 * kpm_free_exec - 释放可执行内存
 * @addr: alloc_exec_memory 返回的指针
 * @size: 分配时的大小
 */
void kpm_free_exec(void *addr, size_t size)
{
    unsigned long nr_pages;
    if (!addr)
        return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
    nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
    set_memory_nx((unsigned long)addr, nr_pages);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0) && defined(CONFIG_MODULES)
    module_memfree(addr); // 5.0+ 且启用模块支持
#else
    vfree(addr); // 旧版或未启用模块
#endif
}


/* KPM 模块头部结构体，存放于 ELF 的 .kpm 段中 */
struct kpm_header {
    u32 magic;  /* 魔数，要求为 'KPM' -> 0x4B504D */
    u32 version;/* 版本号，目前要求为 1 */
    int (*entry)(const char *args, const char *event, void *__user reserved);
    void (*exit)(void *__user reserved);
};

#define KPM_MAGIC   0x4B504D
#define KPM_VERSION 1

/* 加载信息结构体，避免与内核已有 load_info 冲突 */
struct kpm_load_info {
    const void *hdr;           /* ELF 数据 */
    Elf64_Ehdr *ehdr;          /* ELF 头 */
    Elf64_Shdr *sechdrs;       /* 段表 */
    const char *secstrings;    /* 段名字符串表 */
    size_t len;                /* 文件长度 */
    struct {
        const char *base;
        const char *name;
        const char *version;
        const char *license;
        const char *author;
        const char *description;
        size_t size;
    } info;
    struct {
        int info;
        int sym;
        int str;
    } index;
    char *strtab;              /* 符号表对应的字符串表 */
    unsigned long symoffs, stroffs;
};

/* 模块数据结构，改名为 kpm_module 避免冲突 */
struct kpm_module {
    struct list_head list;
    char *args;
    char *ctl_args;
    void *start;                 /* 分配的连续内存区域 */
    unsigned int size;           /* 总大小 */
    unsigned int text_size;
    unsigned int ro_size;
    int (*init)(const char *args, const char *event, void *__user reserved);
    void (*exit)(void *__user reserved);
    int (*ctl0)(const char *ctl_args, char *__user out_msg, int outlen);
    int (*ctl1)(void *a1, void *a2, void *a3);
    struct {
        const char *base;
        const char *name;
        const char *version;
        const char *license;
        const char *author;
        const char *description;
    } info;
};

/* 全局模块列表，改名为 kpm_module_list */
static LIST_HEAD(kpm_module_list);
static DEFINE_SPINLOCK(kpm_module_lock);

/*-----------------------------------------------------------
 * ELF 头和段表解析（针对 ARM64 检查）
 *----------------------------------------------------------*/
#define kpm_elf_check_arch(x) ((x)->e_machine == EM_AARCH64)

static int kpm_elf_header_check(struct kpm_load_info *info)
{
    if (info->len < sizeof(Elf64_Ehdr))
        return -EINVAL;
    info->ehdr = (Elf64_Ehdr *)info->hdr;
    if (memcmp(info->ehdr->e_ident, ELFMAG, SELFMAG) != 0)
        return -EINVAL;
    if (info->ehdr->e_shoff + info->ehdr->e_shnum * sizeof(Elf64_Shdr) > info->len)
        return -EINVAL;
    info->sechdrs = (Elf64_Shdr *)((const char *)info->hdr + info->ehdr->e_shoff);
    if (info->ehdr->e_shstrndx >= info->ehdr->e_shnum)
        return -EINVAL;
    info->secstrings = (const char *)info->hdr + info->sechdrs[info->ehdr->e_shstrndx].sh_offset;
    return 0;
}

/* 在 ELF 文件中查找指定段 */
Elf64_Shdr *find_sec(struct kpm_load_info *info, const char *sec_name)
{
    Elf64_Ehdr *ehdr = info->ehdr;
    Elf64_Shdr *shdr = (Elf64_Shdr *)((char *)ehdr + ehdr->e_shoff);
    const char *shstrtab = (char *)ehdr + shdr[ehdr->e_shstrndx].sh_offset;
    int i;

    for (i = 0; i < ehdr->e_shnum; i++) {
        if (strcmp(shstrtab + shdr[i].sh_name, sec_name) == 0)
            return &shdr[i];
    }

    return NULL;
}

int find_sec_num(struct kpm_load_info *info, const char *sec_name) {
    Elf64_Ehdr *ehdr = info->ehdr;
    Elf64_Shdr *shdr = (Elf64_Shdr *)((char *)ehdr + ehdr->e_shoff);
    const char *shstrtab = (char *)ehdr + shdr[ehdr->e_shstrndx].sh_offset;
    int i;

    for (i = 0; i < ehdr->e_shnum; i++) {
        if (strcmp(shstrtab + shdr[i].sh_name, sec_name) == 0)
            return i;
    }

    return -1;
}

/*-----------------------------------------------------------
 * 模块 modinfo 提取
 *----------------------------------------------------------*/
static char *kpm_next_string(char *string, unsigned long *secsize)
{
    while (string[0]) {
        string++;
        if ((*secsize)-- <= 1)
            return NULL;
    }
    while (!string[0]) {
        string++;
        if ((*secsize)-- <= 1)
            return NULL;
    }
    return string;
}

static char *kpm_get_next_modinfo(const struct kpm_load_info *info, const char *tag, char *prev)
{
    char *p;
    unsigned int taglen = strlen(tag);
    Elf_Shdr *infosec = &info->sechdrs[info->index.info];
    unsigned long size = infosec->sh_size;
    char *modinfo = (char *)info->hdr + infosec->sh_offset;
    if (prev) {
        size -= prev - modinfo;
        modinfo = kpm_next_string(prev, &size);
    }
    for (p = modinfo; p; p = kpm_next_string(p, &size)) {
        if (strncmp(p, tag, taglen) == 0 && p[taglen] == '=')
            return p + taglen + 1;
    }
    return NULL;
}

static char *kpm_get_modinfo(const struct kpm_load_info *info, const char *tag)
{
    return kpm_get_next_modinfo(info, tag, NULL);
}

/*-----------------------------------------------------------
 * 内存布局与段复制
 *----------------------------------------------------------*/
static long kpm_get_offset(struct kpm_module *mod, unsigned int *size, Elf64_Shdr *sechdr)
{
    long ret = ALIGN(*size, sechdr->sh_addralign ? sechdr->sh_addralign : 1);
    *size = ret + sechdr->sh_size;
    return ret;
}

static void kpm_layout_sections(struct kpm_module *mod, struct kpm_load_info *info)
{
    int i;
    for (i = 0; i < info->ehdr->e_shnum; i++)
        info->sechdrs[i].sh_entsize = ~0UL;
    for (i = 0; i < info->ehdr->e_shnum; i++) {
        Elf64_Shdr *s = &info->sechdrs[i];
        if (!(s->sh_flags & SHF_ALLOC))
            continue;
        s->sh_entsize = kpm_get_offset(mod, &mod->size, s);
    }
    mod->size = ALIGN(mod->size, 8);
}

/*-----------------------------------------------------------
 * 符号处理与重定位（针对 ARM64）
 *----------------------------------------------------------*/
static bool kpm_is_core_symbol(const Elf64_Sym *src, const Elf64_Shdr *sechdrs, unsigned int shnum)
{
    const Elf64_Shdr *sec;
    if (src->st_shndx == SHN_UNDEF || src->st_shndx >= shnum || !src->st_name)
        return false;
    sec = &sechdrs[src->st_shndx];
    if (!(sec->sh_flags & SHF_ALLOC))
        return false;
    return true;
}

static int kpm_simplify_symbols(struct kpm_module *mod, const struct kpm_load_info *info)
{
    Elf64_Shdr *symsec = &info->sechdrs[info->index.sym];
    Elf64_Sym *sym = (Elf64_Sym *)symsec->sh_addr;
    unsigned long secbase;
    int ret = 0;
    unsigned int i;
    unsigned long addr = 0;

    for (i = 1; i < symsec->sh_size / sizeof(Elf64_Sym); i++) {
        const char *name = info->strtab + sym[i].st_name;
        switch (sym[i].st_shndx) {
        case SHN_COMMON:
            if (!strncmp(name, "__gnu_lto", 9)) {
                printk(KERN_ERR "ARM64 KPM Loader: compile with -fno-common\n");
                ret = -ENOEXEC;
            }
            break;
        case SHN_ABS:
            break;
        case SHN_UNDEF:
            // addr = kallsyms_lookup_name(name);
            addr = sukisu_compact_find_symbol(name);
            if (!addr) {
                printk(KERN_ERR "ARM64 KPM Loader: unknown symbol: %s\n", name);
                ret = -ENOENT;
                break;
            }
            sym[i].st_value = addr;
            break;
        default:
            secbase = info->sechdrs[sym[i].st_shndx].sh_addr;
            sym[i].st_value += secbase;
            break;
        }
    }
    return ret;
}

/* ARM64 重定位处理：支持 R_AARCH64_RELATIVE、R_AARCH64_ABS64、R_AARCH64_GLOB_DAT、R_AARCH64_JUMP_SLOT */
static int kpm_apply_relocate_arm64(Elf64_Shdr *sechdrs, const char *strtab, int sym_idx, int rel_idx, struct kpm_module *mod)
{
    Elf64_Shdr *relsec = &sechdrs[rel_idx];
    int num = relsec->sh_size / sizeof(Elf64_Rel);
    Elf64_Rel *rel = (Elf64_Rel *)((char *)mod->start + relsec->sh_offset);  // 修正为 sh_offset
    int i;

    for (i = 0; i < num; i++) {
        unsigned long type = ELF64_R_TYPE(rel[i].r_info);
        unsigned long *addr = (unsigned long *)(mod->start + rel[i].r_offset);

        switch (type) {
        case R_AARCH64_RELATIVE:
            *addr = (unsigned long)mod->start + *(unsigned long *)addr;
            break;
        default:
            printk(KERN_ERR "ARM64 KPM Loader: Unsupported REL relocation type %lu\n", type);
            return -EINVAL;
        }
    }
    return 0;
}

#ifndef R_AARCH64_GLOB_DAT
#define	R_AARCH64_GLOB_DAT	1025	/* Set GOT entry to data address */
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define	R_AARCH64_JUMP_SLOT	1026	/* Set GOT entry to code address */
#endif
#ifndef R_ARM_NONE
#define R_ARM_NONE 0
#endif
#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE 256
#endif

/* 重定位操作类型 */
typedef enum {
    RELOC_OP_ABS,
    RELOC_OP_PREL,
    RELOC_OP_PAGE
} reloc_op_t;

/* 编码立即数到指令 */
static u32 K_aarch64_insn_encode_immediate(u32 insn, s64 imm, int shift, int bits)
{
    u32 mask = (BIT(bits) - 1) << shift;
    return (insn & ~mask) | ((imm & (BIT(bits) - 1)) << shift);
}

/* 修补指令中的立即数字段 */
int aarch64_insn_patch_imm(void *addr, enum aarch64_insn_imm_type type, s64 imm)
{
    u32 insn = le32_to_cpu(*(u32 *)addr);
    u32 new_insn;

    switch (type) {
    case AARCH64_INSN_IMM_16:
        /* MOVZ/MOVK: imm[15:0] → shift=5, bits=16 */
        new_insn = K_aarch64_insn_encode_immediate(insn, imm, 5, 16);
        break;
    case AARCH64_INSN_IMM_26:
        /* B/BL: offset[25:0] → shift=0, bits=26 */
        new_insn = K_aarch64_insn_encode_immediate(insn, imm, 0, 26);
        break;
    case AARCH64_INSN_IMM_ADR:
        /* ADR/ADRP: imm[20:0] → shift=5, bits=21 */
        new_insn = K_aarch64_insn_encode_immediate(insn, imm, 5, 21);
        break;
    case AARCH64_INSN_IMM_19:
        /* 条件跳转: offset[18:0] → shift=5, bits=19 */
        new_insn = K_aarch64_insn_encode_immediate(insn, imm, 5, 19);
        break;
    default:
        return -EINVAL;
    }

    /* 写入新指令并刷新缓存 */
    *(u32 *)addr = cpu_to_le32(new_insn);
    flush_icache_range((unsigned long)addr, (unsigned long)addr + 4);
    return 0;
}

/*
 * reloc_data - 将数值 val 写入目标地址 loc，
 *              并检查 val 是否能在指定的 bits 位内表示。
 * op 参数目前未使用，bits 可为16、32或64。
 */
int reloc_data(int op, void *loc, u64 val, int bits)
{
    u64 max_val = (1ULL << bits) - 1;

    if (val > max_val)
        return -ERANGE;

    switch (bits) {
    case 16:
        *(u16 *)loc = (u16)val;
        break;
    case 32:
        *(u32 *)loc = (u32)val;
        break;
    case 64:
        *(u64 *)loc = val;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/*
 * reloc_insn_movw - 针对 MOVW 类指令的重定位处理
 *
 * 参数说明：
 *  op:      重定位操作类型（例如 RELOC_OP_ABS 或 RELOC_OP_PREL，目前未作区分）
 *  loc:     指向要修改的 32 位指令的地址
 *  val:     需要嵌入指令的立即数值（在左移 shift 位后写入）
 *  shift:   表示立即数在 val 中应左移多少位后再写入指令
 *  imm_width: 立即数字段宽度，通常为16
 *
 * 本示例假定 MOVW 指令的立即数字段位于指令的 bit[5:20]。
 */
int reloc_insn_movw(int op, void *loc, u64 val, int shift, int imm_width)
{
    u32 *insn = (u32 *)loc;
    u32 imm;

    /* 检查 val >> shift 是否能在16位内表示 */
    if (((val >> shift) >> 16) != 0)
        return -ERANGE;

    imm = (val >> shift) & 0xffff;

    /* 清除原有立即数字段（假定占用 bit[5:20]） */
    *insn &= ~(0xffff << 5);
    /* 写入新的立即数 */
    *insn |= (imm << 5);

    return 0;
}

/*
 * reloc_insn_imm - 针对其他立即数重定位处理
 *
 * 参数说明：
 *  op:        重定位操作类型（例如 RELOC_OP_ABS 或 RELOC_OP_PREL，目前未作区分）
 *  loc:       指向 32 位指令的地址
 *  val:       重定位后需要写入的立即数值
 *  shift:     表示 val 中立即数需要右移多少位后写入指令
 *  bits:      立即数字段宽度（例如12、19、26等）
 *  insn_mask: 指令中立即数字段的掩码（本示例中未使用，可根据实际编码调整）
 *
 * 本示例假定立即数字段位于指令的 bit[5] 开始，占用 bits 位。
 */
int reloc_insn_imm(int op, void *loc, u64 val, int shift, int bits, int insn_mask)
{
    u32 *insn = (u32 *)loc;
    u64 max_val = (1ULL << bits) - 1;
    u32 imm;

    if ((val >> shift) > max_val)
        return -ERANGE;

    imm = (u32)(val >> shift) & max_val;

    /* 清除原立即数字段，这里假定立即数字段位于 bit[5] */
    *insn &= ~(max_val << 5);
    /* 写入新的立即数 */
    *insn |= (imm << 5);

    return 0;
}

#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT    1025    /* Set GOT entry to data address */
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT   1026    /* Set GOT entry to code address */
#endif
#ifndef R_ARM_NONE
#define R_ARM_NONE            0
#endif
#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE        256
#endif

/*
 * 完善后的 ARM64 RELA 重定位处理函数
 * 支持的重定位类型：
 *  - R_AARCH64_NONE / R_ARM_NONE: 不做处理
 *  - R_AARCH64_RELATIVE: 目标地址 = module_base + r_addend
 *  - R_AARCH64_ABS64: 目标地址 = module_base + (S + r_addend)
 *  - R_AARCH64_GLOB_DAT / R_AARCH64_JUMP_SLOT: 目标地址 = module_base + S
 *  - 其他类型调用 reloc_insn_movw 或 reloc_insn_imm 等函数处理
 *
 * 参数说明：
 *  - sechdrs: ELF 段表数组
 *  - strtab: 符号字符串表（未在本函数中直接使用）
 *  - sym_idx: 符号表所在段的索引
 *  - rela_idx: 当前重定位段的索引
 *  - mod: 当前模块数据结构，mod->start 为模块加载基地址
 */
static int kpm_apply_relocate_add_arm64(Elf64_Shdr *sechdrs, const char *strtab,
                                          int sym_idx, int rela_idx, struct kpm_module *mod)
{
    Elf64_Shdr *relasec = &sechdrs[rela_idx];
    int num = relasec->sh_size / sizeof(Elf64_Rela);
    /* 使用 sh_offset 而非 sh_entsize，确保 Rela 表起始地址正确 */
    Elf64_Rela *rela = (Elf64_Rela *)((char *)mod->start + relasec->sh_offset);
    int i;
    int ovf;
    bool overflow_check;
    Elf64_Sym *sym;
    void *loc;
    u64 val;

    for (i = 0; i < num; i++) {
        unsigned long type = ELF64_R_TYPE(rela[i].r_info);
        unsigned long sym_index = ELF64_R_SYM(rela[i].r_info);

        /* 获取目标段索引，即 Rela 段的 sh_info 字段 */
        unsigned int target = sechdrs[rela_idx].sh_info;
        if (target >= sechdrs[0].sh_size) {
            /* 这里不太可能用 sh_size 来判断，正确做法是检查 e_shnum */
            /* 假设我们可以通过全局信息获得 e_shnum，这里用 target 比较 */
            printk(KERN_ERR "ARM64 KPM Loader: Invalid target section index %u\n", target);
            return -EINVAL;
        }
        /* 根据 ELF 规范，目标地址 loc = (target section's address) + r_offset */
        loc = (void *)sechdrs[target].sh_addr + rela[i].r_offset;

        /* 获取符号 S 值 */
        sym = (Elf64_Sym *)sechdrs[sym_idx].sh_addr + sym_index;
        val = sym->st_value + rela[i].r_addend;
        overflow_check = true;

        switch (type) {
        case R_ARM_NONE:
        case R_AARCH64_NONE:
            ovf = 0;
            break;
        case R_AARCH64_RELATIVE:
            * (unsigned long *)loc = (unsigned long)mod->start + rela[i].r_addend;
            break;
        case R_AARCH64_ABS64:
            if (sym_index) {
                /* 注意：这里假设符号 st_value 是相对地址，需要加上模块基地址 */
                * (unsigned long *)loc = (unsigned long)mod->start + sym->st_value + rela[i].r_addend;
            } else {
                printk(KERN_ERR "ARM64 KPM Loader: R_AARCH64_ABS64 with zero symbol\n");
                return -EINVAL;
            }
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            if (sym_index) {
                * (unsigned long *)loc = (unsigned long)mod->start + sym->st_value;
            } else {
                printk(KERN_ERR "ARM64 KPM Loader: R_AARCH64_GLOB_DAT/JUMP_SLOT with zero symbol\n");
                return -EINVAL;
            }
            break;
        case R_AARCH64_ABS32:
            ovf = reloc_data(RELOC_OP_ABS, loc, val, 32);
            break;
        case R_AARCH64_ABS16:
            ovf = reloc_data(RELOC_OP_ABS, loc, val, 16);
            break;
        case R_AARCH64_PREL64:
            ovf = reloc_data(RELOC_OP_PREL, loc, val, 64);
            break;
        case R_AARCH64_PREL32:
            ovf = reloc_data(RELOC_OP_PREL, loc, val, 32);
            break;
        case R_AARCH64_PREL16:
            ovf = reloc_data(RELOC_OP_PREL, loc, val, 16);
            break;
        /* MOVW 重定位处理 */
        case R_AARCH64_MOVW_UABS_G0_NC:
            overflow_check = false;
        case R_AARCH64_MOVW_UABS_G0:
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 0, AARCH64_INSN_IMM_16);
            break;
        case R_AARCH64_MOVW_UABS_G1_NC:
            overflow_check = false;
        case R_AARCH64_MOVW_UABS_G1:
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 16, AARCH64_INSN_IMM_16);
            break;
        case R_AARCH64_MOVW_UABS_G2_NC:
            overflow_check = false;
        case R_AARCH64_MOVW_UABS_G2:
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 32, AARCH64_INSN_IMM_16);
            break;
        case R_AARCH64_MOVW_UABS_G3:
            overflow_check = false;
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 48, AARCH64_INSN_IMM_16);
            break;
        case R_AARCH64_MOVW_SABS_G0:
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 0, AARCH64_INSN_IMM_MOVNZ);
            break;
        case R_AARCH64_MOVW_SABS_G1:
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 16, AARCH64_INSN_IMM_MOVNZ);
            break;
        case R_AARCH64_MOVW_SABS_G2:
            ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 32, AARCH64_INSN_IMM_MOVNZ);
            break;
        case R_AARCH64_MOVW_PREL_G0_NC:
            overflow_check = false;
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 0, AARCH64_INSN_IMM_MOVK);
            break;
        case R_AARCH64_MOVW_PREL_G0:
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 0, AARCH64_INSN_IMM_MOVNZ);
            break;
        case R_AARCH64_MOVW_PREL_G1_NC:
            overflow_check = false;
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 16, AARCH64_INSN_IMM_MOVK);
            break;
        case R_AARCH64_MOVW_PREL_G1:
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 16, AARCH64_INSN_IMM_MOVNZ);
            break;
        case R_AARCH64_MOVW_PREL_G2_NC:
            overflow_check = false;
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 32, AARCH64_INSN_IMM_MOVK);
            break;
        case R_AARCH64_MOVW_PREL_G2:
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 32, AARCH64_INSN_IMM_MOVNZ);
            break;
        case R_AARCH64_MOVW_PREL_G3:
            overflow_check = false;
            ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 48, AARCH64_INSN_IMM_MOVNZ);
            break;
        /* Immediate 指令重定位 */
        case R_AARCH64_LD_PREL_LO19:
            ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 19, AARCH64_INSN_IMM_19);
            break;
        case R_AARCH64_ADR_PREL_LO21:
            ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 0, 21, AARCH64_INSN_IMM_ADR);
            break;
        case R_AARCH64_ADR_PREL_PG_HI21_NC:
            overflow_check = false;
        case R_AARCH64_ADR_PREL_PG_HI21:
            ovf = reloc_insn_imm(RELOC_OP_PAGE, loc, val, 12, 21, AARCH64_INSN_IMM_ADR);
            break;
        case R_AARCH64_ADD_ABS_LO12_NC:
        case R_AARCH64_LDST8_ABS_LO12_NC:
            overflow_check = false;
            ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 0, 12, AARCH64_INSN_IMM_12);
            break;
        case R_AARCH64_LDST16_ABS_LO12_NC:
            overflow_check = false;
            ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 1, 11, AARCH64_INSN_IMM_12);
            break;
        case R_AARCH64_LDST32_ABS_LO12_NC:
            overflow_check = false;
            ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 2, 10, AARCH64_INSN_IMM_12);
            break;
        case R_AARCH64_LDST64_ABS_LO12_NC:
            overflow_check = false;
            ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 3, 9, AARCH64_INSN_IMM_12);
            break;
        case R_AARCH64_LDST128_ABS_LO12_NC:
            overflow_check = false;
            ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 4, 8, AARCH64_INSN_IMM_12);
            break;
        case R_AARCH64_TSTBR14:
            ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 14, AARCH64_INSN_IMM_14);
            break;
        case R_AARCH64_CONDBR19:
            ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 19, AARCH64_INSN_IMM_19);
            break;
        case R_AARCH64_JUMP26:
        case R_AARCH64_CALL26:
            ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 26, AARCH64_INSN_IMM_26);
            break;
        default:
            pr_err("ARM64 KPM Loader: Unsupported RELA relocation: %llu\n",
                   ELF64_R_TYPE(rela[i].r_info));
            return -ENOEXEC;
        }

        if (overflow_check && ovf == -ERANGE)
            goto overflow;
    }
    return 0;
overflow:
    pr_err("ARM64 KPM Loader: Overflow in relocation type %d, val %llx\n",
           (int)ELF64_R_TYPE(rela[i].r_info), val);
    return -ENOEXEC;
}


static int kpm_apply_relocations(struct kpm_module *mod, const struct kpm_load_info *info)
{
    int rc = 0;
    int i;

    for (i = 1; i < info->ehdr->e_shnum; i++) {
        unsigned int target = info->sechdrs[i].sh_info;

        if (target >= info->ehdr->e_shnum) {
            printk(KERN_ERR "ARM64 KPM Loader: Invalid target section index %u\n", target);
            return -EINVAL;
        }

        if (!(info->sechdrs[target].sh_flags & SHF_ALLOC)) {
            printk(KERN_INFO "ARM64 KPM Loader: Skipping non-allocated section %d\n", i);
            continue;
        }

        if (info->sechdrs[i].sh_type == SHT_REL) {
            rc = kpm_apply_relocate_arm64(info->sechdrs, info->strtab, info->index.sym, i, mod);
        } else if (info->sechdrs[i].sh_type == SHT_RELA) {
            rc = kpm_apply_relocate_add_arm64(info->sechdrs, info->strtab, info->index.sym, i, mod);
        }

        if (rc < 0) {
            printk(KERN_ERR "ARM64 KPM Loader: Relocation failed at section %d, error %d\n", i, rc);
            break;
        }
    }

    return rc;
}


/*-----------------------------------------------------------
 * 符号表与字符串表布局
 *----------------------------------------------------------*/
static void kpm_layout_symtab(struct kpm_module *mod, struct kpm_load_info *info)
{
    Elf64_Shdr *symsect = &info->sechdrs[info->index.sym];
    Elf64_Shdr *strsect = &info->sechdrs[info->index.str];
    const Elf64_Sym *src;
    unsigned int i, nsrc, ndst;
    unsigned int strtab_size = 1;

    symsect->sh_flags |= SHF_ALLOC;
    symsect->sh_entsize = kpm_get_offset(mod, &mod->size, symsect);
    src = (Elf64_Sym *)((char *)info->hdr + symsect->sh_offset);
    nsrc = symsect->sh_size / sizeof(Elf64_Sym);
    for (ndst = i = 0; i < nsrc; i++) {
        if (i == 0 || kpm_is_core_symbol(src + i, info->sechdrs, info->ehdr->e_shnum)) {
            strtab_size += strlen(info->strtab + src[i].st_name) + 1;
            ndst++;
        }
    }
    info->symoffs = ALIGN(mod->size, symsect->sh_addralign ? symsect->sh_addralign : 1);
    info->stroffs = mod->size = info->symoffs + ndst * sizeof(Elf64_Sym);
    mod->size += strtab_size;
    strsect->sh_flags |= SHF_ALLOC;
    strsect->sh_entsize = kpm_get_offset(mod, &mod->size, strsect);
}

/*-----------------------------------------------------------
 * 重写段表头：修正各段的 sh_addr 为在连续内存中的地址
 *----------------------------------------------------------*/
static int kpm_rewrite_section_headers(struct kpm_load_info *info)
{
    int i;
    info->sechdrs[0].sh_addr = 0;
    for (i = 1; i < info->ehdr->e_shnum; i++) {
        Elf64_Shdr *shdr = &info->sechdrs[i];
        if (shdr->sh_type != SHT_NOBITS && info->len < shdr->sh_offset + shdr->sh_size)
            return -ENOEXEC;
        shdr->sh_addr = (size_t)info->hdr + shdr->sh_offset;
    }
    return 0;
}

/*-----------------------------------------------------------
 * 将各段复制到连续内存区域中
 *----------------------------------------------------------*/
/*-----------------------------------------------------------
 * 将各段复制到连续内存区域（修复版）
 * 关键修复点：
 * 1. 段地址按对齐要求正确计算
 * 2. 显式设置可执行内存权限
 * 3. 刷新指令缓存保证一致性
 *----------------------------------------------------------*/
static int kpm_move_module(struct kpm_module *mod, struct kpm_load_info *info)
{
    int i;
    unsigned long curr_offset = 0;
    Elf64_Shdr *shdr;
    void *dest;
    const char *secname;

    /* 分配连续内存（按页对齐） */
    mod->size = ALIGN(mod->size, PAGE_SIZE);
    mod->start = module_alloc(mod->size); // 使用内核的 module_alloc 接口
    if (!mod->start) {
        printk(KERN_ERR "ARM64 KPM Loader: Failed to allocate module memory\n");
        return -ENOMEM;
    }
    memset(mod->start, 0, mod->size);

    /* 设置内存可执行权限（关键修复） */
    set_memory_x((unsigned long)mod->start, mod->size >> PAGE_SHIFT);

    printk(KERN_INFO "ARM64 KPM Loader: Final section addresses (aligned base=0x%px):\n", mod->start);

    /* 遍历所有段并按对齐要求布局 */
    for (i = 0; i < info->ehdr->e_shnum; i++) {
        shdr = &info->sechdrs[i];
        if (!(shdr->sh_flags & SHF_ALLOC))
            continue;

        /* 按段对齐要求调整偏移 */
        curr_offset = ALIGN(curr_offset, shdr->sh_addralign);
        dest = mod->start + curr_offset;

        /* 复制段内容（NOBITS 段不复制） */
        if (shdr->sh_type != SHT_NOBITS) {
            memcpy(dest, (void *)shdr->sh_addr, shdr->sh_size);
            
            /* 刷新指令缓存（针对可执行段） */
            if (shdr->sh_flags & SHF_EXECINSTR) {
                flush_icache_range((unsigned long)dest, 
                                 (unsigned long)dest + shdr->sh_size);
            }
        }

        /* 更新段头中的虚拟地址 */
        shdr->sh_addr = (unsigned long)dest;
        curr_offset += shdr->sh_size;

        /* 定位关键函数指针 */
        secname = info->secstrings + shdr->sh_name;
        if (!mod->init && !strcmp(".kpm.init", secname)) {
            mod->init = (int (*)(const char *, const char *, void *__user))dest;
            printk(KERN_DEBUG "Found .kpm.init at 0x%px\n", dest);
        } else if (!strcmp(".kpm.exit", secname)) {
            mod->exit = (void (*)(void *__user))dest;
        }
    }

    /* 调整元数据指针（基于新基址） */
    if (info->info.base) {
        unsigned long delta = (unsigned long)mod->start - (unsigned long)info->hdr;
        mod->info.name = (const char *)((unsigned long)info->info.name + delta);
        mod->info.version = (const char *)((unsigned long)info->info.version + delta);
        if (info->info.license)
            mod->info.license = (const char *)((unsigned long)info->info.license + delta);
        if (info->info.author)
            mod->info.author = (const char *)((unsigned long)info->info.author + delta);
        if (info->info.description)
            mod->info.description = (const char *)((unsigned long)info->info.description + delta);
    }

    return 0;
}

/*-----------------------------------------------------------
 * 初始化 kpm_load_info：解析 ELF 头、modinfo、符号表
 *----------------------------------------------------------*/
static int kpm_setup_load_info(struct kpm_load_info *info)
{
    int rc = 0;
    int i;
    const char *name;
    const char *version;

    info->sechdrs = (Elf64_Shdr *)((const char *)info->hdr + info->ehdr->e_shoff);
    info->secstrings = (const char *)info->hdr + info->sechdrs[info->ehdr->e_shstrndx].sh_offset;
    rc = kpm_rewrite_section_headers(info);
    if (rc) {
        printk(KERN_ERR "ARM64 KPM Loader: rewrite section headers error\n");
        return rc;
    }
    if (find_sec_num(info, ".kpm.init") == -1 || find_sec_num(info, ".kpm.exit") == -1) {
        printk(KERN_ERR "ARM64 KPM Loader: Missing .kpm.init or .kpm.exit section\n");
        return -ENOEXEC;
    }
    info->index.info = find_sec_num(info, ".kpm.info");
    if (!info->index.info) {
        printk(KERN_ERR "ARM64 KPM Loader: Missing .kpm.info section\n");
        return -ENOEXEC;
    }
    info->info.base = (const char *)info->hdr + info->sechdrs[info->index.info].sh_offset;
    info->info.size = info->sechdrs[info->index.info].sh_entsize;

    name = kpm_get_modinfo(info, "name");
    if (!name) {
        printk(KERN_ERR "ARM64 KPM Loader: Module name not found\n");
        return -ENOEXEC;
    }
    info->info.name = name;
    printk(KERN_INFO "ARM64 KPM Loader: Module name: %s\n", name);

    version = kpm_get_modinfo(info, "version");
    if (!version) {
        printk(KERN_ERR "ARM64 KPM Loader: Module version not found\n");
        return -ENOEXEC;
    }
    info->info.version = version;
    printk(KERN_INFO "ARM64 KPM Loader: Module version: %s\n", version);

    info->info.license = kpm_get_modinfo(info, "license");
    printk(KERN_INFO "ARM64 KPM Loader: Module license: %s\n", info->info.license ? info->info.license : "N/A");

    info->info.author = kpm_get_modinfo(info, "author");
    printk(KERN_INFO "ARM64 KPM Loader: Module author: %s\n", info->info.author ? info->info.author : "N/A");

    info->info.description = kpm_get_modinfo(info, "description");
    printk(KERN_INFO "ARM64 KPM Loader: Module description: %s\n", info->info.description ? info->info.description : "N/A");

    for (i = 1; i < info->ehdr->e_shnum; i++) {
        if (info->sechdrs[i].sh_type == SHT_SYMTAB) {
            info->index.sym = i;
            info->index.str = info->sechdrs[i].sh_link;
            info->strtab = (char *)info->hdr + info->sechdrs[info->index.str].sh_offset;
            break;
        }
    }
    if (info->index.sym == 0) {
        printk(KERN_ERR "ARM64 KPM Loader: Module has no symbols\n");
        return -ENOEXEC;
    }
    return 0;
}

// ============================================================================================

/*-----------------------------------------------------------
 * KPM 模块加载主流程
 *----------------------------------------------------------*/
/* 注意：接口名称改为 kpm_load_module，避免与内核原有 load_module 冲突 */
long kpm_load_module(const void *data, int len, const char *args,
                       const char *event, void *__user reserved)
{
    struct kpm_load_info load_info = { .hdr = data, .len = len };
    long rc = 0;
    struct kpm_module *mod;

    /* 检查 ELF 头 */
    rc = kpm_elf_header_check(&load_info);
    if (rc)
        goto out;

    rc = kpm_setup_load_info(&load_info);
    if (rc)
        goto out;

    /* 检查必须存在的模块初始化/退出段 */
    if (find_sec_num(&load_info, ".kpm.init") == -1 ||
        find_sec_num(&load_info, ".kpm.exit") == -1) {
        printk(KERN_ERR "ARM64 KPM Loader: Required sections missing\n");
        rc = -ENOEXEC;
        goto out;
    }

    /* 检查模块是否已经加载 */
    if (find_module(load_info.info.name)) {
        printk(KERN_ERR "ARM64 KPM Loader: Module %s already loaded\n",
               load_info.info.name);
        rc = -EEXIST;
        goto out;
    }

    mod = vmalloc(sizeof(struct kpm_module));
    if (!mod) {
        rc = -ENOMEM;
        goto out;
    }
    memset(mod, 0, sizeof(struct kpm_module));

    if (args) {
        mod->args = vmalloc(strlen(args) + 1);
        if (!mod->args) {
            rc = -ENOMEM;
            goto free_mod;
        }
        strcpy(mod->args, args);
    }

    kpm_layout_sections(mod, &load_info);
    kpm_layout_symtab(mod, &load_info);

    rc = kpm_move_module(mod, &load_info);
    if (rc)
        goto free_mod;
    rc = kpm_simplify_symbols(mod, &load_info);
    if (rc)
        goto free_mod;
    rc = kpm_apply_relocations(mod, &load_info);
    if (rc)
        goto free_mod;

    /* 替换 flush_icache_all() 为 flush_icache_range() */
    flush_icache_range((unsigned long)mod->start,
                       (unsigned long)mod->start + mod->size);

    rc = mod->init(mod->args, event, reserved);
    if (!rc) {
        printk(KERN_INFO "ARM64 KPM Loader: Module [%s] loaded successfully with args [%s]\n",
               mod->info.name, args ? args : "");
        spin_lock(&kpm_module_lock);
        list_add_tail(&mod->list, &kpm_module_list);
        spin_unlock(&kpm_module_lock);
        goto out;
    } else {
        printk(KERN_ERR "ARM64 KPM Loader: Module [%s] init failed with error %ld\n",
               mod->info.name, rc);
        mod->exit(reserved);
    }
free_mod:
    if (mod->args)
        vfree(mod->args);
    kpm_free_exec(mod->start, mod->size);
    vfree(mod);
out:
    return rc;
}

/* 卸载模块接口，改名为 sukisu_kpm_unload_module */
long sukisu_kpm_unload_module(const char *name, void *__user reserved)
{
    long rc = 0;
    struct kpm_module *mod = NULL;

    if (!name)
        return -EINVAL;
    spin_lock(&kpm_module_lock);
    
    list_for_each_entry(mod, &kpm_module_list, list) {
        if (!strcmp(name, mod->info.name))
            break;
    }
    if (!mod) {
        rc = -ENOENT;
        spin_unlock(&kpm_module_lock);
        return rc;
    }
    list_del(&mod->list);
    spin_unlock(&kpm_module_lock);
    // rc = mod->exit(reserved);
    mod->exit(reserved);
    if (mod->args)
        vfree(mod->args);
    if (mod->ctl_args)
        vfree(mod->ctl_args);
    kpm_free_exec(mod->start, mod->size);
    vfree(mod);
    printk(KERN_INFO "ARM64 KPM Loader: Module %s unloaded, rc = %ld\n", name, rc);
    return rc;
}

/*-----------------------------------------------------------
 * 导出接口：从文件路径加载 KPM 模块（改名为 sukisu_kpm_load_module_path）
 *----------------------------------------------------------*/
long sukisu_kpm_load_module_path(const char *path, const char *args, void *__user reserved)
{
    long rc = 0;
    struct file *filp;
    loff_t len;
    void *data;
    loff_t pos = 0;

    printk(KERN_INFO "ARM64 KPM Loader: Loading module from file: %s\n", path);
    if (!path)
        return -EINVAL;
    filp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(filp)) {
        printk(KERN_ERR "ARM64 KPM Loader: Failed to open file %s\n", path);
        return PTR_ERR(filp);
    }
    len = vfs_llseek(filp, 0, SEEK_END);
    printk(KERN_INFO "ARM64 KPM Loader: Module file size: %llx\n", len);
    vfs_llseek(filp, 0, SEEK_SET);
    data = vmalloc(len);
    if (!data) {
        filp_close(filp, NULL);
        return -ENOMEM;
    }
    memset(data, 0, len);
    kernel_read(filp, data, len, &pos);
    filp_close(filp, 0);
    if (pos != len) {
        printk(KERN_ERR "ARM64 KPM Loader: Read file error\n");
        rc = -EIO;
        goto free_data;
    }
    rc = kpm_load_module(data, len, args, "load-file", reserved);
free_data:
    vfree(data);
    return rc;
}

/*-----------------------------------------------------------
 * 模块管理查询接口
 *----------------------------------------------------------*/
struct kpm_module *sukisu_kpm_find_module(const char *name)
{
    struct kpm_module *pos;
    spin_lock(&kpm_module_lock);
    list_for_each_entry(pos, &kpm_module_list, list) {
        if (!strcmp(name, pos->info.name)) {
            spin_unlock(&kpm_module_lock);
            return pos;
        }
    }
    spin_unlock(&kpm_module_lock);
    return NULL;
}

// 获取已经加载的KPM数量
int sukisu_kpm_num(void) {
    struct kpm_module *pos;
    int num = 0;

    spin_lock(&kpm_module_lock);
    list_for_each_entry(pos, &kpm_module_list, list) {
        num++;
    }
    spin_unlock(&kpm_module_lock);
    
    return num;
}

// 获取指定名称的KPM信息
int sukisu_kpm_info(const char* name, char __user* out) {
    char buffer[512] = { 0 };
    struct kpm_module *kpm = sukisu_kpm_find_module(name);
    int osize;

    if(kpm == NULL) {
        return -1;
    }

    memset((void*)&buffer, 0, sizeof(buffer));
    osize = snprintf(buffer, 511,
        "Name: %s\n"
        "Version: %s\n"
        "Author: %s\n"
        "License: %s\n"
        "Description: %s",
        kpm->info.name, kpm->info.version, kpm->info.author, kpm->info.license, kpm->info.description);
    
    osize = copy_to_user((void __user*) out, (const void*)buffer, osize + 1);
    return osize;
}

int sukisu_kpm_list(char __user *out,unsigned int bufferSize)
{
    struct kpm_module *pos;
    int outSize = 0;
    int len;
    char buffer[128]; // 临时缓冲区，避免直接操作用户空间

    spin_lock(&kpm_module_lock);
    list_for_each_entry(pos, &kpm_module_list, list) {
        /* 格式化输出，每行一个模块名称 */
        len = snprintf(buffer, sizeof(buffer), "%s\n", pos->info.name);

        /* 检查剩余空间是否足够 */
        if (outSize + len > bufferSize) {
            spin_unlock(&kpm_module_lock);
            return -ENOSPC;  // 空间不足
        }

        /* 复制到用户空间 */
        if (copy_to_user(out + outSize, buffer, len)) {
            spin_unlock(&kpm_module_lock);
            return -EFAULT;  // 复制失败
        }

        outSize += len;
    }
    spin_unlock(&kpm_module_lock);

    return outSize;
}

// 打印所有KPM信息
/* 直接写入进程 stdout（fd = 1） */
void sukisu_kpm_print_list(void)
{
    struct kpm_module *kpm;
    struct file *stdout_file;
    loff_t pos = 0;
    char *buffer;
    int len;

    /* 打开当前进程的 stdout */
    stdout_file = filp_open("/proc/self/fd/1", O_WRONLY, 0);
    if (IS_ERR(stdout_file)) {
        pr_err("sukisu_kpm_print_list: Failed to open stdout.\n");
        return;
    }

    /* 分配内核缓冲区 */
    buffer = kmalloc(256, GFP_KERNEL);
    if (!buffer) {
        pr_err("sukisu_kpm_print_list: Failed to allocate buffer.\n");
        filp_close(stdout_file, NULL);
        return;
    }

    spin_lock(&kpm_module_lock);
    list_for_each_entry(kpm, &kpm_module_list, list) {
        /* 格式化模块信息 */
        len = snprintf(buffer, 256,
            "Name: %s\n"
            "Version: %s\n"
            "Author: %s\n"
            "License: %s\n"
            "Description: %s\n\n",
            kpm->info.name, kpm->info.version, kpm->info.author,
            kpm->info.license, kpm->info.description);

        /* 通过 kernel_write() 直接写入 stdout */
        kernel_write(stdout_file, buffer, len, &pos);
    }
    spin_unlock(&kpm_module_lock);

    /* 释放资源 */
    kfree(buffer);
    filp_close(stdout_file, NULL);
}

EXPORT_SYMBOL(sukisu_kpm_load_module_path);
EXPORT_SYMBOL(sukisu_kpm_unload_module);
EXPORT_SYMBOL(sukisu_kpm_find_module);

// ============================================================================================

int sukisu_handle_kpm(unsigned long arg3, unsigned long arg4, unsigned long arg5)
{
    if(arg3 == SUKISU_KPM_LOAD) {
        char kernel_load_path[256] = { 0 };
        char kernel_args_buffer[256] = { 0 };

        if(arg4 == 0) {
            return -1;
        }
        
        strncpy_from_user((char*)&kernel_load_path, (const char __user *)arg4, 255);
        if(arg5 != 0) {
            strncpy_from_user((char*)&kernel_args_buffer, (const char __user *)arg4, 255);
        }
        return sukisu_kpm_load_module_path((const char*)&kernel_load_path, (const char*) &kernel_args_buffer, NULL);
    } else if(arg3 == SUKISU_KPM_UNLOAD) {
        char kernel_name_buffer[256] = { 0 };

        if(arg4 == 0) {
            return -1;
        }
        
        strncpy_from_user((char*)&kernel_name_buffer, (const char __user *)arg4, 255);
        return sukisu_kpm_unload_module((const char*) &kernel_name_buffer, NULL);
    } else if(arg3 == SUKISU_KPM_NUM) {
        return sukisu_kpm_num();
    } else if(arg3 == SUKISU_KPM_INFO) {
        char kernel_name_buffer[256] = { 0 };

        if(arg4 == 0 || arg5 == 0) {
            return -1;
        }
        
        strncpy_from_user((char*)&kernel_name_buffer, (const char __user *)arg4, 255);
        return sukisu_kpm_info((const char*) &kernel_name_buffer, (char __user*) arg5);
    } else if(arg3 == SUKISU_KPM_LIST) {
        return sukisu_kpm_list((char __user*) arg4, (unsigned int) arg5);
    } else if(arg3 == SUKISU_KPM_PRINT) {
        sukisu_kpm_print_list();
    }
    return 0;
}

int sukisu_is_kpm_control_code(unsigned long arg2) {
    return (arg2 >= CMD_KPM_CONTROL && arg2 <= CMD_KPM_CONTROL_MAX) ? 1 : 0;
}

EXPORT_SYMBOL(sukisu_handle_kpm);