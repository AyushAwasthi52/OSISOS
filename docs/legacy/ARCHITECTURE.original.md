# OSISOS Architecture

OSISOS is an independently developed, standalone operating system kernel. 

## Separation of Concerns

The repository is strictly divided into two distinct environments to ensure no hidden dependencies on Linux:

### 1. `host/`
Contains a Linux-hosted prototype and reference implementation of the OSISOS API. This allows for rapid experimentation with shell utilities, API design, and userland logic. It relies on standard Linux system calls (e.g., `fork`, `execvp`, `pthread`) and POSIX libraries. **Code in `host/` must never be compiled into or linked with the OSISOS kernel.**

### 2. `kernel/`
Contains the actual standalone OSISOS kernel. This code is compiled as a freestanding environment (`-ffreestanding`, `-nostdlib`). It has absolutely no dependencies on Linux. It provides its own hardware initialization, memory management, scheduling, filesystem, and driver ecosystem.

## Boot Flow

The OSISOS kernel targets the **i686 (32-bit x86)** architecture for its initial boot phase. This is a deliberate educational choice to incrementally understand the early boot process before transitioning to long mode (x86_64).

The kernel adheres to the **Multiboot1** specification. The complete boot chain is as follows:

1. **BIOS / UEFI**: The hardware initializes and loads the bootloader from the boot medium.
2. **GRUB (Bootloader)**: GRUB parses the ISO, reads its configuration (`grub.cfg`), and locates the `osisos.bin` kernel ELF file.
3. **Multiboot Detection**: GRUB scans the first 8 KiB of the kernel for a valid Multiboot header (defined in `boot.S`).
4. **Environment Setup**: GRUB transitions the CPU into 32-bit Protected Mode, disables interrupts, and loads the kernel into physical memory starting at 1 MiB.
5. **Kernel Entry (`_start`)**: Execution jumps to the `_start` assembly routine in `boot.S`, which allocates a kernel stack.
6. **C Entry (`kernel_main`)**: The assembly routine calls `kernel_main()` in `kernel.c`, transferring control to high-level C code.
7. **Execution**: The OSISOS kernel now runs entirely independently, executing hardware I/O directly (such as writing to the VGA text buffer).
