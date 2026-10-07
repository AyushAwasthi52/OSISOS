#include "ofs.h"
#include "ide.h"
#include "kheap.h"

// Define a simple string length function since we don't have <string.h>
static size_t ofs_strlen(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') len++;
    return len;
}

// Define a simple string compare
static bool ofs_streq(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (*s1 == *s2);
}

// Define a simple string copy
static void ofs_strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

/* We store the inodes in memory for fast access */
ofs_inode_t inodes[MAX_FILES];
uint32_t next_free_data_block = 3; // Block 0 = Superblock, Blocks 1-2 = Inodes
uint8_t current_dir_idx = OFS_ROOT_DIR;

void ofs_format(void) {
    // 1. Create and write the superblock
    ofs_superblock_t sb;
    sb.magic = OFS_MAGIC;
    sb.total_inodes = MAX_FILES;
    sb.total_blocks = 20480; // 10MB / 512 bytes
    
    // Fill padding with zeros (since we don't have memset)
    for (int i = 0; i < 500; i++) sb.padding[i] = 0;
    
    ide_write_sector(0, (uint8_t*)&sb);
    
    // 2. Clear out the Inodes in memory
    for (int i = 0; i < MAX_FILES; i++) {
        inodes[i].filename[0] = '\0';
        inodes[i].size = 0;
        inodes[i].start_block = 0;
    }
    
    // 3. Write the empty Inodes to disk (Sector 1 and 2)
    // 16 inodes per sector (16 * 32 bytes = 512)
    ide_write_sector(1, (uint8_t*)&inodes[0]);
    ide_write_sector(2, (uint8_t*)&inodes[16]);
    
    next_free_data_block = 3;
}

bool init_ofs(void) {
    uint8_t buffer[512];
    
    // Read the Superblock (Sector 0)
    ide_read_sector(0, buffer);
    ofs_superblock_t* sb = (ofs_superblock_t*) buffer;
    
    if (sb->magic != OFS_MAGIC) {
        // Disk is not formatted!
        return false;
    }
    
    // Read the Inodes from disk into memory
    ide_read_sector(1, (uint8_t*)&inodes[0]);
    ide_read_sector(2, (uint8_t*)&inodes[16]);
    
    // Scan the inodes to find where the next free data block is
    next_free_data_block = 3;
    for (int i = 0; i < MAX_FILES; i++) {
        if (inodes[i].filename[0] != '\0') {
            // Very naive way to calculate blocks used by this file
            uint32_t blocks = (inodes[i].size + 511) / 512;
            if (blocks == 0) blocks = 1;
            uint32_t end_block = inodes[i].start_block + blocks;
            if (end_block > next_free_data_block) {
                next_free_data_block = end_block;
            }
        }
    }
    
    return true;
}

bool ofs_write_file(const char* filename, const char* data) {
    // Find an empty inode, or the existing file's inode
    int target_idx = -1;
    bool exists = false;
    
    for (int i = 0; i < MAX_FILES; i++) {
        if (ofs_streq(inodes[i].filename, filename) && inodes[i].parent_idx == current_dir_idx && inodes[i].type == OFS_TYPE_FILE) {
            target_idx = i;
            exists = true;
            break;
        }
        if (inodes[i].filename[0] == '\0' && target_idx == -1) {
            target_idx = i;
        }
    }
    
    if (target_idx == -1) return false; // No more files allowed!
    
    size_t len = ofs_strlen(data);
    if (len > 512) return false; // Only 1-sector files for this demo!
    
    // Populate the Inode
    ofs_strcpy(inodes[target_idx].filename, filename);
    inodes[target_idx].size = len;
    inodes[target_idx].type = OFS_TYPE_FILE;
    inodes[target_idx].parent_idx = current_dir_idx;
    
    // If it's a new file, give it a new block. If existing, reuse the block.
    if (!exists) {
        inodes[target_idx].start_block = next_free_data_block;
        next_free_data_block++;
    }
    
    // Write the data to the data block
    uint8_t buffer[512];
    for (int i = 0; i < 512; i++) {
        if (i < len) buffer[i] = data[i];
        else buffer[i] = 0;
    }
    ide_write_sector(inodes[target_idx].start_block, buffer);
    
    // Save the updated Inode table to disk
    if (target_idx < 16) {
        ide_write_sector(1, (uint8_t*)&inodes[0]);
    } else {
        ide_write_sector(2, (uint8_t*)&inodes[16]);
    }
    
    return true;
}

bool ofs_read_file(const char* filename, char* buffer) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (ofs_streq(inodes[i].filename, filename) && inodes[i].parent_idx == current_dir_idx && inodes[i].type == OFS_TYPE_FILE) {
            // Found it! Read the first block.
            ide_read_sector(inodes[i].start_block, (uint8_t*)buffer);
            buffer[inodes[i].size] = '\0'; // Null terminate it securely
            return true;
        }
    }
    return false; // File not found
}

extern void terminal_print_string(const char* str);
extern void terminal_print_dec(uint32_t val);

void ofs_list_files(void) {
    int count = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (inodes[i].filename[0] != '\0' && inodes[i].parent_idx == current_dir_idx) {
            if (inodes[i].type == OFS_TYPE_DIR) {
                terminal_print_string(" [DIR] ");
                terminal_print_string(inodes[i].filename);
                terminal_print_string("\n");
            } else {
                terminal_print_string(" - ");
                terminal_print_string(inodes[i].filename);
                terminal_print_string(" (");
                terminal_print_dec(inodes[i].size);
                terminal_print_string(" bytes)\n");
            }
            count++;
        }
    }
    if (count == 0) {
        terminal_print_string("No files found.\n");
    }
}

bool ofs_mkdir(const char* dirname) {
    int target_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (ofs_streq(inodes[i].filename, dirname) && inodes[i].parent_idx == current_dir_idx) {
            return false; // Already exists
        }
        if (inodes[i].filename[0] == '\0' && target_idx == -1) {
            target_idx = i;
        }
    }
    
    if (target_idx == -1) return false;
    
    ofs_strcpy(inodes[target_idx].filename, dirname);
    inodes[target_idx].type = OFS_TYPE_DIR;
    inodes[target_idx].parent_idx = current_dir_idx;
    inodes[target_idx].size = 0;
    inodes[target_idx].start_block = 0; // Directories don't need data blocks yet
    
    if (target_idx < 16) {
        ide_write_sector(1, (uint8_t*)&inodes[0]);
    } else {
        ide_write_sector(2, (uint8_t*)&inodes[16]);
    }
    return true;
}

bool ofs_change_dir(const char* dirname) {
    if (ofs_streq(dirname, "..")) {
        if (current_dir_idx == OFS_ROOT_DIR) return true;
        current_dir_idx = inodes[current_dir_idx].parent_idx;
        return true;
    }
    if (ofs_streq(dirname, "/")) {
        current_dir_idx = OFS_ROOT_DIR;
        return true;
    }
    
    for (int i = 0; i < MAX_FILES; i++) {
        if (ofs_streq(inodes[i].filename, dirname) && inodes[i].parent_idx == current_dir_idx && inodes[i].type == OFS_TYPE_DIR) {
            current_dir_idx = i;
            return true;
        }
    }
    return false;
}
