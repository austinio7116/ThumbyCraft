/*
 * ThumbyCraft — 2-player link transport (byte pipe).
 *
 * Two units + one USB-C cable. Both run the same discovery: flip
 * randomly between USB DEVICE and USB HOST roles until one side (as
 * host) enumerates the other (as a CDC device) — the TinyCircuits
 * engine_link scheme, proven on this exact hardware (and battle-tested
 * again in the Mote OS, which this transport is lifted from). Once
 * linked it is a raw reliable in-order byte pipe; craft_net.c frames
 * messages on top.
 *
 * Host build: same API over a Unix stream socket so two emulator
 * instances can pair (host/craft_link_socket.c). ThumbyOne slot mode:
 * compiled out — the lobby owns USB there (stubs return LINK_OFF).
 */
#ifndef CRAFT_LINK_H
#define CRAFT_LINK_H

#define CRAFT_LINK_OFF        0
#define CRAFT_LINK_SEARCHING  1
#define CRAFT_LINK_CONNECTED  2

void craft_link_start(void);
void craft_link_stop(void);
void craft_link_task(void);          /* pump discovery/transfer; cheap when off */
int  craft_link_status(void);        /* CRAFT_LINK_OFF/SEARCHING/CONNECTED */
int  craft_link_send(const void *data, int len);   /* returns bytes accepted */
int  craft_link_recv(void *buf, int max);          /* returns bytes read */
int  craft_link_active(void);        /* started? */
int  craft_link_role_host(void);     /* diagnostic: current USB role (1=host) */

#endif /* CRAFT_LINK_H */
