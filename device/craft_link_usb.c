/*
 * ThumbyCraft — 2-player USB link, device side (see craft_link.h).
 *
 * Lifted from the Mote OS transport (mote_link.c), which itself mirrors
 * the TinyCircuits engine_link scheme proven on this exact hardware.
 *
 * Role flip discovery: while searching, alternate between USB device and
 * USB host on a randomised 600-1300 ms timer, so two identically-
 * programmed units eventually land on opposite roles and enumerate (the
 * random phase breaks the symmetry). The device role uses the CDC
 * descriptors below; the host role is TinyUSB's CDC host class.
 *
 * Data path: both roles land received bytes in one 512 B ring the game
 * drains with craft_link_recv(). Sends go straight to the active role's
 * CDC FIFO.
 *
 * ThumbyCraft has no other USB consumer while it is running (no
 * stdio_usb, no MSC — in ThumbyOne slot mode the lobby's MSC only runs
 * before the slot takes the chip), so unlike Mote there is no ownership
 * arbitration — the link simply owns the controller whenever started.
 */
#include "craft_link.h"

#include "tusb.h"
#include "class/cdc/cdc_host.h"
#include "class/cdc/cdc_device.h"
#include "pico/stdlib.h"

#include <string.h>

static int      s_started;
static int      s_is_host;
static int      s_connected;
static uint8_t  s_cdc_idx;        /* host role: mounted CDC interface index */
static int      s_cdc_mounted;
static uint32_t s_flip_at_ms;     /* next role flip while searching */
static uint32_t s_rng;
static int      s_usb_inited;

