RISCV_GNU ?= riscv64-unknown-elf
CC        = $(RISCV_GNU)-gcc
LD        = $(RISCV_GNU)-ld
OBJCOPY   = $(RISCV_GNU)-objcopy
CFLAGS    = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
QEMU      ?= qemu-system-riscv64

SUPPORT_SRCS = uart.c dtb.c string.c sbi.c
SUPPORT_OBJS = uart.o dtb.o string.o sbi.o

# ── Dedicated UART bootloader ──────────────────────────────────────────────
# Linked at 0x80200000 / 0x00200000, self-relocates to RELOC_ADDR at runtime.
# Waits for a kernel over UART (BOOT protocol), loads it to LOAD_ADDR, jumps.
#
# Workflow (QEMU):
#   make run-pty              → start QEMU, note PTY (e.g. /dev/pts/5)
#   picocom /dev/pts/5        → connect terminal
#   make kernel-qemu          → build kernel.bin
#   python3 send_kernel.py /dev/pts/5 kernel.bin  → load and boot kernel

bootloader-qemu: clean initramfs
	$(CC) $(CFLAGS) -DQEMU -c start.S $(SUPPORT_SRCS) bootloader_main.c
	$(LD) -T linker_qemu.ld -o bootloader.elf \
	    start.o bootloader_main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary bootloader.elf bootloader.bin

bootloader-board: clean initramfs
	$(CC) $(CFLAGS) -c start.S $(SUPPORT_SRCS) bootloader_main.c
	$(LD) -T linker.ld -o bootloader.elf \
	    start.o bootloader_main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary bootloader.elf bootloader.bin

# ── Kernel (no self-relocation, stays at LOAD_ADDR) ───────────────────────
# Load this with the UART bootloader via send_kernel.py.
# Has a "bootloader" command to jump back to the resident bootloader.

kernel-qemu: clean initramfs
	$(CC) $(CFLAGS) -DQEMU -c kernel_start.S $(SUPPORT_SRCS) main.c
	$(LD) -T linker_qemu.ld -o kernel.elf \
	    kernel_start.o main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary kernel.elf kernel.bin

kernel-board: clean initramfs
	$(CC) $(CFLAGS) -c kernel_start.S $(SUPPORT_SRCS) main.c
	$(LD) -T linker.ld -o kernel.elf \
	    kernel_start.o main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary kernel.elf kernel.bin

# ── QEMU run targets ───────────────────────────────────────────────────────
# Run bootloader with PTY (detach with Ctrl-A D, then send kernel)
run-pty: bootloader-qemu
	$(QEMU) -M virt -m 4G -kernel bootloader.bin \
		-initrd initramfs.cpio -serial pty -display none -no-reboot

# Run bootloader interactively (useful for quick testing without send_kernel)
run: bootloader-qemu
	$(QEMU) -M virt -m 4G -kernel bootloader.bin \
		-initrd initramfs.cpio -nographic -no-reboot

# ── OrangePi RV2 FIT image ────────────────────────────────────────────────
# U-Boot on OrangePi RV2 loads the file named "kernel.fit" from the SD card.
# Both fit-bootloader and fit-kernel produce kernel.fit so the deploy command
# is always:  sudo cp kernel.fit /mnt/kernel.fit
fit-bootloader: bootloader-board
	mkimage -f bootloader.its kernel.fit

fit-kernel: kernel-board
	mkimage -f kernel.its kernel.fit

# ── initramfs ─────────────────────────────────────────────────────────────
initramfs:
	cd rootfs && find . | cpio -o -H newc > ../initramfs.cpio

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f *.bin *.elf *.fit *.o

.PHONY: bootloader-qemu bootloader-board kernel-qemu kernel-board \
        run-pty run fit-bootloader fit-kernel initramfs clean
