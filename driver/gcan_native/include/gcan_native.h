/* gcan_native.h - public API for a from-scratch, libusb-1.0-only userspace
 * driver for the GCAN/ECAN/Rexon USBCANI-V503 USB-CAN adapter.
 *
 * This API is deliberately shaped to match driver/include/gsusb.h as
 * closely as this hardware's actually-confirmed capabilities allow --
 * same init/open split, same channel-handle model, same function and
 * error-code naming -- so porting code between the two drivers is close
 * to a search-and-replace rather than a rewrite. Where a gsusb capability
 * has no reverse-engineered equivalent on this protocol yet (channel
 * state, identify, termination, raw bit-timing, a second channel), the
 * matching entry point still exists here but returns
 * GCAN_NATIVE_ERR_INVALID rather than being silently absent -- see
 * ../README.md for exactly what's confirmed vs. stubbed, and what it
 * would take to un-stub each one.
 *
 * The one deliberate exception is CAN-FD: gsusb_frame has a flags byte
 * for FD/BRS/ESI because gs_usb hardware can have those features. This
 * hardware confirmed does not support CAN-FD at all (not "not ported
 * yet"), so gcan_native_frame has no such field -- adding one would
 * misrepresent a hardware fact as a temporary gap.
 *
 * Unlike driver/gcan/'s vendor libECanVci.so.1, this talks to the
 * device's USB protocol directly, using only libusb-1.0 -- no vendor
 * blob, so it cross-compiles and runs natively on any architecture
 * libusb-1.0 supports, including armv7 (e.g. the Teltonika RUTX11).
 *
 * The protocol itself was reverse-engineered from USB captures against
 * real hardware (see ../README.md's "Origin" section) -- only channel 0
 * @ 500 kbit/s classic CAN has a confirmed-working init sequence so far.
 */
#ifndef GCAN_NATIVE_H
#define GCAN_NATIVE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCAN_NATIVE_VID 0x0c66
#define GCAN_NATIVE_PID 0x000c

/* CAN id flags/masks -- identical bit layout to Linux <linux/can.h> and to
 * gsusb_frame's can_id, for API-shape parity. GCAN_NATIVE_RTR_FLAG and
 * GCAN_NATIVE_ERR_FLAG are defined (so calling code doesn't need
 * different id-flag constants per driver) but not confirmed supported by
 * this protocol yet -- gcan_native_channel_send() rejects a frame with
 * either bit set with GCAN_NATIVE_ERR_INVALID rather than silently
 * ignoring them, and gcan_native_channel_recv() never sets either. */
#define GCAN_NATIVE_EFF_FLAG 0x80000000U /* extended (29-bit) frame */
#define GCAN_NATIVE_RTR_FLAG 0x40000000U /* remote transmission request -- not confirmed supported */
#define GCAN_NATIVE_ERR_FLAG 0x20000000U /* error frame -- not confirmed supported (no error reporting at all yet) */
#define GCAN_NATIVE_EFF_MASK 0x1FFFFFFFU
#define GCAN_NATIVE_SFF_MASK 0x000007FFU

#define GCAN_NATIVE_MAX_DLEN 8 /* classic CAN only -- this device has no CAN-FD, see this header's module doc */

typedef struct {
	uint32_t can_id;   /* id plus GCAN_NATIVE_*_FLAG bits, see above */
	uint8_t  len;      /* data length: 0-8 */
	uint8_t  data[GCAN_NATIVE_MAX_DLEN];
	uint32_t timestamp; /* device hw tick counter -- always 0 for now, see
			      * ../README.md: the RX record's trailing bytes
			      * that presumably carry this aren't decoded yet */
} gcan_native_frame;

/* channel/device feature bits -- mirrors gsusb_feature's shape so
 * gcan_native_channel_features() can be checked the same way. Every bit
 * is 0 today: none of these are confirmed supported on this protocol
 * yet (see ../README.md). */
