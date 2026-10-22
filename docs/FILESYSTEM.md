# OFS v2 - the OSISOS file system

A small, simple, crash-tolerant file system on a raw ATA disk (10 MB addressable, 512-byte sectors).

## On-disk format

```
block 0        superblock   { magic 0x05150516, total_inodes=128, total_blocks=20480, padding }
blocks 1-8     inode table  128 inodes x 32 bytes, 16 per sector
blocks 9..     data         every file is ONE contiguous run of blocks ("extent")
```

Inode (32 bytes, packed):

| Field | Size | Meaning |
|---|---|---|
| `filename` | 22 | NUL-terminated, at most 21 characters; **empty = free inode** |
| `type` | 1 | 0 = file, 1 = directory |
| `parent_idx` | 1 | inode number of the parent directory, 255 = the root |
| `size` | 4 | bytes (0 for directories) |
| `start_block` | 4 | first data block (files only) |

The root directory has no inode. Directories have no data blocks: a directory is just "the inodes whose
`parent_idx` points at it". Maximum file size 2 MB (4096 blocks).

## Allocation (no bitmap)

Free space is **derived from the inode table**: collect every file's extent `[start, start+blocks)`, sort by start,
and first-fit into the gaps (from block 9 up to block 20480). There is no free-space structure that can disagree
with the inodes, so deleting a file (clearing its inode) frees its blocks instantly, and shrinking a file frees its
tail. The cost is that a very fragmented disk can refuse a large file even if the total free space is sufficient.

## Safety properties

* **Publish order.** `ofs_put` writes all data blocks first and updates the inode last, so a reader never sees a
  half-written file, and an interrupted write leaves the old version intact. When a file grows, the old extent stays
  reserved until the new inode is published.
* **No concurrent ATA sequences.** Every sector transfer runs with interrupts disabled with bounded waits
  (`drivers/ide.c`), because the shell and network tasks both use the disk.
* **Path lookups are independent of `cd`.** The `ofs_*` path API never touches the shell's current directory.
* **Bounds.** Names longer than 21 characters are rejected (the original code overflowed the field).
* Not provided: journaling, permissions, timestamps, hard/soft links, multi-sector atomicity.

## API (`fs/ofs.h`)

| Function | Purpose |
|---|---|
| `init_ofs()`, `ofs_format()` | mount (migrating v1) / format; `ofs_available()` tells whether a disk is usable |
| `ofs_lookup(path)` | inode number, `OFS_ROOT_DIR` (255) for `/`, or -1 |
| `ofs_stat(path, &st)` | type and size |
| `ofs_inode_read(idx, offset, buf, len)` | read a byte range (any size) |
| `ofs_readdir(dir, pos, &entry)` | iterate a directory |
| `ofs_put(path, data, len)` | create or replace a file of any size |
| `ofs_remove(path)` | delete a file or an empty directory (not the current directory) |
| `ofs_rename(from, to)` | rename/move; moves into `to` if it is a directory; refuses overwrite and cycles |
| `ofs_usage(&u)` | blocks used/free, files, dirs, free inodes |
| `ofs_abspath(in, out, max)` | resolve a relative path against the shell's directory, normalising `.`/`..` |
| legacy: `ofs_write_file`, `ofs_read_file`, `ofs_mkdir`, `ofs_change_dir`, `ofs_list_files` | relative-to-cwd API used by `edit`, `cat`, `mkdir`, `cd`, `ls` (1-sector files) |

## Migration from v1

v1 had 32 inodes in blocks 1-2 and data from block 3. At mount, a v1 disk is **upgraded in place** (never
formatted): files are relocated 6 blocks upward (highest extent first, last block first, so nothing is overwritten
before it is copied), the 32 inodes are copied into the larger table, and the new superblock is written last. The
kernel log records `ofs: migrated v1 disk to v2, N files kept`. The migration is covered by a test that builds a v1
disk image by hand.

## Files the system itself creates

| Path | Created by | Contents |
|---|---|---|
| `hello.txt` | first format | a welcome text |
| `/etc/shadow` | first boot | `name:iterations:salt-hex:hash-hex:must-change` per user |
| `/etc/rcon` | `rcon on` | presence = remote console enabled |

`/etc` is hidden from the web server whatever the document root is.
