RISCV_GNU ?= riscv64-unknown-elf
CC        = $(RISCV_GNU)-gcc
LD        = $(RISCV_GNU)-ld
OBJCOPY   = $(RISCV_GNU)-objcopy
CFLAGS    = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
QEMU      ?= qemu-system-riscv64

SUPPORT_SRCS = uart.c dtb.c string.c sbi.c mm.c plic.c timer.c trap.c
SUPPORT_OBJS = uart.o dtb.o string.o sbi.o mm.o plic.o timer.o trap.o

# ── User program (flat binary, placed in rootfs) ──────────────────────────
user_prog:
	$(CC) $(CFLAGS) -c user_prog.S -o user_prog.o
	$(LD) -T user_prog.ld -o user_prog.elf user_prog.o
	$(OBJCOPY) -O binary user_prog.elf rootfs/user_prog.bin

# ── Kernel ─────────────────────────────────────────────────────────────────
kernel-qemu: clean user_prog initramfs
	$(CC) $(CFLAGS) -DQEMU -c kernel_start.S trapasm.S $(SUPPORT_SRCS) main.c
	$(LD) -T linker_qemu.ld -o kernel.elf \
	    kernel_start.o trapasm.o main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary kernel.elf kernel.bin

kernel-board: clean user_prog initramfs
	$(CC) $(CFLAGS) -c kernel_start.S trapasm.S $(SUPPORT_SRCS) main.c
	$(LD) -T linker.ld -o kernel.elf \
	    kernel_start.o trapasm.o main.o $(SUPPORT_OBJS)
	$(OBJCOPY) -O binary kernel.elf kernel.bin

# ── QEMU run targets ───────────────────────────────────────────────────────
run-pty: kernel-qemu
	$(QEMU) -M virt -m 4G -kernel kernel.bin \
		-initrd initramfs.cpio -serial pty -display none -no-reboot

run: kernel-qemu
	$(QEMU) -M virt -m 4G -kernel kernel.bin \
		-initrd initramfs.cpio -nographic -no-reboot

# ── OrangePi RV2 FIT image ────────────────────────────────────────────────
fit-kernel: kernel-board
	mkimage -f kernel.its kernel.fit

# ── initramfs ─────────────────────────────────────────────────────────────
initramfs:
	cd rootfs && find . | cpio -o -H newc > ../initramfs.cpio

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f *.bin *.elf *.fit *.o rootfs/user_prog.bin

.PHONY: kernel-qemu kernel-board run-pty run fit-kernel initramfs clean user_prog
