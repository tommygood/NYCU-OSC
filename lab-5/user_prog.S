/*
 * user_prog.S - Simple user-mode test program for lab-5.
 *
 * Lab-5 syscall convention:
 *   a7 = 0: getpid() -> a0
 *   a7 = 2: uart_write(buf, count)
 *   a7 = 6: exit(status)
 */
.global _start
_start:
    /* Print "Hello from U-mode!\n" via uart_write (syscall 2) */
    1: auipc a0, %pcrel_hi(msg)
    addi  a0, a0, %pcrel_lo(1b)
    li   a1, 19
    li   a7, 2
    ecall

    /* Get PID via getpid (syscall 0) */
    li   a7, 0
    ecall
    /* a0 = pid, convert to ASCII digit */
    addi a0, a0, '0'
    1: auipc t0, %pcrel_hi(pidbuf)
    addi  t0, t0, %pcrel_lo(1b)
    sb   a0, 0(t0)

    /* Print "PID: X\n" */
    1: auipc a0, %pcrel_hi(pidmsg)
    addi  a0, a0, %pcrel_lo(1b)
    li   a1, 7
    li   a7, 2
    ecall

    /* Exit (syscall 6) */
    li   a0, 0
    li   a7, 6
    ecall

    /* Should not reach here */
1:  j 1b

.section .data
msg:
    .ascii "Hello from U-mode!\n"
pidmsg:
    .ascii "PID: "
pidbuf:
    .ascii "?\n"
