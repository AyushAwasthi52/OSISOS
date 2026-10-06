#ifndef OFS_H
#define OFS_H

#include <stdint.h>
#include <stdbool.h>

#define OFS_MAGIC 0x05150515
#define MAX_FILES 32

/* 
 * The Superblock sits at LBA 0.
 * It identifies the disk as an OSISOS formatted drive.
 */
typedef struct {
    uint32_t magic;
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint8_t  padding[500]; // Pad to exactly 512 bytes
} __attribute__((packed)) ofs_superblock_t;

/* 
 * An Inode describes a single file. 
 * Exactly 32 bytes long, so 16 inodes fit perfectly in one 512-byte sector.
 */
typedef struct {
    char filename[24];
    uint32_t size;        // File size in bytes
    uint32_t start_block; // The LBA where the file's data begins
} __attribute__((packed)) ofs_inode_t;

/* Format the hard drive, destroying all data! */
void ofs_format(void);

/* Initialize the filesystem (reads superblock) */
bool init_ofs(void);

/* Create a new file, and write a single string to it */
bool ofs_write_file(const char* filename, const char* data);

/* Read a file's contents into a buffer */
bool ofs_read_file(const char* filename, char* buffer);

#endif
