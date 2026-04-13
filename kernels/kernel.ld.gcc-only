/**
 * kernel.ld
 * 
 * Linker script for minimal IA-64 kernel
 * 
 * Memory Layout:
 * - Kernel code starts at 0xE000000000100000 (Region 7, 1MB physical)
 * - Stack at 0xE000000000200000 (2MB physical)
 * - Backing store at 0xE000000000180000 (1.5MB physical)
 */

OUTPUT_FORMAT("elf64-ia64-little")
OUTPUT_ARCH(ia64)
ENTRY(_start)

/* IA-64 kernel region 7 base + 1MB physical offset */
KERNEL_BASE = 0xE000000000100000;

SECTIONS
{
    /* Start at kernel base address */
    . = KERNEL_BASE;
    
    /* Ensure proper alignment for IA-64 bundles */
    . = ALIGN(16);
    
    /* Code section */
    .text : ALIGN(16)
    {
        *(.text.start)          /* Entry point first */
        *(.text)                /* Rest of code */
        *(.text.*)
        . = ALIGN(16);          /* Ensure bundle alignment */
    }
    
    /* Read-only data */
    .rodata : ALIGN(16)
    {
        *(.rodata)
        *(.rodata.*)
        *(.rodata.str1.1)
        *(.rodata.str1.8)
        . = ALIGN(8);
    }
    
    /* IA-64 unwind information */
    .IA_64.unwind_info : ALIGN(8)
    {
        *(.IA_64.unwind_info)
        *(.IA_64.unwind_info.*)
    }
    
    .IA_64.unwind : ALIGN(8)
    {
        *(.IA_64.unwind)
        *(.IA_64.unwind.*)
    }
    
    /* Initialized data */
    .data : ALIGN(16)
    {
        *(.data)
        *(.data.*)
        . = ALIGN(8);
    }
    
    /* Small data (for gp-relative addressing) */
    .sdata : ALIGN(8)
    {
        __gp = .;               /* Global pointer */
        *(.sdata)
        *(.sdata.*)
    }
    
    /* BSS (uninitialized data) */
    .bss : ALIGN(16)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    
    /* End of kernel */
    . = ALIGN(4096);            /* Page align */
    __kernel_end = .;
    
    /* Stack (grows downward) */
    . = 0xE000000000200000;
    .stack (NOLOAD) : ALIGN(16)
    {
        __stack_bottom = .;
        . += 0x4000;            /* 16KB kernel stack */
        __stack_top = .;
    }
    
    /* Backing store for RSE */
    . = 0xE000000000180000;
    .backing_store (NOLOAD) : ALIGN(16)
    {
        __backing_store_base = .;
        . += 0x4000;            /* 16KB backing store */
        __backing_store_top = .;
    }
    
    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}

/* Provide symbols for kernel information */
PROVIDE(__kernel_start = KERNEL_BASE);
PROVIDE(__kernel_size = __kernel_end - KERNEL_BASE);
PROVIDE(__kernel_entry = _start);