/* ---- CDC device-role descriptors ----------------------------------------
 * Minimal CDC-ACM composite, same shape as Mote's (which mirrors
 * ThumbyOne's proven RP2350 descriptors). Distinct PID so a PC that sees
 * a searching unit doesn't confuse it with a Mote/ThumbyOne device. */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCAFE,
    .idProduct          = 0x5443,   /* 'TC' — ThumbyCraft */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: language id — English */
    "TinyCircuits",                /* 1: manufacturer */
    "ThumbyCraft Link",            /* 2: product */
    "TCRAFT-LINK",                 /* 3: serial (fixed — peers don't care) */
    "ThumbyCraft CDC",             /* 4: CDC interface */
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *s = string_desc_arr[index];
        if (!s) return NULL;
        chr_count = (uint8_t)strlen(s);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = (uint16_t)s[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* ---- RX ring (both roles). Filled from tuh/tud task context (main loop,
 * core 0) and drained by the game — no cross-core access. 512 B is
 * PACING, not capacity: when it fills the CDC endpoint NAKs and the
 * sender's FIFO backpressures — bulk messages arrive intact, just
 * flow-controlled. */
static uint8_t  s_ring[512];
static uint16_t s_rhead, s_rtail;

static void ring_put(uint8_t b) {
    uint16_t next = (uint16_t)((s_rhead + 1) % sizeof s_ring);
    if (next == s_rtail) return;              /* full: drop newest */
    s_ring[s_rhead] = b;
    s_rhead = next;
}
static int ring_avail(void) {
    return (int)((s_rhead - s_rtail + sizeof s_ring) % sizeof s_ring);
}
static void ring_clear(void) { s_rhead = s_rtail = 0; }

/* ---- TinyUSB host-role callbacks ---------------------------------------- */
void tuh_mount_cb(uint8_t daddr)   { (void)daddr; }
void tuh_umount_cb(uint8_t daddr)  { (void)daddr; s_cdc_mounted = 0; }
void tuh_cdc_mount_cb(uint8_t idx)   {
    s_cdc_idx = idx; s_cdc_mounted = 1;
    /* Assert DTR+RTS. TinyUSB's cdc_device keeps its TX fifo OVERWRITABLE
     * until the host raises DTR — without this, the device-role peer's
     * tud_cdc_write silently overwrites queued bytes under pressure AND
     * returns the full count (one lossy direction, bulk transfers only —
     * exactly the world-transfer case). DTR flips the peer's fifo to
     * honest partial writes. Lesson inherited from Mote. */
    tuh_cdc_set_control_line_state(idx, 0x03, NULL, 0);
}
void tuh_cdc_umount_cb(uint8_t idx)  { (void)idx; s_cdc_mounted = 0; }
void tuh_cdc_rx_cb(uint8_t idx) {
    /* Read ONLY what the ring can hold: whatever stays in TinyUSB's rx
     * buffer backpressures the sender; the link task drains it as the
     * game frees ring space. An unconditional drain here would silently
     * drop bytes under burst (ring_put drops when full). */
    uint8_t b;
    while (ring_avail() < (int)sizeof s_ring - 1 && tuh_cdc_read(idx, &b, 1) == 1)
        ring_put(b);
}

/* ---- role switching ------------------------------------------------------ */
static void switch_to_device(void) {
    tuh_deinit(0);
    tud_init(0);
    tud_connect();
    s_is_host = 0;
    s_cdc_mounted = 0;
}
static void switch_to_host(void) {
    tud_deinit(0);
    tuh_init(0);
    s_is_host = 1;
    s_cdc_mounted = 0;
}

static uint32_t rng_next(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}
static void schedule_flip(uint32_t now_ms) {
    /* A window must comfortably outlast a full enumeration + CDC mount by
     * the peer (tens of ms when the task is pumped continuously), while
     * random lengths keep the two units from flipping in lockstep.
     * 600–1300 ms pairs within a few seconds. */
    s_flip_at_ms = now_ms + 600 + (rng_next() >> 20) % 700;
}

/* ---- public API ----------------------------------------------------------- */
int craft_link_active(void) { return s_started; }

void craft_link_start(void) {
    if (s_started) return;
    s_started = 1;
    s_connected = 0;
    ring_clear();
    s_rng = time_us_32() | 1;
    /* Start in the DEVICE role. ThumbyCraft never initialises USB
     * otherwise, so bring the stack up on first use. */
    if (!s_usb_inited) { tusb_init(); s_usb_inited = 1; }
    if (!tud_inited()) tud_init(0);
    tud_connect();
    s_is_host = 0;
    schedule_flip(to_ms_since_boot(get_absolute_time()));
}

void craft_link_stop(void) {
    if (!s_started) return;
    s_started = 0;
    s_connected = 0;
    if (s_is_host) switch_to_device();
    tud_disconnect();   /* nothing else uses USB — drop off the bus */
    ring_clear();
}

void craft_link_task(void) {
    if (!s_started) return;

    if (s_is_host) {
        tuh_task();
        /* drain what the rx callback left behind (ring was full then) */
        if (s_cdc_mounted) {
            uint8_t b;
            while (ring_avail() < (int)sizeof s_ring - 1 &&
                   tuh_cdc_read(s_cdc_idx, &b, 1) == 1) ring_put(b);
        }
        s_connected = s_cdc_mounted;
    } else {
        tud_task();
        while (tud_cdc_available() && ring_avail() < (int)sizeof s_ring - 1) {
            int c = tud_cdc_read_char();
            if (c < 0) break;
            ring_put((uint8_t)c);
        }
        s_connected = tud_ready();
    }
    if (s_connected) return;

    /* Searching: flip roles on the randomised deadline. */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - s_flip_at_ms) >= 0) {
        if (s_is_host) switch_to_device();
        else           switch_to_host();
        schedule_flip(now);
    }
}

int craft_link_status(void) {
    if (!s_started) return CRAFT_LINK_OFF;
    return s_connected ? CRAFT_LINK_CONNECTED : CRAFT_LINK_SEARCHING;
}

int craft_link_role_host(void) { return s_is_host; }

int craft_link_send(const void *data, int len) {
    if (!s_started || !s_connected || len <= 0) return 0;
    uint32_t n;
    if (s_is_host) {
        n = tuh_cdc_write(s_cdc_idx, data, (uint32_t)len);
        tuh_cdc_write_flush(s_cdc_idx);
    } else {
        /* Belt-and-braces for the DTR fix above: if the peer hasn't raised
         * DTR (mismatched firmware on the other unit), the TX fifo is
         * overwritable and tud_cdc_write LIES about full acceptance while
         * dropping queued bytes. Clamp to real space so the return is
         * always honest. */
        uint32_t room = tud_cdc_write_available();
        uint32_t l = (uint32_t)len; if (l > room) l = room;
        n = l ? tud_cdc_write(data, l) : 0;
        tud_cdc_write_flush();
    }
    return (int)n;
}

int craft_link_recv(void *buf, int max) {
    uint8_t *out = (uint8_t *)buf;
    int n = 0;
    while (n < max && s_rtail != s_rhead) {
        out[n++] = s_ring[s_rtail];
        s_rtail = (uint16_t)((s_rtail + 1) % sizeof s_ring);
    }
    return n;
}
