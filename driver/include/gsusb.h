/* gsusb.h - public API for the userspace port of the Linux gs_usb driver.
 *
 * This talks directly to Geschwister Schneider / candleLight-firmware USB-CAN
 * adapters over libusb-1.0, replicating the wire protocol used by
 * drivers/net/can/usb/gs_usb.c, without requiring the kernel module or
 * SocketCAN. One process may hold one device open at a time (libusb claims
 * the USB interface exclusively).
 */
#ifndef GSUSB_H
#define GSUSB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Well-known VID:PID pairs the upstream kernel driver binds to (all use USB
 * interface number 0). Passed to gsusb_open() or used by gsusb_info to scan.
 */
#define GSUSB_VID_GS_USB_1        0x1d50
#define GSUSB_PID_GS_USB_1        0x606f

#define GSUSB_VID_CANDLELIGHT     0x1209
#define GSUSB_PID_CANDLELIGHT     0x2323

#define GSUSB_VID_CES_CANEXT_FD   0x1cd2
#define GSUSB_PID_CES_CANEXT_FD   0x606f

#define GSUSB_VID_ABE_CANDEBUGGER 0x16d0
#define GSUSB_PID_ABE_CANDEBUGGER 0x10b8

#define GSUSB_VID_XYLANTA_SAINT3  0x16d0
#define GSUSB_PID_XYLANTA_SAINT3  0x0f30

#define GSUSB_VID_CANNECTIVITY    0x1209
#define GSUSB_PID_CANNECTIVITY    0xca01

/* CAN ID flags/masks - identical bit layout to Linux <linux/can.h>, since the
 * device firmware and the kernel driver share this encoding on the wire. */
#define GSUSB_EFF_FLAG 0x80000000U /* extended (29-bit) frame */
#define GSUSB_RTR_FLAG 0x40000000U /* remote transmission request */
#define GSUSB_ERR_FLAG 0x20000000U /* error frame */
#define GSUSB_EFF_MASK 0x1FFFFFFFU
#define GSUSB_SFF_MASK 0x000007FFU

#define GSUSB_MAX_DLEN 64

/* gsusb_frame::flags */
#define GSUSB_FRAME_FD  0x01u /* CAN-FD frame */
#define GSUSB_FRAME_BRS 0x02u /* bit-rate switch (FD data phase) */
#define GSUSB_FRAME_ESI 0x04u /* error state indicator (FD) */

typedef struct {
	uint32_t can_id;   /* ID plus GSUSB_*_FLAG bits, see above */
	uint8_t  len;      /* data length: 0-8 for classic CAN, 0-64 for FD */
	uint8_t  flags;    /* GSUSB_FRAME_* */
	uint8_t  data[GSUSB_MAX_DLEN];
	uint32_t timestamp_us; /* device hw timestamp; only meaningful for
				 * received frames on a channel that was
				 * started with hw timestamps enabled */
} gsusb_frame;

/* channel/device feature bits - mirrors GS_CAN_FEATURE_* in gs_usb.c */
enum gsusb_feature {
	GSUSB_FEATURE_LISTEN_ONLY        = 1u << 0,
	GSUSB_FEATURE_LOOP_BACK          = 1u << 1,
	GSUSB_FEATURE_TRIPLE_SAMPLE      = 1u << 2,
	GSUSB_FEATURE_ONE_SHOT           = 1u << 3,
	GSUSB_FEATURE_HW_TIMESTAMP       = 1u << 4,
	GSUSB_FEATURE_IDENTIFY           = 1u << 5,
	GSUSB_FEATURE_USER_ID            = 1u << 6,
	GSUSB_FEATURE_PAD_PKTS_TO_MAX    = 1u << 7,
	GSUSB_FEATURE_FD                 = 1u << 8,
	GSUSB_FEATURE_REQ_QUIRK_LPC546XX = 1u << 9,
	GSUSB_FEATURE_BT_CONST_EXT       = 1u << 10,
	GSUSB_FEATURE_TERMINATION        = 1u << 11,
	GSUSB_FEATURE_BERR_REPORTING     = 1u << 12,
	GSUSB_FEATURE_GET_STATE          = 1u << 13,
};

/* mode flags for gsusb_channel_start() - mirrors GS_CAN_MODE_* */
enum gsusb_mode_flags {
	GSUSB_MODE_LISTEN_ONLY    = 1u << 0,
	GSUSB_MODE_LOOPBACK       = 1u << 1,
	GSUSB_MODE_TRIPLE_SAMPLE  = 1u << 2,
	GSUSB_MODE_ONE_SHOT       = 1u << 3,
	GSUSB_MODE_FD             = 1u << 4, /* also send data bittiming first */
	GSUSB_MODE_BERR_REPORTING = 1u << 5,
};

/* device state, mirrors enum gs_can_state */
enum gsusb_can_state {
	GSUSB_STATE_ERROR_ACTIVE = 0,
	GSUSB_STATE_ERROR_WARNING,
	GSUSB_STATE_ERROR_PASSIVE,
	GSUSB_STATE_BUS_OFF,
	GSUSB_STATE_STOPPED,
	GSUSB_STATE_SLEEPING,
};

typedef struct {
	uint32_t feature;   /* raw GSUSB_FEATURE_* bitmask reported by device */
	uint32_t fclk_can;  /* CAN clock, Hz */
	uint32_t tseg1_min, tseg1_max; /* combined prop_seg+phase_seg1 range */
	uint32_t tseg2_min, tseg2_max; /* phase_seg2 range */
	uint32_t sjw_max;
	uint32_t brp_min, brp_max, brp_inc;
} gsusb_bittiming_const;

