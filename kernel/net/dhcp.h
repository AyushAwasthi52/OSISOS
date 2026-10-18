#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

typedef enum {
    DHCP_INIT = 0,       /* about to broadcast DISCOVER                    */
    DHCP_SELECTING,      /* DISCOVER sent, waiting for an OFFER            */
    DHCP_REQUESTING,     /* REQUEST sent, waiting for ACK                  */
    DHCP_BOUND,          /* have a lease                                   */
    DHCP_RENEWING,       /* past T1: asking the server to extend the lease */
    DHCP_FALLBACK,       /* no server answered: using the static fallback  */
    DHCP_STATIC          /* operator configured the address by hand        */
} dhcp_state_t;

void         dhcp_init(void);                 /* start acquiring a lease          */
void         dhcp_poll(void);                 /* drive the state machine (non-blocking) */
void         dhcp_renew_now(void);            /* renew immediately / restart      */
void         dhcp_set_static(void);           /* stop DHCP (after manual ifconfig) */
dhcp_state_t dhcp_state(void);
const char*  dhcp_state_name(void);
uint32_t     dhcp_lease_left_secs(void);      /* 0 if not bound */
uint32_t     dhcp_server(void);

#endif
