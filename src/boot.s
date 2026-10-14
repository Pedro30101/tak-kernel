.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH, 0
.set MB2_HEADER_LEN, mb2_header_end - mb2_header
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

.section .multiboot, "a"
.align 8
mb2_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_HEADER_LEN
    .long MB2_CHECKSUM
    .short 6
    .short 0
    .long 8
    .align 8
    .short 5
    .short 0
    .long 20
    .long 1920
    .long 1080
    .long 32
    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .bss, "aw", @nobits
.align 16
.global stack_bottom
stack_bottom:
    .skip 65536
.global stack_top
stack_top:
boot_magic:
    .skip 4
boot_info:
    .skip 4

.align 4096
.global pml4
pml4:
    .skip 4096
.global pdpt
pdpt:
    .skip 4096
.global page_directory
page_directory:
    .skip 16384

.section .rodata
.align 8
gdt64:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
gdt64_end:
gdt64_pointer:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .text, "ax"
.code32
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp
    mov %eax, boot_magic
    mov %ebx, boot_info

    mov $pml4, %eax
    mov $pdpt, %edx
    or $0x3, %edx
    mov %edx, (%eax)
    mov $pdpt, %edi
    mov $page_directory, %edx
    or $0x3, %edx
    mov %edx, (%edi)
    mov $page_directory, %edx
    add $4096, %edx
    or $0x3, %edx
    mov %edx, 8(%edi)
    mov $page_directory, %edx
    add $8192, %edx
    or $0x3, %edx
    mov %edx, 16(%edi)
    mov $page_directory, %edx
    add $12288, %edx
    or $0x3, %edx
    mov %edx, 24(%edi)

    mov $page_directory, %edi
    xor %ecx, %ecx
1:
    mov %ecx, %eax
    shl $21, %eax
    or $0x83, %eax
    mov %eax, (%edi, %ecx, 8)
    movl $0, 4(%edi, %ecx, 8)
    inc %ecx
    cmp $2048, %ecx
    jne 1b

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx
    rdmsr
    or $0x900, %eax
    wrmsr

    lgdt gdt64_pointer
    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %ax, %ax
    mov %ax, %fs
    mov %ax, %gs
    mov $stack_top, %rsp
    and $-16, %rsp
    mov boot_info(%rip), %esi
    mov boot_magic(%rip), %edi
    call kmain
.hang:
    hlt
    jmp .hang
.size _start, . - _start
