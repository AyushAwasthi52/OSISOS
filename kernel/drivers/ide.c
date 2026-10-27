#include "ide.h"
#include "io.h"

/*
 * The ATA registers are one shared resource and every transfer is a multi-step
 * sequence (select, LBA, command, wait, data). The PIT preempts tasks, so without
 * protection the shell task and the network task (HTTP server reading files) could
 * interleave two sequences and corrupt both. Each sector transfer therefore runs with
 * interrupts disabled (uniprocessor mutual exclusion), and the waits are bounded so a
 * dead drive cannot freeze the machine while interrupts are off.
 */
#define IDE_SPIN_LIMIT 4000000u

static inline uint32_t ide_lock(void) {
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void ide_unlock(uint32_t flags) {
    if (flags & 0x200) __asm__ volatile ("sti" : : : "memory");
}

/* Wait for the drive to be ready (BSY clear, RDY set). Returns false on timeout. */
static bool ide_wait_ready(void) {
    for (uint32_t i = 0; i < IDE_SPIN_LIMIT; i++) {
        if ((inb(ATA_PRIMARY_STATUS) & 0xC0) == 0x40) return true;
    }
    return false;
}

/* Wait for DRQ (data ready). Returns false on timeout or drive error. */
static bool ide_wait_drq(void) {
    for (uint32_t i = 0; i < IDE_SPIN_LIMIT; i++) {
        uint8_t st = inb(ATA_PRIMARY_STATUS);
        if (st & 0x01) return false;          /* ERR */
        if (st & 0x08) return true;           /* DRQ */
    }
    return false;
}

bool init_ide(void) {
    // Select the Primary Master drive
    outb(ATA_PRIMARY_DRV_HEAD, 0xA0);
    
    // Clear Sectorcount, LBA Low, Mid, High
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    
    // Send IDENTIFY command
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_IDENTIFY);
    
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0 || status == 0xFF) {
        return false; // Drive does not exist or floating bus
    }
    
    // Wait until BSY clears (bounded timeout)
    bool bsy_cleared = false;
    for (uint32_t i = 0; i < IDE_SPIN_LIMIT; i++) {
        if (!(inb(ATA_PRIMARY_STATUS) & 0x80)) {
            bsy_cleared = true;
            break;
        }
    }
    if (!bsy_cleared) return false;
    
    // Check if it's an ATAPI device (CD-ROM)
    uint8_t lba_mid = inb(ATA_PRIMARY_LBA_MID);
    uint8_t lba_hi = inb(ATA_PRIMARY_LBA_HI);
    if (lba_mid != 0 || lba_hi != 0) {
        return false; // Not a standard ATA hard drive
    }
    
    // Wait for DRQ or ERR (bounded timeout)
    bool drq_ready = false;
    for (uint32_t i = 0; i < IDE_SPIN_LIMIT; i++) {
        status = inb(ATA_PRIMARY_STATUS);
        if (status & 0x01) return false; // Error occurred
        if (status & 0x08) { drq_ready = true; break; } // DRQ set, data is ready
    }
    if (!drq_ready) return false;
    
    // Read the 256 16-bit words of identification data
    uint16_t identify_data[256];
    insw(ATA_PRIMARY_DATA, identify_data, 256);
    
    return true; // Drive successfully initialized!
}

void ide_read_sector(uint32_t lba, uint8_t* buffer) {
    uint32_t irq = ide_lock();
    if (!ide_wait_ready()) goto fail;
    
    // Select drive and send Highest 4 bits of LBA, plus LBA mode bit (0x40)
    outb(ATA_PRIMARY_DRV_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    
    // Send Sector Count (1 sector)
    outb(ATA_PRIMARY_SECCOUNT, 1);
    
    // Send LBA bits
    outb(ATA_PRIMARY_LBA_LO, (uint8_t) lba);
    outb(ATA_PRIMARY_LBA_MID, (uint8_t) (lba >> 8));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t) (lba >> 16));
    
    // Send Read Command
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_PIO);
    
    // Wait for drive to signal data is ready
    if (!ide_wait_drq()) goto fail;
    
    // Read 256 words (512 bytes) into the buffer
    insw(ATA_PRIMARY_DATA, buffer, 256);
    ide_unlock(irq);
    return;

fail:                                          /* never hand back stale stack garbage */
    for (int i = 0; i < 512; i++) buffer[i] = 0;
    ide_unlock(irq);
}

void ide_write_sector(uint32_t lba, uint8_t* buffer) {
    uint32_t irq = ide_lock();
    if (!ide_wait_ready()) { ide_unlock(irq); return; }
    
    // Select drive and send Highest 4 bits of LBA, plus LBA mode bit (0x40)
    outb(ATA_PRIMARY_DRV_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    
    // Send Sector Count (1 sector)
    outb(ATA_PRIMARY_SECCOUNT, 1);
    
    // Send LBA bits
    outb(ATA_PRIMARY_LBA_LO, (uint8_t) lba);
    outb(ATA_PRIMARY_LBA_MID, (uint8_t) (lba >> 8));
    outb(ATA_PRIMARY_LBA_HI, (uint8_t) (lba >> 16));
    
    // Send Write Command
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_PIO);
    
    // Wait for drive to signal it's ready to receive data
    if (!ide_wait_drq()) { ide_unlock(irq); return; }
    
    // Write 256 words (512 bytes) from the buffer to the drive
    // We need outsw! Wait, we don't have outsw in io.h! 
    // We can just use a loop for now since we haven't written outsw.
    uint16_t* ptr = (uint16_t*) buffer;
    for (int i = 0; i < 256; i++) {
        outw(ATA_PRIMARY_DATA, ptr[i]);
    }
    
    // Send Cache Flush command just to be safe
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_CACHE_FLUSH);
    ide_wait_ready();
    ide_unlock(irq);
}
