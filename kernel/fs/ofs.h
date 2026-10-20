#ifndef OFS_H
#define OFS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * On-disk layout (OFS v2):
 *   block 0        superblock
 *   blocks 1-8     inode table (128 inodes x 32 bytes)
 *   blocks 9..     file data (every file is one contiguous extent)
 * v1 disks (32 inodes, data from block 3) are migrated automatically at mount.
 */
#define OFS_MAGIC     0x05150516          /* v2 */
#define OFS_MAGIC_V1  0x05150515
#define MAX_FILES     128
#define OFS_INODE_SECTORS (MAX_FILES / 16)
#define OFS_DATA_START    (1 + OFS_INODE_SECTORS)
#define OFS_TOTAL_BLOCKS  20480           /* 10 MB / 512 */

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
#define OFS_TYPE_FILE 0
#define OFS_TYPE_DIR  1
#define OFS_ROOT_DIR  255

typedef struct {
    char filename[22];
    uint8_t type;         // 0 = File, 1 = Directory
    uint8_t parent_idx;   // 255 = Root directory, or 0-31 for a parent directory
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

/* List all files in the filesystem */
void ofs_list_files(void);

/* Create a new directory */
bool ofs_mkdir(const char* dirname);

/* Change current directory */
bool ofs_change_dir(const char* dirname);


/* ------------------------------------------------------------------------
 * Path-based API (used by the HTTP server and `fetch`).
 *
 * Unlike the functions above these never look at (or change) the shell's current
 * directory: paths are absolute ("/www/index.html"), '/' separated, components
 * up to 21 characters. Files may span many sectors (stored contiguously).
 * ------------------------------------------------------------------------ */

#define OFS_NAME_MAX 21
#define OFS_MAX_FILE_BLOCKS 4096      /* 2 MB per file */

typedef struct {
    uint8_t  type;                    /* OFS_TYPE_FILE or OFS_TYPE_DIR */
    uint32_t size;                    /* bytes (0 for directories)     */
} ofs_stat_t;

typedef struct {
    char     name[OFS_NAME_MAX + 1];
    uint8_t  type;
    uint32_t size;
} ofs_dirent_t;

/* True once the disk has been detected and the filesystem mounted/formatted. */
bool ofs_available(void);

/* Inode number of `path`, OFS_ROOT_DIR (255) for "/", or -1 if it does not exist. */
int  ofs_lookup(const char* path);
bool ofs_stat(const char* path, ofs_stat_t* out);

/* Read up to `len` bytes starting at byte `offset` from file inode `idx`.
 * Returns the number of bytes read (0 at/after EOF), or -1 on error. */
int  ofs_inode_read(int idx, uint32_t offset, void* buf, uint32_t len);

/* Directory iteration: pass pos = 0 first; returns the next pos, or -1 when done. */
int  ofs_readdir(int dir_idx, int pos, ofs_dirent_t* out);

/* Create or replace a file of any size (parent directory must exist). */
bool ofs_put(const char* path, const void* data, uint32_t len);

/* Delete a file or an EMPTY directory. */
bool ofs_remove(const char* path);

/* Rename / move. If `to` is an existing directory the entry is moved into it.
 * Refuses to overwrite and to move a directory into itself. */
bool ofs_rename(const char* from, const char* to);

typedef struct {
    uint32_t total_blocks, used_blocks;   /* data blocks */
    uint32_t files, dirs;
    uint32_t inodes_total, inodes_free;
} ofs_usage_t;
void ofs_usage(ofs_usage_t* out);

/* Make `in` absolute: relative paths are resolved against the shell's current
 * directory; "." and ".." are normalised. Returns false if it does not fit. */
bool ofs_abspath(const char* in, char* out, uint32_t max);

#endif
