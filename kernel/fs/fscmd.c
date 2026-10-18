/*
 * fscmd.c - shell commands for the filesystem upgrade.
 *
 *   rm <path>        delete a file
 *   rmdir <path>     delete an empty directory
 *   mv <from> <to>   rename / move (into <to> if it is a directory)
 *   df               disk usage
 *
 * Relative paths are resolved against the shell's current directory.
 */
#include "ofs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern void terminal_print_string(const char* str);
extern void terminal_print_dec(uint32_t val);

static bool eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int split(char* s, char** argv, int max) {
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ') *s++ = '\0';
        if (!*s) break;
        argv[argc++] = s;
        while (*s && *s != ' ') s++;
    }
    return argc;
}

static void pr(const char* s) { terminal_print_string(s); }

static bool resolve(const char* in, char* out) {
    if (!ofs_abspath(in, out, 128)) { pr("path too long or too deep\n"); return false; }
    return true;
}

bool fs_shell_command(const char* line) {
    char buf[128];
    size_t n = 0;
    while (line[n] && n < sizeof(buf) - 1) { buf[n] = line[n]; n++; }
    buf[n] = '\0';
    char* argv[4];
    int argc = split(buf, argv, 4);
    if (argc == 0) return false;

    bool is_rm = eq(argv[0], "rm"), is_rmdir = eq(argv[0], "rmdir");
    if (is_rm || is_rmdir) {
        if (!ofs_available()) { pr("no filesystem\n"); return true; }
        if (argc != 2 || argv[1][0] == '-') { pr(is_rm ? "usage: rm <file>\n" : "usage: rmdir <dir>\n"); return true; }
        char path[128];
        if (!resolve(argv[1], path)) return true;
        ofs_stat_t st;
        if (!ofs_stat(path, &st)) { pr(argv[1]); pr(": no such file or directory\n"); return true; }
        if (is_rm && st.type == OFS_TYPE_DIR) { pr(argv[1]); pr(": is a directory (use rmdir)\n"); return true; }
        if (is_rmdir && st.type != OFS_TYPE_DIR) { pr(argv[1]); pr(": not a directory\n"); return true; }
        if (!ofs_remove(path)) { pr("cannot remove "); pr(argv[1]); pr(" (directory not empty, or it is your current directory)\n"); }
        return true;
    }

    if (eq(argv[0], "mv")) {
        if (!ofs_available()) { pr("no filesystem\n"); return true; }
        if (argc != 3) { pr("usage: mv <from> <to>\n"); return true; }
        char a[128], b[128];
        if (!resolve(argv[1], a) || !resolve(argv[2], b)) return true;
        if (!ofs_rename(a, b)) pr("mv: failed (source missing, target exists, or bad destination)\n");
        return true;
    }

    if (eq(argv[0], "df") && argc == 1) {
        if (!ofs_available()) { pr("no filesystem\n"); return true; }
        ofs_usage_t u;
        ofs_usage(&u);
        pr("OFS v2   data blocks: "); terminal_print_dec(u.total_blocks);
        pr(" total, "); terminal_print_dec(u.used_blocks); pr(" used ("); terminal_print_dec(u.used_blocks / 2);
        pr(" KB), "); terminal_print_dec(u.total_blocks - u.used_blocks); pr(" free ("); terminal_print_dec((u.total_blocks - u.used_blocks) / 2);
        pr(" KB)\n         files: "); terminal_print_dec(u.files);
        pr("  dirs: "); terminal_print_dec(u.dirs);
        pr("  inodes free: "); terminal_print_dec(u.inodes_free); pr("/"); terminal_print_dec(u.inodes_total); pr("\n");
        return true;
    }
    return false;
}
