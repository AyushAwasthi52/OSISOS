# Makefile for OSISOS Kernel

# We use the standard GNU toolchain, but force 32-bit (i686) targets.
CC = gcc
AS = gcc

# Compiler flags for a freestanding kernel
# -m32: Compile for 32-bit architecture
# -ffreestanding: Assumes no standard library exists (no malloc, printf, etc.)
# -O2: Optimization level
# -Wall -Wextra: Enable warnings
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-pie -mno-sse -mno-sse2 -mno-mmx -mno-80387

# Linker flags
# -nostdlib: Do not link the standard C library
# -T linker.ld: Use our custom linker script
LDFLAGS = -m32 -T linker.ld -nostdlib -lgcc -no-pie

# Files to build
OBJS = boot.o gdt_flush.o gdt.o kernel.o

all: osisos.bin

# Assemble boot.S
boot.o: boot.S
	$(AS) -m32 -c boot.S -o boot.o

# Assemble gdt_flush.S
gdt_flush.o: gdt_flush.S
	$(AS) -m32 -c gdt_flush.S -o gdt_flush.o

# Compile gdt.c
gdt.o: gdt.c
	$(CC) $(CFLAGS) -c gdt.c -o gdt.o

# Compile kernel.c
kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

# Link into the final executable
osisos.bin: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o osisos.bin

# Target to build a bootable ISO using GRUB
# This requires 'grub-mkrescue' and 'xorriso' to be installed on the host.
iso: osisos.bin
	mkdir -p isodir/boot/grub
	cp osisos.bin isodir/boot/osisos.bin
	echo 'menuentry "OSISOS" {' > isodir/boot/grub/grub.cfg
	echo '	multiboot /boot/osisos.bin' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o osisos.iso isodir

clean:
	rm -f $(OBJS) osisos.bin osisos.iso
	rm -rf isodir
