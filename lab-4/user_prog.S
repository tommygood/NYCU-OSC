/*
 * user_prog.S - Simple user-mode program for lab-4.
 *
 * Compiled as a flat binary, loaded into memory and executed in U-mode.
 * Uses ecall to request services from the kernel:
 *   a7 = 1: putchar(a0)
 *   a7 = 0: exit
 */
.global _start
_start:
    /* Print "Hello from U-mode!\n" */
    li a7, 1
    li a0, 'H'
    ecall
    li a0, 'e'
    ecall
    li a0, 'l'
    ecall
    li a0, 'l'
    ecall
    li a0, 'o'
    ecall
    li a0, ' '
    ecall
    li a0, 'f'
    ecall
    li a0, 'r'
    ecall
    li a0, 'o'
    ecall
    li a0, 'm'
    ecall
    li a0, ' '
    ecall
    li a0, 'U'
    ecall
    li a0, '-'
    ecall
    li a0, 'm'
    ecall
    li a0, 'o'
    ecall
    li a0, 'd'
    ecall
    li a0, 'e'
    ecall
    li a0, '!'
    ecall
    li a0, '\n'
    ecall

    /* Exit */
    li a7, 0
    ecall

    /* Should not reach here */
1:  j 1b
