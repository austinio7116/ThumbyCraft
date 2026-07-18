/*
 * ThumbyCraft — TinyUSB configuration for the 2-player USB link.
 *
 * Settings mirror the Mote OS / ThumbyOne PROVEN RP2350 TinyUSB setup
 * (same SDK, same board) — notably OPT_MCU_RP2040 and the mandatory
 * RHPORT0_MODE. CDC class only, both device and host roles: the link
 * flips the one native controller into HOST role to enumerate the peer
 * unit as a CDC device (craft_link_usb.c). RHPORT0_MODE stays DEVICE —
 * the host role is entered with an explicit tuh_init(0), exactly like
 * engine_link does.
 */
#ifndef CRAFT_TUSB_CONFIG_H
#define CRAFT_TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          0

/* CRITICAL: without CFG_TUSB_RHPORT0_MODE, tusb_init() succeeds but never
 * calls tud_init() — the device silently doesn't enumerate. */
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

/* Class enables — CDC only. */
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

/* CDC FIFO + endpoint buffer sizes. 128-byte FIFOs (Mote uses 256):
 * link traffic is a few hundred bytes per frame at most, and in the
 * ThumbyOne slot every byte of BSS counts. craft_net paces its sends
 * to whatever the FIFO accepts, so smaller just means more calls. */
#define CFG_TUD_CDC_RX_BUFSIZE  128
#define CFG_TUD_CDC_TX_BUFSIZE  128
#define CFG_TUD_CDC_EP_BUFSIZE  64

/* Host role — CDC host class only, no hub, a single peer. The
 * enumeration buffer only ever holds the PEER's descriptor set
 * (device + config + strings, well under 128 bytes each). */
#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 128
#define CFG_TUH_DEVICE_MAX          1
#define CFG_TUH_HUB                 0
#define CFG_TUH_CDC                 1
#define CFG_TUH_CDC_RX_BUFSIZE      64
#define CFG_TUH_CDC_TX_BUFSIZE      64

#endif /* CRAFT_TUSB_CONFIG_H */
