/* gsusb_proto.h - wire protocol of the Linux gs_usb kernel driver
 * (drivers/net/can/usb/gs_usb.c), reimplemented for userspace.
 *
 * All multi-byte control-transfer fields are little-endian on the wire
 * (candleLight firmware always uses LE; the original GS USB firmware can be
 * told to use the host's byte order via GS_USB_BREQ_HOST_FORMAT, but every
 * real-world Linux host is little-endian, which is what we send). Frame
 * buffers are built/parsed by hand with explicit little-endian helpers below
 * instead of relying on compiler struct layout, since this is wire data.
 */
#ifndef GSUSB_PROTO_H
#define GSUSB_PROTO_H

#include <stdint.h>

/* USB control request codes (bRequest), enum gs_usb_breq */
enum {
	GS_USB_BREQ_HOST_FORMAT = 0,
	GS_USB_BREQ_BITTIMING,
	GS_USB_BREQ_MODE,
	GS_USB_BREQ_BERR,
	GS_USB_BREQ_BT_CONST,
	GS_USB_BREQ_DEVICE_CONFIG,
	GS_USB_BREQ_TIMESTAMP,
	GS_USB_BREQ_IDENTIFY,
	GS_USB_BREQ_GET_USER_ID,
	GS_USB_BREQ_SET_USER_ID,
	GS_USB_BREQ_DATA_BITTIMING,
	GS_USB_BREQ_BT_CONST_EXT,
	GS_USB_BREQ_SET_TERMINATION,
	GS_USB_BREQ_GET_TERMINATION,
	GS_USB_BREQ_GET_STATE,
};

enum {
	GS_CAN_MODE_RESET = 0,
	GS_CAN_MODE_START = 1,
};

enum {
	GS_CAN_IDENTIFY_OFF = 0,
	GS_CAN_IDENTIFY_ON = 1,
};

enum {
	GS_CAN_TERMINATION_STATE_OFF = 0,
	GS_CAN_TERMINATION_STATE_ON = 1,
};
#define GS_USB_TERMINATION_ENABLED 120

/* device_mode / start flags, enum bits in struct gs_device_mode::flags */
#define GS_CAN_MODE_LISTEN_ONLY             (1u << 0)
#define GS_CAN_MODE_LOOP_BACK               (1u << 1)
#define GS_CAN_MODE_TRIPLE_SAMPLE           (1u << 2)
#define GS_CAN_MODE_ONE_SHOT                (1u << 3)
#define GS_CAN_MODE_HW_TIMESTAMP            (1u << 4)
#define GS_CAN_MODE_PAD_PKTS_TO_MAX_PKT_SIZE (1u << 7)
#define GS_CAN_MODE_FD                       (1u << 8)
#define GS_CAN_MODE_BERR_REPORTING          (1u << 12)

/* feature bits reported by GS_USB_BREQ_BT_CONST, enum gs_can_feature */
#define GS_CAN_FEATURE_LISTEN_ONLY          (1u << 0)
#define GS_CAN_FEATURE_LOOP_BACK            (1u << 1)
#define GS_CAN_FEATURE_TRIPLE_SAMPLE        (1u << 2)
#define GS_CAN_FEATURE_ONE_SHOT             (1u << 3)
#define GS_CAN_FEATURE_HW_TIMESTAMP         (1u << 4)
#define GS_CAN_FEATURE_IDENTIFY             (1u << 5)
#define GS_CAN_FEATURE_USER_ID              (1u << 6)
#define GS_CAN_FEATURE_PAD_PKTS_TO_MAX_PKT_SIZE (1u << 7)
#define GS_CAN_FEATURE_FD                   (1u << 8)
#define GS_CAN_FEATURE_REQ_USB_QUIRK_LPC546XX (1u << 9)
#define GS_CAN_FEATURE_BT_CONST_EXT         (1u << 10)
#define GS_CAN_FEATURE_TERMINATION          (1u << 11)
#define GS_CAN_FEATURE_BERR_REPORTING       (1u << 12)
#define GS_CAN_FEATURE_GET_STATE            (1u << 13)
#define GS_CAN_FEATURE_MASK                 0x3FFFu

/* per-frame flags, struct gs_host_frame::flags */
#define GS_CAN_FLAG_OVERFLOW (1u << 0)
#define GS_CAN_FLAG_FD       (1u << 1)
#define GS_CAN_FLAG_BRS      (1u << 2)
#define GS_CAN_FLAG_ESI      (1u << 3)

/* struct gs_host_frame::echo_id value meaning "this is a real received
 * frame", not an echo/ack of one of our own tx frames */
#define GS_HOST_FRAME_ECHO_ID_RX 0xffffffffu

/* --- wire byte layout sizes (all little-endian, hand packed) ---
 *
 * struct gs_host_config { __le32 byte_order; }
 */
#define GS_HOST_CONFIG_SIZE 4

/* struct gs_device_config {
 *   u8 reserved1, reserved2, reserved3, icount;
 *   __le32 sw_version;
 *   __le32 hw_version;
 * }
 */
#define GS_DEVICE_CONFIG_SIZE 12

/* struct gs_device_mode { __le32 mode; __le32 flags; } */
#define GS_DEVICE_MODE_SIZE 8

/* struct gs_device_state { __le32 state, rxerr, txerr; } */
#define GS_DEVICE_STATE_SIZE 12

/* struct gs_device_bittiming {
 *   __le32 prop_seg, phase_seg1, phase_seg2, sjw, brp;
 * }
 */
#define GS_DEVICE_BITTIMING_SIZE 20

/* struct gs_identify_mode { __le32 mode; } */
#define GS_IDENTIFY_MODE_SIZE 4

/* struct gs_device_termination_state { __le32 state; } */
#define GS_TERMINATION_STATE_SIZE 4

/* struct gs_device_bt_const {
 *   __le32 feature, fclk_can, tseg1_min, tseg1_max, tseg2_min, tseg2_max,
 *          sjw_max, brp_min, brp_max, brp_inc;
 * } -- 10 * 4 bytes
 */
#define GS_DEVICE_BT_CONST_SIZE 40

/* struct gs_device_bt_const_extended: bt_const fields followed by
 * dtseg1_min, dtseg1_max, dtseg2_min, dtseg2_max, dsjw_max, dbrp_min,
 * dbrp_max, dbrp_inc -- 18 * 4 bytes
 */
#define GS_DEVICE_BT_CONST_EXT_SIZE 72

/* struct gs_host_frame header: u32 echo_id; __le32 can_id; u8 can_dlc,
 * channel, flags, reserved; -- treated as all-LE, see file comment. */
#define GS_HOST_FRAME_HDR_SIZE 12
#define GS_HOST_FRAME_TS_SIZE 4 /* trailing timestamp_us, when present */

#define GS_CLASSIC_DLEN 8
#define GS_FD_DLEN 64

static inline unsigned int gs_frame_size(unsigned int data_len, int with_ts)
{
	return GS_HOST_FRAME_HDR_SIZE + data_len + (with_ts ? GS_HOST_FRAME_TS_SIZE : 0);
}

/* --- little-endian pack/unpack helpers (portable, no struct-layout games) */

static inline void gs_put_u8(uint8_t *p, uint8_t v) { p[0] = v; }
static inline uint8_t gs_get_u8(const uint8_t *p) { return p[0]; }

static inline void gs_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v);
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t gs_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* GSUSB_PROTO_H */
