/* slcan.h - userspace port of the Linux kernel's slcan line discipline
 * (drivers/net/can/slcan/slcan-core.c), for USB-CAN adapters that present
 * themselves as a virtual serial port (USB CDC-ACM, /dev/ttyACM*) running
 * SLCAN firmware -- a different wire protocol and transport from gsusb.h's
 * gs_usb devices (ASCII commands over a tty, not binary frames over
 * libusb bulk/control transfers).
 *
 * No special kernel module needed: the generic cdc_acm driver already
 * exposes these as /dev/ttyACM*, and is built into virtually every Linux
 * kernel (unlike gs_usb, which often is not). This library only needs
 * plain POSIX termios on that tty -- no libusb, no vendor driver, no
 * dependency on this project's gsusb.c at all.
 *
 * Classic CAN only (11/29-bit ids, 0-8 byte payloads): a faithful port of
 * the reference driver above, which does not implement CAN-FD, and does
 * not implement the 'Z' hardware-timestamp extension some SLCAN firmwares
 * support (the reference driver never sends 'Z' or parses a timestamp
 * suffix, so neither does this).
 */
#ifndef SLCAN_H
#define SLCAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CAN ID flags/masks - same bit layout as Linux <linux/can.h> and this
 * project's gsusb.h, for consistency across backends. */
#define SLCAN_EFF_FLAG 0x80000000U /* extended (29-bit) frame */
#define SLCAN_RTR_FLAG 0x40000000U /* remote transmission request */
#define SLCAN_EFF_MASK 0x1FFFFFFFU
#define SLCAN_SFF_MASK 0x000007FFU

#define SLCAN_MAX_DLEN 8 /* classic CAN only */

typedef struct {
	uint32_t can_id; /* id | SLCAN_EFF_FLAG | SLCAN_RTR_FLAG, see above */
	uint8_t  len;    /* 0-8 */
	uint8_t  data[SLCAN_MAX_DLEN];
} slcan_frame;

/* enum can_state, mirrored from the reference driver's slcan_bump_state():
 * only ever updated by frames the adapter itself chooses to send ('s'
 * lines), not polled on request -- there is no "get state" command in this
 * protocol. slcan_get_state() reports the most recently seen one. */
enum slcan_state {
	SLCAN_STATE_ERROR_ACTIVE = 0,
	SLCAN_STATE_ERROR_WARNING,
	SLCAN_STATE_ERROR_PASSIVE,
	SLCAN_STATE_BUS_OFF,
	SLCAN_STATE_UNKNOWN, /* no 's' line observed yet */
};

typedef struct slcan_dev slcan_dev; /* opaque */

/* --- lifecycle ---
 *
 * One slcan_dev is one serial device, one CAN channel (SLCAN adapters are
 * inherently single-channel, unlike gs_usb's multi-channel devices) --
 * there is no separate "channel" object to look up.
 */

/* Opens and raw-configures the tty at device_path (e.g. "/dev/ttyACM0").
 * Does not touch the CAN channel itself (no bitrate/open commands sent
 * yet - see slcan_set_bitrate()/slcan_start()). Returns NULL on error and
 * writes a message into errbuf (if non-NULL). */
slcan_dev *slcan_open(const char *device_path, char *errbuf, size_t errbuf_len);
void slcan_close(slcan_dev *dev);

/* Best-effort liveness probe used by candev.h's auto-detection: opens
 * device_path, sends 'V', and returns 1 if anything comes back within
 * timeout_ms, 0 otherwise. Not a real protocol identification -- see the
 * doc comment in slcan.c. */
int slcan_probe(const char *device_path, unsigned int timeout_ms);

/* Sets the nominal bitrate. Must be one of the reference driver's fixed
 * table: 10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000,
 * 1000000 (bps) -- SLCAN has no arbitrary bit-timing calculator, only this
 * 9-entry hardware table (slcan_bitrate_const[] upstream). Must be called
 * before slcan_start(). Returns SLCAN_ERR_INVALID for any other value. */
int slcan_set_bitrate(slcan_dev *dev, uint32_t bitrate);

/* Starts the channel (sends 'O' or, with listen_only, 'L') and spins up
 * the background reader thread that feeds slcan_recv(). */
int slcan_start(slcan_dev *dev, int listen_only);
/* Sends 'C' (close) and stops the reader thread. */
int slcan_stop(slcan_dev *dev);

/* Sends one frame; blocks until the write to the tty completes (does not
 * wait for any response from the adapter -- the reference driver doesn't
 * either). 0 on success, <0 on error. */
int slcan_send(slcan_dev *dev, const slcan_frame *frame, unsigned int timeout_ms);

/* Blocks up to timeout_ms for the next received CAN frame. Returns 1 if a
 * frame was written to *frame, 0 on timeout, <0 on error. */
int slcan_recv(slcan_dev *dev, slcan_frame *frame, unsigned int timeout_ms);

/* Most recently observed state + error counters from an 's' line (see
 * enum slcan_state above); *state is SLCAN_STATE_UNKNOWN and rxerr/txerr
 * are 0 if the adapter has not sent one yet. Always succeeds. */
void slcan_get_state(slcan_dev *dev, enum slcan_state *state, uint32_t *rxerr, uint32_t *txerr);

/* Best-effort RX diagnostics, all written only by the reader thread and
 * read without synchronization -- fine for diagnostics, not control flow.
 * Mirrors gsusb_rx_stats' spirit for this project's other backend. */
typedef struct {
	unsigned long rx_frames;   /* frames queued for the application */
	unsigned long queue_drops; /* queued frames overwritten because the
				    * application did not drain in time */
	unsigned long state_frames;/* 's' lines observed */
	unsigned long error_frames;/* 'e' lines observed (see slcan_get_last_error()) */
	unsigned long decode_errors; /* lines that didn't parse as any known type */
	unsigned long line_overflows;/* input line longer than the protocol
				      * allows, discarded (SLCAN_MTU) */
} slcan_rx_stats;

void slcan_get_rx_stats(slcan_dev *dev, slcan_rx_stats *out);

/* The most recent 'e' (bus error) line's error-code characters, e.g. "bcO"
 * -- see the SLCAN ENCAPSULATION FORMAT comment in the reference driver
 * for what each letter means (a=ACK, b=Bit0, B=Bit1, c=CRC, f=Form,
 * o=Rx overrun, O=Tx overrun, s=Stuff). This library does not decode
 * these into the kernel's CAN_ERR_* skb bit flags -- that encoding is
 * SocketCAN-specific and not meaningful outside it; the raw letters are
 * given as-is. Empty string if none seen yet. out must be at least 16
 * bytes; always succeeds and always NUL-terminates. */
void slcan_get_last_error(slcan_dev *dev, char *out, size_t out_len);

enum slcan_error {
	SLCAN_OK = 0,
	SLCAN_ERR_IO = -1,
	SLCAN_ERR_NOMEM = -2,
	SLCAN_ERR_NOT_FOUND = -3,
	SLCAN_ERR_ACCESS = -4,
	SLCAN_ERR_TIMEOUT = -5,
	SLCAN_ERR_INVALID = -6,
	SLCAN_ERR_BUSY = -7,
};

const char *slcan_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* SLCAN_H */
