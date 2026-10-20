#ifndef HTTPD_H
#define HTTPD_H

#include <stdint.h>
#include <stdbool.h>

/*
 * OSISOS httpd - a small static-file HTTP/1.1 server (port 80).
 *
 *   GET / HEAD       files and directories from the OFS document root (default /www)
 *   /_status         plain-text server statistics
 *   no /www/index.html -> a built-in welcome page is served at "/"
 *
 * Every response closes the connection ("Connection: close"); files are streamed
 * from disk in small chunks, so file size is limited by OFS, not by RAM.
 */

#define HTTPD_LOG_SIZE  16

typedef struct {
    uint32_t ip;
    uint32_t when;                 /* tick */
    uint16_t status;
    uint32_t bytes;                /* body bytes sent */
    char     method[8];
    char     path[40];
} httpd_log_t;

typedef struct {
    uint32_t requests, bytes_sent, active;
    uint32_t s2xx, s3xx, s4xx, s5xx;
} httpd_stats_t;

void        httpd_init(void);
void        httpd_poll(void);                       /* call from the network task */
void        httpd_set_root(const char* ofs_dir);
const char* httpd_root(void);
void        httpd_get_stats(httpd_stats_t* out);
bool        httpd_get_log(int n_from_newest, httpd_log_t* out);

#endif
