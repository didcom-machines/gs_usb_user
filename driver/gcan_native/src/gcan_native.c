#include "../include/gcan_native.h"
#include "gcan_native_proto.h"

#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCAN_NATIVE_MAX_CHANNELS 1 /* only channel 0 has a captured protocol so far */

struct gcan_native_channel {
	struct gcan_native_dev *dev;
	unsigned int index;
	int configured;
	int started;

	/* One USB IN read can carry several 16-byte frame records; leftover
	 * ones (beyond the one returned by the current
	 * gcan_native_channel_recv() call) are buffered here and served
	 * before issuing another read. */
	uint8_t pending[64];
	int pending_len;
	int pending_off;
};

struct gcan_native_dev {
	libusb_device_handle *handle;
	gcan_native_device_config config;
	gcan_native_channel channels[GCAN_NATIVE_MAX_CHANNELS];
};

static void set_err(char *errbuf, size_t errbuf_len, const char *fmt, ...)
{
	if (!errbuf || errbuf_len == 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errbuf, errbuf_len, fmt, ap);
	va_end(ap);
}

int gcan_native_init(void **ctx_out)
{
	libusb_context *ctx = NULL;
	int rc = libusb_init(&ctx);
	if (rc != 0)
		return GCAN_NATIVE_ERR_IO;
	*ctx_out = ctx;
	return GCAN_NATIVE_OK;
}

void gcan_native_exit(void *ctx)
{
	if (ctx)
		libusb_exit((libusb_context *)ctx);
}

gcan_native_dev *gcan_native_open(void *ctx, char *errbuf, size_t errbuf_len)
{
	gcan_native_dev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		set_err(errbuf, errbuf_len, "out of memory");
		return NULL;
	}

	dev->handle = libusb_open_device_with_vid_pid((libusb_context *)ctx,
						      GCAN_NATIVE_VID, GCAN_NATIVE_PID);
	if (!dev->handle) {
		set_err(errbuf, errbuf_len, "no GCAN adapter found (%04x:%04x)",
			GCAN_NATIVE_VID, GCAN_NATIVE_PID);
		free(dev);
		return NULL;
	}

	libusb_set_auto_detach_kernel_driver(dev->handle, 1);
	if (libusb_kernel_driver_active(dev->handle, GCAN_NATIVE_IFACE) == 1)
		libusb_detach_kernel_driver(dev->handle, GCAN_NATIVE_IFACE);

	int rc = libusb_claim_interface(dev->handle, GCAN_NATIVE_IFACE);
	if (rc != 0) {
		set_err(errbuf, errbuf_len, "libusb_claim_interface: %s", libusb_error_name(rc));
		libusb_close(dev->handle);
		free(dev);
		return NULL;
	}

	dev->config.channel_count = GCAN_NATIVE_MAX_CHANNELS;
	for (unsigned int i = 0; i < GCAN_NATIVE_MAX_CHANNELS; i++) {
		dev->channels[i].dev = dev;
		dev->channels[i].index = i;
	}

	return dev;
}

void gcan_native_close(gcan_native_dev *dev)
{
	if (!dev)
		return;

	if (dev->handle) {
		uint8_t close_cmd[GCAN_NATIVE_CMD_LEN];
		int transferred = 0;
		memcpy(close_cmd, GCAN_NATIVE_CLOSE_CMD, sizeof(close_cmd));
		libusb_bulk_transfer(dev->handle, GCAN_NATIVE_EP_OUT, close_cmd,
				     sizeof(close_cmd), &transferred, 1000);

		libusb_release_interface(dev->handle, GCAN_NATIVE_IFACE);
		libusb_close(dev->handle);
	}
	free(dev);
}

unsigned int gcan_native_channel_count(const gcan_native_dev *dev)
{
	return dev ? dev->config.channel_count : 0;
}

const gcan_native_device_config *gcan_native_get_device_config(const gcan_native_dev *dev)
{
	return dev ? &dev->config : NULL;
}

gcan_native_channel *gcan_native_channel_get(gcan_native_dev *dev, unsigned int channel)
{
	if (!dev || channel >= GCAN_NATIVE_MAX_CHANNELS)
		return NULL;
	return &dev->channels[channel];
}

uint32_t gcan_native_channel_features(const gcan_native_channel *ch)
{
	(void)ch;
	return 0; /* nothing confirmed supported yet, see gcan_native_feature's doc */
}

static int write_cmd(gcan_native_dev *dev, const uint8_t *cmd)
{
	uint8_t buf[GCAN_NATIVE_CMD_LEN];
	int transferred = 0;
	memcpy(buf, cmd, sizeof(buf));
	int rc = libusb_bulk_transfer(dev->handle, GCAN_NATIVE_EP_OUT, buf, sizeof(buf),
				      &transferred, 1000);
	if (rc != 0 || transferred != (int)sizeof(buf))
		return GCAN_NATIVE_ERR_IO;
	return GCAN_NATIVE_OK;
}