enum gcan_native_feature {
	GCAN_NATIVE_FEATURE_LISTEN_ONLY    = 1u << 0,
	GCAN_NATIVE_FEATURE_LOOP_BACK      = 1u << 1,
	GCAN_NATIVE_FEATURE_ONE_SHOT       = 1u << 2,
	GCAN_NATIVE_FEATURE_HW_TIMESTAMP   = 1u << 3,
	GCAN_NATIVE_FEATURE_IDENTIFY       = 1u << 4,
	GCAN_NATIVE_FEATURE_TERMINATION    = 1u << 5,
	GCAN_NATIVE_FEATURE_GET_STATE      = 1u << 6,
	GCAN_NATIVE_FEATURE_SECOND_CHANNEL = 1u << 7,
};

/* mode flags for gcan_native_channel_start() -- mirrors gsusb_mode_flags's
 * shape. Only mode_flags=0 (normal) is confirmed working right now; any
 * bit set returns GCAN_NATIVE_ERR_INVALID rather than a silently ignored
 * request. */
enum gcan_native_mode_flags {
	GCAN_NATIVE_MODE_LISTEN_ONLY = 1u << 0,
	GCAN_NATIVE_MODE_LOOPBACK    = 1u << 1,
	GCAN_NATIVE_MODE_ONE_SHOT    = 1u << 2,
};

/* device state, mirrors gsusb_can_state's shape -- not populated by
 * anything yet, since gcan_native_channel_get_state() is a stub (see
 * below); kept here so calling code can share one set of constants. */
enum gcan_native_can_state {
	GCAN_NATIVE_STATE_ERROR_ACTIVE = 0,
	GCAN_NATIVE_STATE_ERROR_WARNING,
	GCAN_NATIVE_STATE_ERROR_PASSIVE,
	GCAN_NATIVE_STATE_BUS_OFF,
	GCAN_NATIVE_STATE_STOPPED,
};

typedef struct {
	uint8_t channel_count; /* what this *driver* currently supports (1),
				 * not necessarily this hardware's true
				 * channel count -- "USBCANI" vs. "PRO II"
				 * naming suggests a second physical channel
				 * may exist, but it has no captured protocol
				 * yet, see ../README.md */
} gcan_native_device_config;

typedef struct gcan_native_dev gcan_native_dev;         /* opaque: whole USB device */
typedef struct gcan_native_channel gcan_native_channel; /* opaque: one CAN channel on it */

/* --- discovery / lifecycle --- */

/* Initializes libusb. *ctx_out receives the libusb context to pass to
 * gcan_native_open()/gcan_native_exit(). Returns 0 on success, <0 on
 * error. */
int gcan_native_init(void **ctx_out);
void gcan_native_exit(void *ctx);

/* Opens the first attached GCAN/ECAN/Rexon USBCANI-V503
 * (GCAN_NATIVE_VID:GCAN_NATIVE_PID). Unlike gsusb_open(), there's no
 * vid/pid/bus/addr to pass -- this driver only knows one adapter model,
 * and has no way yet to select among several identical ones if more than
 * one is attached (it opens whichever libusb enumerates first). Returns
 * NULL on error and writes a message into errbuf (if non-NULL). */
gcan_native_dev *gcan_native_open(void *ctx, char *errbuf, size_t errbuf_len);
void gcan_native_close(gcan_native_dev *dev);

unsigned int gcan_native_channel_count(const gcan_native_dev *dev);
const gcan_native_device_config *gcan_native_get_device_config(const gcan_native_dev *dev);

/* --- per channel --- */

/* Channel objects are owned by the gcan_native_dev; do not free them
 * yourself. Only channel=0 exists right now -- see gcan_native_channel_count(). */
gcan_native_channel *gcan_native_channel_get(gcan_native_dev *dev, unsigned int channel);

uint32_t gcan_native_channel_features(const gcan_native_channel *ch);

