#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

/*
 * Kernel log: a 4 KB ring buffer of timestamped lines, readable with `dmesg`.
 * Safe to call from any task (it briefly disables interrupts).
 *
 *     klog("dhcp: lease %I from %I", ip, server);
 *
 * Supported formats:  %s string   %d signed   %u unsigned   %I IPv4 (host order)   %%
 */
void klog(const char* fmt, ...);

/* Copy the last `max_lines` lines into `out` (NUL-terminated); returns the length. */
uint32_t klog_read(char* out, uint32_t out_size, int max_lines);

#endif
