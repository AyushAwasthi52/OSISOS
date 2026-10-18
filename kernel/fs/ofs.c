#include "ofs.h"
#include "ide.h"
#include "kheap.h"
#include "klog.h"

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

extern void terminal_print_string(const char* str);
extern void terminal_print_dec(uint32_t val);

/* We store the inodes in memory for fast access */
ofs_inode_t inodes[MAX_FILES];
uint8_t current_dir_idx = OFS_ROOT_DIR;
static bool ofs_ready = false;     /* disk present and filesystem usable */

static inline uint32_t irq_save(void) {
    uint32_t f; __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory"); return f;
}
static inline void irq_restore(uint32_t f) { if (f & 0x200) __asm__ volatile ("sti" : : : "memory"); }

/* Write back the inode-table sector that contains inode `idx`. */
static void inode_flush(int idx) {
    int sector = idx / 16;
    ide_write_sector(1 + (uint32_t)sector, (uint8_t*)&inodes[sector * 16]);
}
static void inodes_flush_all(void) {
    for (int s = 0; s < OFS_INODE_SECTORS; s++) ide_write_sector(1 + (uint32_t)s, (uint8_t*)&inodes[s * 16]);
}

static uint32_t blocks_of(const ofs_inode_t* n) {
    uint32_t b = (n->size + 511) / 512;
    return b ? b : 1;
}

/* First-fit search for `blocks` contiguous free data blocks, derived from the inode
 * table itself (no bitmap to get out of sync). Returns 0 if there is no room. */
static uint32_t alloc_extent(uint32_t blocks) {
    static struct { uint32_t start, end; } ext[MAX_FILES];
    int n = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (inodes[i].filename[0] != '\0' && inodes[i].type == OFS_TYPE_FILE) {
            ext[n].start = inodes[i].start_block;
            ext[n].end = inodes[i].start_block + blocks_of(&inodes[i]);
            n++;
        }
    }
    for (int i = 1; i < n; i++) {                       /* insertion sort by start */
        uint32_t s = ext[i].start, e = ext[i].end;
        int j = i - 1;
        while (j >= 0 && ext[j].start > s) { ext[j + 1] = ext[j]; j--; }
        ext[j + 1].start = s; ext[j + 1].end = e;
    }
    uint32_t cursor = OFS_DATA_START;
    for (int i = 0; i < n; i++) {
        if (ext[i].start >= cursor + blocks) return cursor;   /* the gap before this extent fits */
        if (ext[i].end > cursor) cursor = ext[i].end;
    }
    return (cursor + blocks <= OFS_TOTAL_BLOCKS) ? cursor : 0;
}

/* v1 -> v2: v1 kept 32 inodes in blocks 1-2 and data from block 3. Shift every file's
 * data up to make room for the bigger inode table (highest extents first, last block
 * first, so nothing is overwritten before it has been copied). */
static bool migrate_v1(void) {
    static ofs_inode_t old[32];
    ide_read_sector(1, (uint8_t*)&old[0]);
    ide_read_sector(2, (uint8_t*)&old[16]);

    const uint32_t shift = OFS_DATA_START - 3;
    bool moved[32];
    for (int i = 0; i < 32; i++) moved[i] = false;

    uint8_t tmp[512];
    int nfiles = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < 32; i++) {
            if (old[i].filename[0] && old[i].type == OFS_TYPE_FILE && !moved[i] &&
                (best < 0 || old[i].start_block > old[best].start_block)) best = i;
        }
        if (best < 0) break;
        moved[best] = true;
        uint32_t blocks = blocks_of(&old[best]);
        if (old[best].start_block < 3 || old[best].start_block + blocks + shift > OFS_TOTAL_BLOCKS) return false;
        for (int b = (int)blocks - 1; b >= 0; b--) {
            ide_read_sector(old[best].start_block + (uint32_t)b, tmp);
            ide_write_sector(old[best].start_block + (uint32_t)b + shift, tmp);
        }
        old[best].start_block += shift;
        nfiles++;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (i < 32 && old[i].filename[0]) inodes[i] = old[i];
        else { uint8_t* p = (uint8_t*)&inodes[i]; for (unsigned k = 0; k < sizeof(ofs_inode_t); k++) p[k] = 0; }
    }
    inodes_flush_all();

    ofs_superblock_t sb;                                 /* superblock last: marks the migration as done */
    sb.magic = OFS_MAGIC; sb.total_inodes = MAX_FILES; sb.total_blocks = OFS_TOTAL_BLOCKS;
    for (int i = 0; i < 500; i++) sb.padding[i] = 0;
    ide_write_sector(0, (uint8_t*)&sb);

    klog("ofs: migrated v1 disk to v2, %d files kept", nfiles);
    terminal_print_string("OFS: upgraded disk v1 -> v2 (");
    terminal_print_dec((uint32_t)nfiles);
    terminal_print_string(" files kept, 128 inodes now)\n");
    return true;
}

