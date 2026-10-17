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
static bool ofs_ready = false;     /* disk present and filesystem usable */

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
    ofs_ready = true;
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
    
    ofs_ready = true;
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
            /* `buffer` is a single 512-byte sector. Files can now be larger than that (and a
             * 512-byte file used to write the terminator one byte past the buffer), so only
             * the first sector is returned, always NUL-terminated inside the buffer. */
            ide_read_sector(inodes[i].start_block, (uint8_t*)buffer);
            uint32_t n = inodes[i].size;
            if (n > 511) n = 511;
            buffer[n] = '\0';
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


/* ======================================================================== */
/* Path-based API                                                            */
/* ======================================================================== */

bool ofs_available(void) { return ofs_ready; }

int ofs_lookup(const char* path) {
    if (!ofs_ready || !path) return -1;
    int cur = OFS_ROOT_DIR;
    const char* p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[OFS_NAME_MAX + 1];
        int n = 0;
        while (*p && *p != '/') {
            if (n >= OFS_NAME_MAX) return -1;             /* name too long: cannot exist */
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        if (cur != OFS_ROOT_DIR && inodes[cur].type != OFS_TYPE_DIR) return -1;   /* file used as dir */

        int found = -1;
        for (int i = 0; i < MAX_FILES; i++) {
            if (inodes[i].filename[0] != '\0' && inodes[i].parent_idx == cur &&
                ofs_streq(inodes[i].filename, comp)) { found = i; break; }
        }
        if (found < 0) return -1;
        cur = found;
    }
    return cur;
}

bool ofs_stat(const char* path, ofs_stat_t* out) {
    int idx = ofs_lookup(path);
    if (idx < 0) return false;
    if (idx == OFS_ROOT_DIR) { out->type = OFS_TYPE_DIR; out->size = 0; return true; }
    out->type = inodes[idx].type;
    out->size = inodes[idx].type == OFS_TYPE_FILE ? inodes[idx].size : 0;
    return true;
}

int ofs_inode_read(int idx, uint32_t offset, void* buf, uint32_t len) {
    if (!ofs_ready || idx < 0 || idx >= MAX_FILES) return -1;
    if (inodes[idx].filename[0] == '\0' || inodes[idx].type != OFS_TYPE_FILE) return -1;

    uint32_t size = inodes[idx].size;
    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;

    uint8_t* out = (uint8_t*)buf;
    uint8_t sector[512];
    uint32_t done = 0;
    while (done < len) {
        uint32_t pos = offset + done;
        uint32_t in_sector = pos % 512;
        uint32_t chunk = 512 - in_sector;
        if (chunk > len - done) chunk = len - done;

        ide_read_sector(inodes[idx].start_block + pos / 512, sector);
        for (uint32_t i = 0; i < chunk; i++) out[done + i] = sector[in_sector + i];
        done += chunk;
    }
    return (int)done;
}

int ofs_readdir(int dir_idx, int pos, ofs_dirent_t* out) {
    if (!ofs_ready || pos < 0) return -1;
    for (int i = pos; i < MAX_FILES; i++) {
        if (inodes[i].filename[0] != '\0' && inodes[i].parent_idx == dir_idx) {
            ofs_strcpy(out->name, inodes[i].filename);
            out->type = inodes[i].type;
            out->size = inodes[i].type == OFS_TYPE_FILE ? inodes[i].size : 0;
            return i + 1;
        }
    }
    return -1;
}

bool ofs_put(const char* path, const void* data, uint32_t len) {
    if (!ofs_ready || !path) return false;

    /* Split into parent directory + leaf name. */
    size_t plen = ofs_strlen(path);
    while (plen > 1 && path[plen - 1] == '/') plen--;
    size_t slash = plen;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    size_t leaf_len = plen - slash;
    if (leaf_len == 0 || leaf_len > OFS_NAME_MAX) return false;

    char leaf[OFS_NAME_MAX + 1];
    for (size_t i = 0; i < leaf_len; i++) leaf[i] = path[slash + i];
    leaf[leaf_len] = '\0';

    char parent_path[128];
    if (slash >= sizeof(parent_path)) return false;
    for (size_t i = 0; i < slash; i++) parent_path[i] = path[i];
    parent_path[slash] = '\0';

    int parent = ofs_lookup(parent_path[0] ? parent_path : "/");
    if (parent < 0) return false;
    if (parent != OFS_ROOT_DIR && inodes[parent].type != OFS_TYPE_DIR) return false;

    /* Existing file (reuse its blocks if the new data fits) or a free inode. */
    int idx = -1, free_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (inodes[i].filename[0] == '\0') { if (free_idx < 0) free_idx = i; }
        else if (inodes[i].parent_idx == parent && ofs_streq(inodes[i].filename, leaf)) {
            if (inodes[i].type != OFS_TYPE_FILE) return false;      /* it is a directory */
            idx = i;
        }
    }
    if (idx < 0) idx = free_idx;
    if (idx < 0) return false;                                      /* inode table full */

    uint32_t blocks = (len + 511) / 512;
    if (blocks == 0) blocks = 1;
    if (blocks > OFS_MAX_FILE_BLOCKS) return false;

    bool reuse = (inodes[idx].filename[0] != '\0');
    if (reuse) {
        uint32_t old_blocks = (inodes[idx].size + 511) / 512;
        if (old_blocks == 0) old_blocks = 1;
        if (blocks > old_blocks) reuse = false;                     /* needs fresh space */
    }

    uint32_t start;
    if (reuse) {
        start = inodes[idx].start_block;
    } else {
        start = next_free_data_block;
        if (start + blocks > 20480) return false;                   /* disk full (10 MB) */
    }

    /* 1. data first ... */
    const uint8_t* src = (const uint8_t*)data;
    uint8_t sector[512];
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t off = b * 512;
        for (uint32_t i = 0; i < 512; i++) sector[i] = (off + i < len) ? src[off + i] : 0;
        ide_write_sector(start + b, sector);
    }

    /* 2. ... then publish the inode, so readers never see a half-written file */
    uint32_t irq;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(irq) : : "memory");
    ofs_strcpy(inodes[idx].filename, leaf);
    inodes[idx].type = OFS_TYPE_FILE;
    inodes[idx].parent_idx = (uint8_t)parent;
    inodes[idx].size = len;
    inodes[idx].start_block = start;
    if (!reuse && start + blocks > next_free_data_block) next_free_data_block = start + blocks;
    if (irq & 0x200) __asm__ volatile ("sti" : : : "memory");

    if (idx < 16) ide_write_sector(1, (uint8_t*)&inodes[0]);
    else          ide_write_sector(2, (uint8_t*)&inodes[16]);
    return true;
}