static void drain(gcan_native_dev *dev, int loops, int timeout_ms)
{
	uint8_t buf[64];
	int transferred;
	for (int i = 0; i < loops; i++)
		libusb_bulk_transfer(dev->handle, GCAN_NATIVE_EP_IN, buf, sizeof(buf),
				     &transferred, timeout_ms);
}

int gcan_native_channel_set_bitrate(gcan_native_channel *ch, uint32_t bitrate, double sample_point)
{
	(void)sample_point; /* no bit-timing calculator behind this yet, see header doc */
	if (!ch || !ch->dev || !ch->dev->handle)
		return GCAN_NATIVE_ERR_INVALID;
	/* Only this exact configuration has a reverse-engineered, confirmed-
	 * working init sequence -- see gcan_native_proto.h's module doc. */
	if (bitrate != 500000)
		return GCAN_NATIVE_ERR_INVALID;

	gcan_native_dev *dev = ch->dev;

	for (int i = 0; i < 4; i++) {
		int rc = write_cmd(dev, GCAN_NATIVE_POLL_CMD);
		if (rc != GCAN_NATIVE_OK)
			return rc;
		drain(dev, 2, 20);
	}

	int rc = write_cmd(dev, GCAN_NATIVE_INIT_CMD);
	if (rc != GCAN_NATIVE_OK)
		return rc;
	drain(dev, 3, 20);

	rc = write_cmd(dev, GCAN_NATIVE_CFG_CMD);
	if (rc != GCAN_NATIVE_OK)
		return rc;
	drain(dev, 3, 20);

	ch->configured = 1;
	return GCAN_NATIVE_OK;
}

int gcan_native_channel_set_bittiming_raw(gcan_native_channel *ch, uint32_t prop_seg,
					  uint32_t phase_seg1, uint32_t phase_seg2,
					  uint32_t sjw, uint32_t brp)
{
	(void)ch; (void)prop_seg; (void)phase_seg1; (void)phase_seg2; (void)sjw; (void)brp;
	/* No raw register-level timing has been reverse engineered for this
	 * protocol -- see this function's doc in gcan_native.h. */
	return GCAN_NATIVE_ERR_INVALID;
}

int gcan_native_channel_start(gcan_native_channel *ch, uint32_t mode_flags)
{
	if (!ch || !ch->configured)
		return GCAN_NATIVE_ERR_INVALID;
	if (mode_flags != 0)
		return GCAN_NATIVE_ERR_INVALID; /* no mode is confirmed supported yet */
	if (ch->started)
		return GCAN_NATIVE_OK;

	int rc = write_cmd(ch->dev, GCAN_NATIVE_START_CMD);
	if (rc != GCAN_NATIVE_OK)
		return rc;
	drain(ch->dev, 10, 20);

	ch->started = 1;
	return GCAN_NATIVE_OK;
}

int gcan_native_channel_stop(gcan_native_channel *ch)
{
	if (!ch)
		return GCAN_NATIVE_ERR_INVALID;
	/* No confirmed wire command for "stop CAN" distinct from the CLOSE
	 * sequence gcan_native_close() already sends -- this just marks the
	 * channel stopped on our side. */
	ch->started = 0;
	return GCAN_NATIVE_OK;
}

int gcan_native_channel_get_state(gcan_native_channel *ch, uint32_t *state, uint32_t *rxerr, uint32_t *txerr)
{
	(void)ch; (void)state; (void)rxerr; (void)txerr;
	return GCAN_NATIVE_ERR_INVALID; /* no status/error register readout reverse engineered yet */
}

int gcan_native_channel_set_identify(gcan_native_channel *ch, int on)
{
	(void)ch; (void)on;
	return GCAN_NATIVE_ERR_INVALID; /* unknown whether this hardware has an identify/blink command */
}

int gcan_native_channel_get_termination(gcan_native_channel *ch, int *enabled_120ohm)
{
	(void)ch; (void)enabled_120ohm;
	return GCAN_NATIVE_ERR_INVALID; /* unknown whether this hardware has switchable termination */
}

int gcan_native_channel_set_termination(gcan_native_channel *ch, int enable_120ohm)
{
	(void)ch; (void)enable_120ohm;
	return GCAN_NATIVE_ERR_INVALID;
}

