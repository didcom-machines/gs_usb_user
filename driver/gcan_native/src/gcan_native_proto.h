/* gcan_native_proto.h - wire protocol constants for the GCAN/ECAN/Rexon
 * USBCANI-V503, reverse-engineered from USB captures of the vendor's own
 * libECanVci.so talking to real hardware (see ../../README.md's "Origin"
 * section -- this project didn't do that capture work itself, it's built
 * from an existing reverse-engineering writeup).
 *
 * Only the CAN1 @ 500 kbit/s classic-CAN init sequence and the TX/RX frame
 * formats below are confirmed against hardware. There is no known command
 * yet for other channels, other bitrates, or CAN-FD (this device doesn't
 * support CAN-FD at all) -- gcan_native_configure() in gcan_native.c
 * refuses anything outside what's captured here rather than guessing.
 */
#ifndef GCAN_NATIVE_PROTO_H
#define GCAN_NATIVE_PROTO_H

#include <stdint.h>

#define GCAN_NATIVE_VID 0x0c66
#define GCAN_NATIVE_PID 0x000c

#define GCAN_NATIVE_IFACE  0
#define GCAN_NATIVE_EP_OUT 0x02
#define GCAN_NATIVE_EP_IN  0x82

#define GCAN_NATIVE_CMD_LEN 14
#define GCAN_NATIVE_REC_LEN 16

/* Fixed control packets for the only known-working configuration (CAN1,
 * 500 kbit/s, classic CAN). Sent in this exact order: POLL x4 (each
 * followed by draining a couple of reads), INIT, CFG, then START once the
 * channel is actually started. CLOSE is sent on gcan_native_close(). */
static const uint8_t GCAN_NATIVE_POLL_CMD[GCAN_NATIVE_CMD_LEN] = {
	0x81, 0x86, 0x00, 0x00, 0x40, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GCAN_NATIVE_INIT_CMD[GCAN_NATIVE_CMD_LEN] = {
	0x81, 0x0C, 0x00, 0x00, 0x40, 0x08, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
};
static const uint8_t GCAN_NATIVE_CFG_CMD[GCAN_NATIVE_CMD_LEN] = {
	0x81, 0x01, 0x00, 0x00, 0x40, 0x08, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GCAN_NATIVE_START_CMD[GCAN_NATIVE_CMD_LEN] = {
	0x81, 0x0F, 0x00, 0x00, 0x40, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t GCAN_NATIVE_CLOSE_CMD[GCAN_NATIVE_CMD_LEN] = {
	0x81, 0xA0, 0x00, 0x00, 0x40, 0x08, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};

/* TX packet (14 bytes): byte0 = opcode (CAN1 TX), bytes1-4 = CAN id,
 * little-endian, with bit 0x20000000 set for a 29-bit extended id, byte5 =
 * DLC, bytes6-13 = data, zero-padded to 8 bytes. */
#define GCAN_NATIVE_TX_OPCODE_CAN1 0x21

/* Extended-id flag as observed ON THE WIRE, both for TX id encoding and
 * for decoding the id field of an RX record -- NOT the same bit as the
 * public API's GCAN_NATIVE_EFF_FLAG (../include/gcan_native.h uses
 * gsusb_frame's bit-31 convention for API-shape parity; the wire format
 * this hardware actually speaks uses bit 29). gcan_native.c translates
 * between the two. GCAN_NATIVE_EFF_MASK/SFF_MASK happen to have the same
 * value in both layouts, so gcan_native.h's copies are reused as-is.
 */
#define GCAN_NATIVE_WIRE_EFF_FLAG 0x20000000U

/* RX record (16 bytes): bytes0-3 = encoded CAN id, little-endian, byte4 =
 * DLC, bytes5-12 = data (8 bytes, always present, padded), bytes13-15 =
 * timestamp/status-like bytes -- not decoded, meaning unknown. A single
 * USB IN read can carry several of these back to back. */

#endif /* GCAN_NATIVE_PROTO_H */
