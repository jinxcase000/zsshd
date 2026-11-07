/*
 * System calls that report failure, for the PS5 build.
 *
 * The payload SDK's libc.a (syscalls.o) defines 233 system calls as raw
 * stubs, `mov $n,%rax; mov %rcx,%r10; syscall; ret`. None checks the carry
 * flag the kernel sets on failure (the FreeBSD amd64 convention), so a failed
 * call returns the positive errno in %rax and leaves errno alone: a caller
 * testing `< 0` reads the failure as success. Once syscalls.o is linked its
 * definitions also win over libkernel's exports of the same names.
 *
 * The Makefile links with --wrap=<name> for each stub below (RAW_WRAPPED),
 * so a reference to one resolves to __wrap_<name> here, and traces every
 * other stub name (ps5_raw_syscalls.txt): a reference to one fails the build
 * instead of being called. Each wrapper makes the same call as the stub and,
 * on carry, stores %rax in *__error() and returns -1.
 *
 * Built for the PS5 by the Makefile. test/host/raw_syscall.sh also builds it
 * on an x86_64 macOS or FreeBSD host (same carry-flag convention; macOS
 * numbers carry the 0x2000000 BSD class) to check the error path.
 */
#include <sys/syscall.h>

#if defined(__APPLE__)
#define SYM(n) _##n
#define NR(n) (0x2000000 + (n))
#define FUNC(n) .globl SYM(n); .p2align 4; SYM(n):
#define END(n)
#else
#define SYM(n) n
#define NR(n) (n)
#define FUNC(n) .globl n; .type n, @function; .p2align 4; n:
#define END(n) .size n, . - n
#endif

/* A system call with its number fixed, arguments as the C ABI passes them. */
#define RAW(name)                                   \
    FUNC(__wrap_##name)                             \
    movq $NR(SYS_##name), %rax; movq %rcx, %r10;    \
    syscall; jc zsshd_raw_fail; ret;                \
    END(__wrap_##name)

/* syscall(number, ...) and __syscall(number, ...), shuffled as the stubs do. */
#if defined(__APPLE__)
#define CLASS orq $0x2000000, %rax
#else
#define CLASS
#endif
#define INDIRECT(name)                                              \
    FUNC(__wrap_##name)                                             \
    movq %rdi, %rax; CLASS; movq %rsi, %rdi; movq %rdx, %rsi;       \
    movq %rcx, %rdx; movq %r8, %r10; movq %r9, %r8;                 \
    movq 8(%rsp), %r9; syscall; jc zsshd_raw_fail; ret;             \
    END(__wrap_##name)

    .text

/* %rax holds the errno. The stack is as on entry to the wrapper (8 mod 16),
 * so the push aligns it for the call. */
    .p2align 4
zsshd_raw_fail:
    pushq %rax
    call *SYM(__error)@GOTPCREL(%rip)
    popq %rcx
    movl %ecx, (%rax)
    movq $-1, %rax
    ret

/* The stubs the build calls; keep in step with the Makefile's RAW_WRAPPED. */
RAW(link)
RAW(symlink)
RAW(readlink)
RAW(chown)
RAW(fchown)
RAW(getpgrp)
#if defined(SYS_utimensat)
RAW(utimensat)
#endif
RAW(getfsstat)
RAW(statfs)
INDIRECT(syscall)
INDIRECT(__syscall)

#if !defined(__APPLE__)
    .section .note.GNU-stack, "", @progbits
#endif
