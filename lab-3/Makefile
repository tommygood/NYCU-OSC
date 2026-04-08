RISCV_GNU ?= riscv64-unknown-elf
CC        = $(RISCV_GNU)-gcc
LD        = $(RISCV_GNU)-ld
OBJCOPY   = $(RISCV_GNU)-objcopy
CFLAGS    = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
QEMU      ?= qemu-system-riscv64

SUPPORT_SRCS = uart.c dtb.c string.c sbi.c mm.c
SUPPORT_OBJS = uart.o dtb.o string.o sbi.o mm.o

# ── Kernel ─────────────────────────────────────────────────────────────────
# Self-relocates to RELOC_ADDR at startup (start.S), freeing LOAD_ADDR so
# the built-in "bootloader" command can receive a new kernel there via UART.
#
# Workflow (QEMU):
#   make run-pty              → start QEMU, note PTY (e.g. /dev/pts/5)
#   picocom /dev/pts/5        → connect terminal, run "bootloader" command
#   python3 send_kernel.py /dev/pts/5 kernel.bin  → load and boot a kernel

kernel-qemu: clean initramfs
	$(CC) $(CFLAGS) -DQEMU -c start.S $(SUPPORT_SRCS) main.c
	$(LD) -T linker_qemu.ld -o kernel.elf \
	    start.o main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary kernel.elf kernel.bin

kernel-board: clean initramfs
	$(CC) $(CFLAGS) -c start.S $(SUPPORT_SRCS) main.c
	$(LD) -T linker.ld -o kernel.elf \
	    start.o main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary kernel.elf kernel.bin

# ── QEMU run targets ───────────────────────────────────────────────────────
# Run kernel with PTY (detach with Ctrl-A D, run "bootloader" to send kernel)
run-pty: kernel-qemu
	$(QEMU) -M virt -m 4G -kernel kernel.bin \
		-initrd initramfs.cpio -serial pty -display none -no-reboot

# Run kernel interactively
run: kernel-qemu
	$(QEMU) -M virt -m 4G -kernel kernel.bin \
		-initrd initramfs.cpio -nographic -no-reboot

# ── OrangePi RV2 FIT image ────────────────────────────────────────────────
# U-Boot on OrangePi RV2 loads the file named "kernel.fit" from the SD card.
# Deploy:  sudo cp kernel.fit /mnt/kernel.fit
fit-kernel: kernel-board
	mkimage -f kernel.its kernel.fit

# ── initramfs ─────────────────────────────────────────────────────────────
initramfs:
	cd rootfs && find . | cpio -o -H newc > ../initramfs.cpio

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f *.bin *.elf *.fit *.o

.PHONY: kernel-qemu kernel-board run-pty run fit-kernel initramfs clean
