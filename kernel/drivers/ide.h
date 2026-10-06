#ifndef IDE_H
#define IDE_H

#include <stdint.h>
#include <stdbool.h>

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERR          0x1F1
#define ATA_PRIMARY_SECCOUNT     0x1F2
#define ATA_PRIMARY_LBA_LO       0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HI       0x1F5
#define ATA_PRIMARY_DRV_HEAD     0x1F6
#define ATA_PRIMARY_COMMAND      0x1F7
#define ATA_PRIMARY_STATUS       0x1F7

#define ATA_CMD_READ_PIO         0x20
#define ATA_CMD_WRITE_PIO        0x30
#define ATA_CMD_CACHE_FLUSH      0xE7
#define ATA_CMD_IDENTIFY         0xEC

/* Initialize the IDE driver and detect the drive */
bool init_ide(void);

/* Read a 512-byte sector from the hard drive */
void ide_read_sector(uint32_t lba, uint8_t* buffer);

/* Write a 512-byte sector to the hard drive */
void ide_write_sector(uint32_t lba, uint8_t* buffer);

#endif
