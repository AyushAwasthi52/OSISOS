#include "ide.h"
#include "io.h"

/* Wait for the drive to be ready */
static void ide_wait_ready(void) {
    while ((inb(ATA_PRIMARY_STATUS) & 0xC0) != 0x40) {
        // Wait until BSY (bit 7) is clear and RDY (bit 6) is set
    }
}

/* Wait for the drive to have data ready to transfer */
static void ide_wait_drq(void) {
    while (!(inb(ATA_PRIMARY_STATUS) & 0x08)) {
        // Wait until DRQ (bit 3) is set
    }
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
    if (status == 0) {
        return false; // Drive does not exist
    }
    
    // Wait until BSY clears
    while (inb(ATA_PRIMARY_STATUS) & 0x80);
    
    // Check if it's an ATAPI device (CD-ROM)
    uint8_t lba_mid = inb(ATA_PRIMARY_LBA_MID);
    uint8_t lba_hi = inb(ATA_PRIMARY_LBA_HI);
    if (lba_mid != 0 || lba_hi != 0) {
        return false; // Not a standard ATA hard drive
    }
    
    // Wait for DRQ or ERR
    while (true) {
        status = inb(ATA_PRIMARY_STATUS);
        if (status & 0x01) return false; // Error occurred
        if (status & 0x08) break; // DRQ set, data is ready
    }
    
    // Read the 256 16-bit words of identification data
    uint16_t identify_data[256];
    insw(ATA_PRIMARY_DATA, identify_data, 256);
    
    return true; // Drive successfully initialized!
}

void ide_read_sector(uint32_t lba, uint8_t* buffer) {
    ide_wait_ready();
    
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
    ide_wait_drq();
    
    // Read 256 words (512 bytes) into the buffer
    insw(ATA_PRIMARY_DATA, buffer, 256);
}

void ide_write_sector(uint32_t lba, uint8_t* buffer) {
    ide_wait_ready();
    
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
    ide_wait_drq();
    
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
}
