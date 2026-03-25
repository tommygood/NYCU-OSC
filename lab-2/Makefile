RISCV_GNU ?= riscv64-unknown-elf
CC        = $(RISCV_GNU)-gcc
LD        = $(RISCV_GNU)-ld
OBJCOPY   = $(RISCV_GNU)-objcopy
CFLAGS    = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
QEMU      ?= qemu-system-riscv64
TARGET    = kernel

# ── OrangePi RV2 (bare-metal, 0x00200000) ────────────────────────────────
build: clean initramfs
	$(CC) $(CFLAGS) -c *.S *.c
	$(LD) -T linker.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

fit: build
	mkimage -f kernel.its $(TARGET).fit

# ── QEMU virt (0x80200000) ────────────────────────────────────────────────
run: clean initramfs
	$(CC) $(CFLAGS) -DQEMU -c *.S *.c
	$(LD) -T linker_qemu.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	$(QEMU) -M virt -m 4G -kernel $(TARGET).bin \
		-initrd initramfs.cpio -nographic -no-reboot

# ── QEMU with pseudo-TTY (for bootloader testing) ─────────────────────────
# QEMU prints the PTY path, e.g.: char device redirected to /dev/pts/5
# Then: picocom /dev/pts/5   and   python3 send_kernel.py /dev/pts/5 new.bin
run-pty: clean initramfs
	$(CC) $(CFLAGS) -DQEMU -c *.S *.c
	$(LD) -T linker_qemu.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	$(QEMU) -M virt -m 4G -kernel $(TARGET).bin \
		-initrd initramfs.cpio -serial pty -display none -no-reboot

# ── QEMU build only (no run) ──────────────────────────────────────────────
build-qemu: clean initramfs
	$(CC) $(CFLAGS) -DQEMU -c *.S *.c
	$(LD) -T linker_qemu.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

# ── initramfs ─────────────────────────────────────────────────────────────
initramfs:
	cd rootfs && find . | cpio -o -H newc > ../initramfs.cpio

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET).bin $(TARGET).elf $(TARGET).fit *.o