void ofs_format(void) {
    ofs_superblock_t sb;
    sb.magic = OFS_MAGIC;
    sb.total_inodes = MAX_FILES;
    sb.total_blocks = OFS_TOTAL_BLOCKS;
    for (int i = 0; i < 500; i++) sb.padding[i] = 0;
    ide_write_sector(0, (uint8_t*)&sb);

    for (int i = 0; i < MAX_FILES; i++) {
        uint8_t* p = (uint8_t*)&inodes[i];
        for (unsigned k = 0; k < sizeof(ofs_inode_t); k++) p[k] = 0;
    }
    inodes_flush_all();
    ofs_ready = true;
}

bool init_ofs(void) {
    uint8_t buffer[512];
    ide_read_sector(0, buffer);
    ofs_superblock_t* sb = (ofs_superblock_t*) buffer;

    if (sb->magic == OFS_MAGIC_V1) {                     /* old disk: upgrade it, never wipe it */
        if (!migrate_v1()) return false;
        ofs_ready = true;
        return true;
    }
    if (sb->magic != OFS_MAGIC) return false;            /* blank / foreign disk: caller formats */

    for (int s = 0; s < OFS_INODE_SECTORS; s++) ide_read_sector(1 + (uint32_t)s, (uint8_t*)&inodes[s * 16]);
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
    if (ofs_strlen(filename) > OFS_NAME_MAX || filename[0] == '\0') return false;   /* would overflow the name field */
    
    size_t len = ofs_strlen(data);
    if (len > 512) return false; // the shell editor only produces 1-sector files
    
    uint32_t new_start = 0;
    if (!exists) {
        new_start = alloc_extent(1);
        if (!new_start) return false;                      // disk full
    }

    // Populate the Inode
    ofs_strcpy(inodes[target_idx].filename, filename);
    inodes[target_idx].size = len;
    inodes[target_idx].type = OFS_TYPE_FILE;
    inodes[target_idx].parent_idx = current_dir_idx;
    
    // If it's a new file, give it a new block. If existing, reuse the block.
    if (!exists) {
        inodes[target_idx].start_block = new_start;
    }
    
    // Write the data to the data block
    uint8_t buffer[512];
    for (int i = 0; i < 512; i++) {
        if (i < len) buffer[i] = data[i];
        else buffer[i] = 0;
    }
    ide_write_sector(inodes[target_idx].start_block, buffer);
    
    // Save the updated Inode table to disk
    inode_flush(target_idx);
    
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
    if (ofs_strlen(dirname) > OFS_NAME_MAX || dirname[0] == '\0') return false;
    
    ofs_strcpy(inodes[target_idx].filename, dirname);
    inodes[target_idx].type = OFS_TYPE_DIR;
    inodes[target_idx].parent_idx = current_dir_idx;
    inodes[target_idx].size = 0;
    inodes[target_idx].start_block = 0; // Directories don't need data blocks yet
    
    inode_flush(target_idx);
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

/* "/a/b/c" -> parent "/a/b", leaf "c" */
static bool split_path(const char* path, char* parent, size_t pmax, char* leaf) {
    size_t plen = ofs_strlen(path);
    while (plen > 1 && path[plen - 1] == '/') plen--;
    size_t slash = plen;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    size_t leaf_len = plen - slash;
    if (leaf_len == 0 || leaf_len > OFS_NAME_MAX || slash >= pmax) return false;
    for (size_t i = 0; i < leaf_len; i++) leaf[i] = path[slash + i];
    leaf[leaf_len] = '\0';
    for (size_t i = 0; i < slash; i++) parent[i] = path[i];
    parent[slash] = '\0';
    return true;
}

bool ofs_put(const char* path, const void* data, uint32_t len) {
    if (!ofs_ready || !path) return false;

    char leaf[OFS_NAME_MAX + 1], parent_path[128];
    if (!split_path(path, parent_path, sizeof(parent_path), leaf)) return false;

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

    bool existing = (inodes[idx].filename[0] != '\0');
    uint32_t start;
    if (existing && blocks <= blocks_of(&inodes[idx])) {
        start = inodes[idx].start_block;                            /* fits where it is */
    } else {
        /* New space. The old extent stays reserved until the new inode is published, so a
         * reader that is streaming the old version is never overwritten underneath. */
        start = alloc_extent(blocks);
        if (!start) return false;                                   /* disk full / too fragmented */
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
    uint32_t irq = irq_save();
    ofs_strcpy(inodes[idx].filename, leaf);
    inodes[idx].type = OFS_TYPE_FILE;
    inodes[idx].parent_idx = (uint8_t)parent;
    inodes[idx].size = len;
    inodes[idx].start_block = start;
    irq_restore(irq);

    inode_flush(idx);
    return true;
}

bool ofs_remove(const char* path) {
    if (!ofs_ready) return false;
    int idx = ofs_lookup(path);
    if (idx < 0 || idx == OFS_ROOT_DIR) return false;
    if (inodes[idx].type == OFS_TYPE_DIR) {
        for (int i = 0; i < MAX_FILES; i++)
            if (inodes[i].filename[0] != '\0' && inodes[i].parent_idx == idx) return false;   /* not empty */
        if (current_dir_idx == idx) return false;                                           /* you are standing in it */
    }
    uint32_t irq = irq_save();
    uint8_t* p = (uint8_t*)&inodes[idx];
    for (unsigned k = 0; k < sizeof(ofs_inode_t); k++) p[k] = 0;
    irq_restore(irq);
    inode_flush(idx);                                  /* its blocks are free again: extents are derived from inodes */
    return true;
}

bool ofs_rename(const char* from, const char* to) {
    if (!ofs_ready) return false;
    int idx = ofs_lookup(from);
    if (idx < 0 || idx == OFS_ROOT_DIR) return false;

    int new_parent;
    char leaf[OFS_NAME_MAX + 1];
    int target = ofs_lookup(to);
    if (target >= 0) {
        /* `to` exists: only OK if it is a directory to move into */
        if (target != OFS_ROOT_DIR && inodes[target].type != OFS_TYPE_DIR) return false;
        new_parent = target;
        ofs_strcpy(leaf, inodes[idx].filename);
    } else {
        char parent_path[128];
        if (!split_path(to, parent_path, sizeof(parent_path), leaf)) return false;
        new_parent = ofs_lookup(parent_path[0] ? parent_path : "/");
        if (new_parent < 0) return false;
        if (new_parent != OFS_ROOT_DIR && inodes[new_parent].type != OFS_TYPE_DIR) return false;
    }

    /* never move a directory into itself or one of its own descendants */
    if (inodes[idx].type == OFS_TYPE_DIR) {
        for (int p = new_parent, guard = 0; p != OFS_ROOT_DIR && guard < MAX_FILES; p = inodes[p].parent_idx, guard++)
            if (p == idx) return false;
    }
    if (new_parent == inodes[idx].parent_idx && ofs_streq(leaf, inodes[idx].filename)) return true;   /* no-op */
    for (int i = 0; i < MAX_FILES; i++)                                                  /* name collision */
        if (inodes[i].filename[0] != '\0' && i != idx && inodes[i].parent_idx == new_parent && ofs_streq(inodes[i].filename, leaf))
            return false;

    uint32_t irq = irq_save();
    ofs_strcpy(inodes[idx].filename, leaf);
    inodes[idx].parent_idx = (uint8_t)new_parent;
    irq_restore(irq);
    inode_flush(idx);
    return true;
}

void ofs_usage(ofs_usage_t* out) {
    out->total_blocks = OFS_TOTAL_BLOCKS - OFS_DATA_START;
    out->used_blocks = 0; out->files = 0; out->dirs = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (inodes[i].filename[0] == '\0') continue;
        if (inodes[i].type == OFS_TYPE_DIR) out->dirs++;
        else { out->files++; out->used_blocks += blocks_of(&inodes[i]); }
    }
    out->inodes_total = MAX_FILES;
    out->inodes_free = MAX_FILES - out->files - out->dirs;
}

bool ofs_abspath(const char* in, char* out, uint32_t max) {
    /* Collect the components of (cwd + in), applying "." and ".." as we go. */
    char comps[16][OFS_NAME_MAX + 1];
    int n = 0;

    if (in[0] != '/') {                                  /* start from the current directory */
        int chain[16], depth = 0;
        for (int d = current_dir_idx; d != OFS_ROOT_DIR && depth < 16; d = inodes[d].parent_idx) chain[depth++] = d;
        for (int i = depth - 1; i >= 0; i--) { ofs_strcpy(comps[n], inodes[chain[i]].filename); n++; }
    }
    const char* p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char c[OFS_NAME_MAX + 1];
        int k = 0;
        while (*p && *p != '/') { if (k >= OFS_NAME_MAX) return false; c[k++] = *p++; }
        c[k] = '\0';
        if (ofs_streq(c, ".")) continue;
        if (ofs_streq(c, "..")) { if (n > 0) n--; continue; }
        if (n >= 16) return false;
        ofs_strcpy(comps[n++], c);
    }
    uint32_t o = 0;
    if (max < 2) return false;
    out[o++] = '/';
    for (int i = 0; i < n; i++) {
        size_t l = ofs_strlen(comps[i]);
        if (o + l + 1 >= max) return false;
        for (size_t k = 0; k < l; k++) out[o++] = comps[i][k];
        if (i + 1 < n) out[o++] = '/';
    }
    out[o] = '\0';
    return true;
}