/* Runs the reverse-engineered init handshake. Must be called before
 * gcan_native_channel_start(). Only bitrate=500000 is confirmed-working
 * right now (see ../README.md) -- anything else returns
 * GCAN_NATIVE_ERR_INVALID. Unlike gsusb_channel_set_bitrate(), there's no
 * bit-timing calculator behind this (the init sequence is one fixed
 * captured byte string, not decomposed register values) -- sample_point
 * is accepted for signature parity but has no effect. */
int gcan_native_channel_set_bitrate(gcan_native_channel *ch, uint32_t bitrate, double sample_point);

/* Not supported -- no raw register-level timing has been reverse
 * engineered for this protocol (unlike the vendor ECanVci SDK's
 * Timing0/Timing1, this capture's init sequence is fully opaque bytes).
 * Always returns GCAN_NATIVE_ERR_INVALID. */
int gcan_native_channel_set_bittiming_raw(gcan_native_channel *ch, uint32_t prop_seg,
					  uint32_t phase_seg1, uint32_t phase_seg2,
					  uint32_t sjw, uint32_t brp);

/* Starts the channel (bus goes live). mode_flags must be 0 -- see
 * gcan_native_mode_flags's doc above. */
int gcan_native_channel_start(gcan_native_channel *ch, uint32_t mode_flags);
/* Marks the channel stopped on this side; there's no confirmed distinct
 * "stop CAN" wire command yet (see ../README.md), so this doesn't send
 * anything beyond what gcan_native_close() already sends on close. */
int gcan_native_channel_stop(gcan_native_channel *ch);

/* Not supported yet -- no status/error register readout has been
 * reverse-engineered on this protocol. Always returns
 * GCAN_NATIVE_ERR_INVALID. */
int gcan_native_channel_get_state(gcan_native_channel *ch, uint32_t *state, uint32_t *rxerr, uint32_t *txerr);
/* Not supported -- unknown whether/how this hardware exposes an identify/
 * blink command. Always returns GCAN_NATIVE_ERR_INVALID. */
int gcan_native_channel_set_identify(gcan_native_channel *ch, int on);
/* Not supported -- unknown whether this hardware has switchable
 * termination. Always returns GCAN_NATIVE_ERR_INVALID. */
int gcan_native_channel_get_termination(gcan_native_channel *ch, int *enabled_120ohm);
int gcan_native_channel_set_termination(gcan_native_channel *ch, int enable_120ohm);

/* Sends one frame; blocks until the USB OUT transfer completes. Rejects
 * (GCAN_NATIVE_ERR_INVALID) a frame with GCAN_NATIVE_RTR_FLAG or
 * GCAN_NATIVE_ERR_FLAG set, or len > GCAN_NATIVE_MAX_DLEN -- see this
 * header's module doc. 0 on success, <0 on error. */
int gcan_native_channel_send(gcan_native_channel *ch, const gcan_native_frame *frame, unsigned int timeout_ms);

/* Blocks up to timeout_ms for the next received CAN frame. A single USB
 * read can carry more than one frame record; extra ones are buffered
 * internally and returned on subsequent calls before issuing another USB
 * read. Returns 1 if a frame was written to *frame, 0 on timeout (or an
 * unrecognized record, discarded the same way the reverse-engineered CLI
 * tool this was built from did), <0 on error. */
int gcan_native_channel_recv(gcan_native_channel *ch, gcan_native_frame *frame, unsigned int timeout_ms);

/* Values and meanings match gsusb_error 1:1, for interchangeable
 * exception/error handling between the two drivers. */
enum gcan_native_error {
	GCAN_NATIVE_OK = 0,
	GCAN_NATIVE_ERR_IO = -1,
	GCAN_NATIVE_ERR_NOMEM = -2,
	GCAN_NATIVE_ERR_NOT_FOUND = -3,
	GCAN_NATIVE_ERR_ACCESS = -4,
	GCAN_NATIVE_ERR_TIMEOUT = -5,
	GCAN_NATIVE_ERR_INVALID = -6,
	GCAN_NATIVE_ERR_BUSY = -7,
	GCAN_NATIVE_ERR_NO_BITTIMING_SOLUTION = -8,
};

const char *gcan_native_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* GCAN_NATIVE_H */
