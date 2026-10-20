.section .text
.global _start
_start:
    mov $39, %eax
    syscall
    test %rax, %rax
    jle failed

    xor %edi, %edi
    mov $12, %eax
    syscall
    test %rax, %rax
    jle failed
    mov %rax, %r13
    lea 4096(%r13), %rdi
    mov $12, %eax
    syscall
    cmp %r13, %rax
    jbe failed

    xor %edi, %edi
    mov $4096, %esi
    mov $3, %edx
    mov $0x22, %r10d
    mov $-1, %r8
    xor %r9d, %r9d
    mov $9, %eax
    syscall
    test %rax, %rax
    js failed
    mov %rax, %r12
    mov %r12, %rdi
    mov $4096, %esi
    xor %edx, %edx
    mov $10, %eax
    syscall
    test %rax, %rax
    jne failed
    mov %r12, %rdi
    mov $4096, %esi
    mov $3, %edx
    mov $10, %eax
    syscall
    test %rax, %rax
    jne failed
    movb $0x5A, (%r12)
    mov %r12, %rdi
    mov $4096, %esi
    mov $11, %eax
    syscall
    test %rax, %rax
    jne failed

    lea uname_buf(%rip), %rdi
    mov $63, %eax
    syscall
    test %rax, %rax
    jne failed
    cmpb $'L', uname_buf(%rip)
    jne failed

    mov $1, %edi
    lea clock_buf(%rip), %rsi
    mov $228, %eax
    syscall
    test %rax, %rax
    jne failed

    lea tls_data(%rip), %rsi
    mov $0x1002, %edi
    mov $158, %eax
    syscall
    test %rax, %rax
    jne failed
    mov $0x1003, %edi
    lea tls_result(%rip), %rsi
    mov $158, %eax
    syscall
    test %rax, %rax
    jne failed
    mov tls_data(%rip), %r12
    cmp tls_result(%rip), %r12
    jne failed

    lea random_buf(%rip), %rdi
    mov $16, %esi
    xor %edx, %edx
    mov $318, %eax
    syscall
    cmp $16, %rax
    jne failed

    movdqu test_vec(%rip), %xmm0
    lea sleep_req(%rip), %rdi
    xor %esi, %esi
    mov $35, %eax
    syscall
    test %rax, %rax
    jne failed
    pcmpeqb test_vec(%rip), %xmm0
    pmovmskb %xmm0, %eax
    cmp $0xFFFF, %eax
    jne failed

    mov $1, %edi
    lea success_msg(%rip), %rsi
    mov $success_len, %edx
    mov $1, %eax
    syscall
    xor %edi, %edi
    mov $231, %eax
    syscall

failed:
    mov $1, %edi
    lea fail_msg(%rip), %rsi
    mov $fail_len, %edx
    mov $1, %eax
    syscall
    mov $99, %edi
    mov $231, %eax
    syscall

.section .rodata
success_msg:
    .ascii "LINUX_ABI_SMOKE_OK\n"
.set success_len, . - success_msg
fail_msg:
    .ascii "LINUX_ABI_SMOKE_FAIL\n"
.set fail_len, . - fail_msg
sleep_req:
    .quad 0, 10000000
test_vec:
    .quad 0x0123456789ABCDEF, 0xFEDCBA9876543210

.section .bss
.align 16
uname_buf:
    .skip 390
clock_buf:
    .skip 16
tls_data:
    .quad 0x12345678
tls_result:
    .quad 0
random_buf:
    .skip 16