typedef struct {
	uint32_t sw_version;
	uint32_t hw_version;
	uint8_t  channel_count;
} gsusb_device_config;

typedef struct gsusb_dev gsusb_dev;         /* opaque: whole USB device */
typedef struct gsusb_channel gsusb_channel; /* opaque: one CAN channel on it */

/* --- discovery / lifecycle --- */

/* Initializes libusb. *ctx_out receives the libusb context to pass to
 * gsusb_open()/gsusb_exit(). Returns 0 on success, <0 on error. */
int gsusb_init(void **ctx_out);
void gsusb_exit(void *ctx);

/* Opens the first attached device matching vid:pid. If bus/addr are >= 0,
 * the match is further restricted to that USB bus number/device address
 * (see gsusb_info's listing). Fetches device config and every channel's
 * bit-timing constants up front, same as the kernel driver's probe().
 * Returns NULL on error and writes a message into errbuf (if non-NULL). */
gsusb_dev *gsusb_open(void *ctx, uint16_t vid, uint16_t pid, int bus, int addr,
		      char *errbuf, size_t errbuf_len);
void gsusb_close(gsusb_dev *dev);

unsigned int gsusb_channel_count(const gsusb_dev *dev);
const gsusb_device_config *gsusb_get_device_config(const gsusb_dev *dev);

/* --- per channel --- */

/* Channel objects are owned by the gsusb_dev; do not free them yourself. */
gsusb_channel *gsusb_channel_get(gsusb_dev *dev, unsigned int channel);

const gsusb_bittiming_const *gsusb_channel_bt_const(const gsusb_channel *ch);
/* NULL unless the channel advertises FD + BT_CONST_EXT (separate data-phase
 * timing constants); otherwise the data phase shares bt_const(). */
const gsusb_bittiming_const *gsusb_channel_data_bt_const(const gsusb_channel *ch);
uint32_t gsusb_channel_features(const gsusb_channel *ch);

/* Computes nominal bit timing for the given bitrate (bps) and desired sample
 * point (0.0-1.0; pass 0 for an automatic CiA-recommended sample point) and
 * sends it to the device. Must be called before gsusb_channel_start(). */
int gsusb_channel_set_bitrate(gsusb_channel *ch, uint32_t bitrate, double sample_point);
/* Same, for the CAN-FD data phase. Only meaningful with GSUSB_MODE_FD. */
int gsusb_channel_set_data_bitrate(gsusb_channel *ch, uint32_t bitrate, double sample_point);

/* Sends exact raw timing register values, bypassing the calculator. */
int gsusb_channel_set_bittiming_raw(gsusb_channel *ch, uint32_t prop_seg,
				    uint32_t phase_seg1, uint32_t phase_seg2,
				    uint32_t sjw, uint32_t brp);
int gsusb_channel_set_data_bittiming_raw(gsusb_channel *ch, uint32_t prop_seg,
					 uint32_t phase_seg1, uint32_t phase_seg2,
					 uint32_t sjw, uint32_t brp);

/* Starts the channel (bus goes live). If GSUSB_FEATURE_HW_TIMESTAMP is
 * supported it is enabled automatically. Spins up the shared background
 * reader thread on the parent device if this is the first channel started. */
int gsusb_channel_start(gsusb_channel *ch, uint32_t mode_flags);
/* Resets/stops the channel. If it was the last active channel on this
 * device, stops the reader thread. */
int gsusb_channel_stop(gsusb_channel *ch);

int gsusb_channel_get_state(gsusb_channel *ch, uint32_t *state, uint32_t *rxerr, uint32_t *txerr);
int gsusb_channel_set_identify(gsusb_channel *ch, int on);
int gsusb_channel_get_termination(gsusb_channel *ch, int *enabled_120ohm);
int gsusb_channel_set_termination(gsusb_channel *ch, int enable_120ohm);

/* Sends one frame; blocks until the USB OUT transfer completes (does not
 * wait for the device's tx-echo/ack, nor for the frame to actually go out
 * on the bus). 0 on success, <0 on error (e.g. GSUSB_ERR_TIMEOUT). */
int gsusb_channel_send(gsusb_channel *ch, const gsusb_frame *frame, unsigned int timeout_ms);

/* Blocks up to timeout_ms for the next received CAN frame on this channel
 * (the reader thread transparently discards the device's tx-echo/ack
 * frames). Returns 1 if a frame was written to *frame, 0 on timeout, <0 on
 * error. */
int gsusb_channel_recv(gsusb_channel *ch, gsusb_frame *frame, unsigned int timeout_ms);

enum gsusb_error {
	GSUSB_OK = 0,
	GSUSB_ERR_IO = -1,
	GSUSB_ERR_NOMEM = -2,
	GSUSB_ERR_NOT_FOUND = -3,
	GSUSB_ERR_ACCESS = -4,
	GSUSB_ERR_TIMEOUT = -5,
	GSUSB_ERR_INVALID = -6,
	GSUSB_ERR_BUSY = -7,
	GSUSB_ERR_NO_BITTIMING_SOLUTION = -8,
};

const char *gsusb_strerror(int err);

/* Given len (0-8) return the classic-CAN DLC (identity, capped at 8). */
uint8_t gsusb_len2dlc_cc(uint8_t len);
uint8_t gsusb_dlc2len_cc(uint8_t dlc);
/* CAN-FD length <-> DLC mapping per ISO 11898-1 (dlc 9..15 => 12,16,20,24,32,48,64). */
uint8_t gsusb_len2dlc_fd(uint8_t len);
uint8_t gsusb_dlc2len_fd(uint8_t dlc);

#ifdef __cplusplus
}
#endif

#endif /* GSUSB_H */
