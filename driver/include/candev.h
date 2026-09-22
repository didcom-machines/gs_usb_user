/* candev.h - transparent CAN adapter access: auto-detects which protocol
 * the attached USB CAN adapter actually speaks and talks it, so calling
 * code does not need to know or care whether the device is a gs_usb
 * (candleLight-firmware etc., see gsusb.h) or SLCAN (see slcan.h) adapter.
 *
 * candev_open() tries gs_usb first (matched by known VID:PID, a strong,
 * reliable signal) and falls back to SLCAN (matched by probing serial
 * ports, a best-effort heuristic -- SLCAN has no standard way to identify
 * itself; see the doc comment on candev_open() below).
 *
 * The API surface here is the intersection of what both backends offer:
 * one classic-CAN (no FD) channel, the fixed 9-entry bitrate table shared
 * by both, and no access to gsusb-only features (multi-channel, FD,
 * termination, identify) or slcan-only diagnostics (raw error-line text).
 * Code that needs those talks to gsusb.h/slcan.h directly instead.
 */
#ifndef CANDEV_H
#define CANDEV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CANDEV_EFF_FLAG 0x80000000U
#define CANDEV_RTR_FLAG 0x40000000U
#define CANDEV_EFF_MASK 0x1FFFFFFFU
#define CANDEV_SFF_MASK 0x000007FFU
#define CANDEV_MAX_DLEN 8

typedef struct {
	uint32_t can_id; /* ID plus CANDEV_*_FLAG bits, same encoding as gsusb/slcan */
	uint8_t  len;    /* 0-8 */
	uint8_t  data[CANDEV_MAX_DLEN];
} candev_frame;

enum candev_backend {
	CANDEV_BACKEND_NONE = 0,
	CANDEV_BACKEND_GSUSB,
	CANDEV_BACKEND_SLCAN,
};

enum candev_state {
	CANDEV_STATE_ERROR_ACTIVE = 0,
	CANDEV_STATE_ERROR_WARNING,
	CANDEV_STATE_ERROR_PASSIVE,
	CANDEV_STATE_BUS_OFF,
	CANDEV_STATE_UNKNOWN, /* also covers gsusb's STOPPED/SLEEPING */
};

typedef struct candev candev;

/* --- discovery / lifecycle --- */

/* Auto-detects and opens the first CAN adapter found:
 *  1. Scans for a gs_usb-protocol device by known VID:PID (see gsusb.h's
 *     GSUSB_VID_.../GSUSB_PID_... list) via libusb. This is a reliable
 *     match: those ids are reserved for gs_usb-class firmware.
 *  2. If none is found, falls back to SLCAN: if slcan_hint is non-NULL, it
 *     is opened directly as the device path; otherwise every /dev/ttyACM*
 *     is tried in turn, each briefly probed with the SLCAN 'V' (version)
 *     command and accepted on any non-empty reply. This is a best-effort
 *     heuristic, not a real identification -- SLCAN has no standard way to
 *     self-identify over a plain serial port, so *any* device that answers
 *     a stray 'V\r' will be accepted. Pass an explicit slcan_hint whenever
 *     you know the path, or when another serial device on the system could
 *     produce a false positive.
 *
 * Returns NULL and writes a message into errbuf (if non-NULL) if neither
 * backend finds anything.
 */
candev *candev_open(const char *slcan_hint, char *errbuf, size_t errbuf_len);
void candev_close(candev *dev);

enum candev_backend candev_which_backend(const candev *dev);
const char *candev_backend_name(enum candev_backend backend);

/* --- control --- */

/* bitrate must be one of: 10000 20000 50000 100000 125000 250000 500000
 * 800000 1000000 (the table both backends share). Must be called before
 * candev_start(). */
int candev_set_bitrate(candev *dev, uint32_t bitrate);
int candev_start(candev *dev, int listen_only);
int candev_stop(candev *dev);

int candev_get_state(candev *dev, enum candev_state *state, uint32_t *rxerr, uint32_t *txerr);

/* --- data --- */

int candev_send(candev *dev, const candev_frame *frame, unsigned int timeout_ms);
int candev_recv(candev *dev, candev_frame *frame, unsigned int timeout_ms);

enum candev_error {
	CANDEV_OK = 0,
	CANDEV_ERR_IO = -1,
	CANDEV_ERR_NOMEM = -2,
	CANDEV_ERR_NOT_FOUND = -3,
	CANDEV_ERR_ACCESS = -4,
	CANDEV_ERR_TIMEOUT = -5,
	CANDEV_ERR_INVALID = -6,
	CANDEV_ERR_BUSY = -7,
};

const char *candev_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* CANDEV_H */