int gcan_native_channel_send(gcan_native_channel *ch, const gcan_native_frame *frame, unsigned int timeout_ms)
{
	if (!ch || !ch->dev || !ch->dev->handle || !frame)
		return GCAN_NATIVE_ERR_INVALID;
	if (frame->len > GCAN_NATIVE_MAX_DLEN)
		return GCAN_NATIVE_ERR_INVALID;
	if (frame->can_id & (GCAN_NATIVE_RTR_FLAG | GCAN_NATIVE_ERR_FLAG))
		return GCAN_NATIVE_ERR_INVALID; /* not confirmed supported by this protocol yet */

	int is_extended = (frame->can_id & GCAN_NATIVE_EFF_FLAG) ? 1 : 0;
	uint32_t bare_id = frame->can_id & (is_extended ? GCAN_NATIVE_EFF_MASK : GCAN_NATIVE_SFF_MASK);
	uint32_t wire_id = bare_id | (is_extended ? GCAN_NATIVE_WIRE_EFF_FLAG : 0);

	uint8_t pkt[GCAN_NATIVE_CMD_LEN];
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = GCAN_NATIVE_TX_OPCODE_CAN1;
	pkt[1] = (uint8_t)(wire_id & 0xFF);
	pkt[2] = (uint8_t)((wire_id >> 8) & 0xFF);
	pkt[3] = (uint8_t)((wire_id >> 16) & 0xFF);
	pkt[4] = (uint8_t)((wire_id >> 24) & 0xFF);
	pkt[5] = frame->len;
	memcpy(&pkt[6], frame->data, frame->len);

	int transferred = 0;
	int rc = libusb_bulk_transfer(ch->dev->handle, GCAN_NATIVE_EP_OUT, pkt, sizeof(pkt),
				      &transferred, (unsigned int)timeout_ms);
	if (rc == LIBUSB_ERROR_TIMEOUT)
		return GCAN_NATIVE_ERR_TIMEOUT;
	if (rc != 0 || transferred != (int)sizeof(pkt))
		return GCAN_NATIVE_ERR_IO;
	return GCAN_NATIVE_OK;
}

/* Returns 1 and fills *frame if `rec` decodes to a plausible classic-CAN
 * frame, 0 if it doesn't (discarded the same way the reverse-engineered
 * CLI tool this is built from did -- see gcan_native_proto.h). */
static int parse_record(const uint8_t *rec, gcan_native_frame *frame)
{
	uint32_t wire_id = (uint32_t)rec[0] | ((uint32_t)rec[1] << 8) |
			   ((uint32_t)rec[2] << 16) | ((uint32_t)rec[3] << 24);
	uint8_t dlc = rec[4];
	if (dlc > GCAN_NATIVE_MAX_DLEN)
		return 0;

	int is_extended = (wire_id & GCAN_NATIVE_WIRE_EFF_FLAG) ? 1 : 0;
	uint32_t bare_id = is_extended ? (wire_id & GCAN_NATIVE_EFF_MASK) : wire_id;
	if (!is_extended && bare_id > GCAN_NATIVE_SFF_MASK)
		return 0;

	frame->can_id = bare_id | (is_extended ? GCAN_NATIVE_EFF_FLAG : 0);
	frame->len = dlc;
	memcpy(frame->data, &rec[5], dlc);
	frame->timestamp = 0; /* not decoded yet, see gcan_native_frame's doc */
	return 1;
}

int gcan_native_channel_recv(gcan_native_channel *ch, gcan_native_frame *frame, unsigned int timeout_ms)
{
	if (!ch || !ch->dev || !ch->dev->handle || !frame)
		return GCAN_NATIVE_ERR_INVALID;

	if (ch->pending_off >= ch->pending_len) {
		uint8_t buf[64];
		int transferred = 0;
		int rc = libusb_bulk_transfer(ch->dev->handle, GCAN_NATIVE_EP_IN, buf, sizeof(buf),
					      &transferred, (unsigned int)timeout_ms);
		if (rc == LIBUSB_ERROR_TIMEOUT)
			return 0;
		if (rc != 0)
			return GCAN_NATIVE_ERR_IO;

		if (transferred < GCAN_NATIVE_REC_LEN || (transferred % GCAN_NATIVE_REC_LEN) != 0)
			return 0; /* not a recognized frame record -- ignore */

		memcpy(ch->pending, buf, (size_t)transferred);
		ch->pending_len = transferred;
		ch->pending_off = 0;
	}

	const uint8_t *rec = &ch->pending[ch->pending_off];
	ch->pending_off += GCAN_NATIVE_REC_LEN;
	return parse_record(rec, frame);
}

const char *gcan_native_strerror(int err)
{
	switch (err) {
	case GCAN_NATIVE_OK: return "success";
	case GCAN_NATIVE_ERR_IO: return "I/O error";
	case GCAN_NATIVE_ERR_NOMEM: return "out of memory";
	case GCAN_NATIVE_ERR_NOT_FOUND: return "device not found";
	case GCAN_NATIVE_ERR_ACCESS: return "access denied";
	case GCAN_NATIVE_ERR_TIMEOUT: return "timeout";
	case GCAN_NATIVE_ERR_INVALID: return "invalid argument or unsupported configuration";
	case GCAN_NATIVE_ERR_BUSY: return "busy";
	case GCAN_NATIVE_ERR_NO_BITTIMING_SOLUTION: return "no bit-timing solution";
	default: return "unknown error";
	}
}
