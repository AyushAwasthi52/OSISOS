# Makefile for OSISOS Kernel

# We use the standard GNU toolchain, but force 32-bit (i686) targets.
CC = gcc
AS = gcc

# Compiler flags for a freestanding kernel
# -I flags tell the compiler where to look for header files (.h)
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-pie -mno-sse -mno-sse2 -mno-mmx -mno-80387 -fno-stack-protector \
         -Iinclude -Iarch/i386 -Idrivers -Imm -Icore -Ifs

# Linker flags
LDFLAGS = -m32 -T linker.ld -nostdlib -lgcc -no-pie

# Files to build (Now organized into subdirectories)
OBJS = arch/i386/boot.o \
       arch/i386/gdt_flush.o arch/i386/gdt.o \
       arch/i386/idt_flush.o arch/i386/idt.o \
       arch/i386/pic.o arch/i386/paging_flush.o \
       arch/i386/switch.o arch/i386/usermode.o \
       drivers/keyboard.o drivers/timer.o drivers/ide.o drivers/pci.o drivers/e1000.o \
       mm/pmm.o mm/paging.o mm/kheap.o fs/ofs.o \
       core/task.o core/kernel.o core/syscall.o

all: osisos.bin

# Convenience target to build the ISO
iso: osisos.iso

# A convenience target to run our OS with exactly 32 MB of RAM constraint
drive.img:
	qemu-img create -f qcow2 drive.img 10M

run: osisos.iso drive.img
	qemu-system-i386 -m 32M -cdrom osisos.iso -hda drive.img -netdev user,id=u1 -device e1000,netdev=u1

# Pattern rule to automatically compile any .c file into a .o file
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Pattern rule to automatically assemble any .S file into a .o file
%.o: %.S
	$(AS) -m32 -c $< -o $@

# Link all the object files together to create the final kernel binary
osisos.bin: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o osisos.bin

# Generate the bootable ISO file
osisos.iso: osisos.bin
	mkdir -p isodir/boot/grub
	cp osisos.bin isodir/boot/osisos.bin
	echo 'menuentry "OSISOS" {' > isodir/boot/grub/grub.cfg
	echo '	multiboot /boot/osisos.bin' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o osisos.iso isodir

# Clean up all compiled files
clean:
	rm -f $(OBJS) osisos.bin osisos.iso
	rm -rf isodir
