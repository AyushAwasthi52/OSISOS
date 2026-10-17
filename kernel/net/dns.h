#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/*
 * Blocking DNS resolver (A records, UDP, port 53).
 * The server is netif.dns (set by DHCP or the `dns <ip>` shell command).
 *
 *     uint32_t ip;
 *     if (dns_resolve("example.com", &ip, 6 * NET_HZ) == DNS_OK) { ... }
 *
 * Results are cached for min(TTL, 5 minutes). Do not call from the network task
 * (it waits for that task to deliver the reply).
 */

#define DNS_OK         0
#define DNS_ERR       (-1)      /* bad name, no DNS server configured, no free socket */
#define DNS_TIMEOUT   (-2)
#define DNS_NXDOMAIN  (-3)      /* the name does not exist */
#define DNS_FAIL      (-4)      /* server error / no usable A record in the answer */

int dns_resolve(const char* name, uint32_t* ip, uint32_t timeout_ticks);

#endif
