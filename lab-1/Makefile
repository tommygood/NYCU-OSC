RISCV_GNU ?= riscv64-unknown-elf
CC       = $(RISCV_GNU)-gcc
LD       = $(RISCV_GNU)-ld
OBJCOPY  = $(RISCV_GNU)-objcopy
CFLAGS   = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
QEMU     ?= qemu-system-riscv64
TARGET   = kernel

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(LD) -T linker.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

run: clean
	$(CC) $(CFLAGS) -DQEMU -c *.S *.c
	$(LD) -T linker_qemu.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	$(QEMU) -M virt -m 4G -kernel $(TARGET).bin -nographic

fit: build
	mkimage -f kernel.its $(TARGET).fit

clean:
	rm -f $(TARGET).bin $(TARGET).elf $(TARGET).fit *.o
