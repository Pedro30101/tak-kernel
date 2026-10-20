.section .text
.code64

.global isr0
isr0:
    pushq $0
    pushq $0
    jmp isr_common

.global isr1
isr1:
    pushq $0
    pushq $1
    jmp isr_common

.global isr2
isr2:
    pushq $0
    pushq $2
    jmp isr_common

.global isr3
isr3:
    pushq $0
    pushq $3
    jmp isr_common

.global isr4
isr4:
    pushq $0
    pushq $4
    jmp isr_common

.global isr5
isr5:
    pushq $0
    pushq $5
    jmp isr_common

.global isr6
isr6:
    pushq $0
    pushq $6
    jmp isr_common

.global isr7
isr7:
    pushq $0
    pushq $7
    jmp isr_common

.global isr8
isr8:
    pushq $8
    jmp isr_common

.global isr9
isr9:
    pushq $0
    pushq $9
    jmp isr_common

.global isr10
isr10:
    pushq $10
    jmp isr_common

.global isr11
isr11:
    pushq $11
    jmp isr_common

.global isr12
isr12:
    pushq $12
    jmp isr_common

.global isr13
isr13:
    pushq $13
    jmp isr_common

.global isr14
isr14:
    pushq $14
    jmp isr_common

.global isr15
isr15:
    pushq $0
    pushq $15
    jmp isr_common

.global isr16
isr16:
    pushq $0
    pushq $16
    jmp isr_common

.global isr17
isr17:
    pushq $17
    jmp isr_common

.global isr18
isr18:
    pushq $0
    pushq $18
    jmp isr_common

.global isr19
isr19:
    pushq $0
    pushq $19
    jmp isr_common

.global isr20
isr20:
    pushq $0
    pushq $20
    jmp isr_common

.global isr21
isr21:
    pushq $21
    jmp isr_common

.global isr22
isr22:
    pushq $0
    pushq $22
    jmp isr_common

.global isr23
isr23:
    pushq $0
    pushq $23
    jmp isr_common

.global isr24
isr24:
    pushq $0
    pushq $24
    jmp isr_common

.global isr25
isr25:
    pushq $0
    pushq $25
    jmp isr_common

.global isr26
isr26:
    pushq $0
    pushq $26
    jmp isr_common

.global isr27
isr27:
    pushq $0
    pushq $27
    jmp isr_common

.global isr28
isr28:
    pushq $0
    pushq $28
    jmp isr_common

.global isr29
isr29:
    pushq $29
    jmp isr_common

.global isr30
isr30:
    pushq $30
    jmp isr_common

.global isr31
isr31:
    pushq $0
    pushq $31
    jmp isr_common

.global isr32
isr32:
    pushq $0
    pushq $32
    jmp isr_common

.global isr33
isr33:
    pushq $0
    pushq $33
    jmp isr_common

.global isr34
isr34:
    pushq $0
    pushq $34
    jmp isr_common

.global isr35
isr35:
    pushq $0
    pushq $35
    jmp isr_common

.global isr36
isr36:
    pushq $0
    pushq $36
    jmp isr_common

.global isr37
isr37:
    pushq $0
    pushq $37
    jmp isr_common

.global isr38
isr38:
    pushq $0
    pushq $38
    jmp isr_common

.global isr39
isr39:
    pushq $0
    pushq $39
    jmp isr_common

.global isr40
isr40:
    pushq $0
    pushq $40
    jmp isr_common

.global isr41
isr41:
    pushq $0
    pushq $41
    jmp isr_common

.global isr42
isr42:
    pushq $0
    pushq $42
    jmp isr_common

.global isr43
isr43:
    pushq $0
    pushq $43
    jmp isr_common

.global isr44
isr44:
    pushq $0
    pushq $44
    jmp isr_common

.global isr45
isr45:
    pushq $0
    pushq $45
    jmp isr_common

.global isr46
isr46:
    pushq $0
    pushq $46
    jmp isr_common

.global isr47
isr47:
    pushq $0
    pushq $47
    jmp isr_common

.global isr128
isr128:
    pushq $0
    pushq $128
    jmp isr_common

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    cld
    call isr_handler
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

.global switch_ctx
switch_ctx:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.global task_tramp
task_tramp:
    sti
    movq %r13, %rdi
    call *%r12
    call task_done
1:  hlt
    jmp 1b

.global enter_user
enter_user:
    pushq $0x1B
    pushq %rsi
    pushq $0x202
    pushq $0x23
    pushq %rdi
    movq %rdx, %rdi
    iretq

.global linux_syscall_entry
linux_syscall_entry:
    movq %rsp, syscall_saved_user_rsp(%rip)
    movq syscall_kernel_rsp(%rip), %rsp
    pushq $0x1B
    pushq syscall_saved_user_rsp(%rip)
    pushq %r11
    pushq $0x23
    pushq %rcx
    pushq $0
    pushq $0x100
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    cld
    call linux_sys_call
    movq %rax, 112(%rsp)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    popq %rcx
    addq $8, %rsp
    popq %r11
    popq %rsp
    sysretq

.section .bss
.align 8
.global syscall_saved_user_rsp
syscall_saved_user_rsp:
    .quad 0

.section .rodata
.align 8
.global isr_table
isr_table:
    .quad isr0
    .quad isr1
    .quad isr2
    .quad isr3
    .quad isr4
    .quad isr5
    .quad isr6
    .quad isr7
    .quad isr8
    .quad isr9
    .quad isr10
    .quad isr11
    .quad isr12
    .quad isr13
    .quad isr14
    .quad isr15
    .quad isr16
    .quad isr17
    .quad isr18
    .quad isr19
    .quad isr20
    .quad isr21
    .quad isr22
    .quad isr23
    .quad isr24
    .quad isr25
    .quad isr26
    .quad isr27
    .quad isr28
    .quad isr29
    .quad isr30
    .quad isr31
    .quad isr32
    .quad isr33
    .quad isr34
    .quad isr35
    .quad isr36
    .quad isr37
    .quad isr38
    .quad isr39
    .quad isr40
    .quad isr41
    .quad isr42
    .quad isr43
    .quad isr44
    .quad isr45
    .quad isr46
    .quad isr47
    .quad isr128
.global isr_table_n
isr_table_n: .quad 49
