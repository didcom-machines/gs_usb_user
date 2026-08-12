/* gsusb.c - userspace port of drivers/net/can/usb/gs_usb.c, built on
 * libusb-1.0. See include/gsusb.h for the public API and src/gsusb_proto.h
 * for the wire protocol this replicates.
 *
 * Simplifications versus the kernel driver (documented, not accidental):
 *  - Synchronous bulk transfers driven by one background reader thread per
 *    device, instead of the kernel's ~30 concurrent RX / 10 concurrent TX
 *    URBs. Plenty for diagnostics/testing; a high-throughput production
 *    use would want libusb's async API instead.
 *  - No tx-echo/context tracking: gsusb_channel_send() reports success once
 *    the USB OUT transfer completes; it does not wait for the device's
 *    tx-ack frame. The reader thread still correctly recognizes and
 *    discards those ack frames (echo_id != GS_HOST_FRAME_ECHO_ID_RX) so
 *    they never show up as bogus received frames.
 *  - The CANtact Pro / LPC546XX quirk (GS_CAN_FEATURE_REQ_USB_QUIRK_LPC546XX,
 *    GS_CAN_FEATURE_QUIRK_BREQ_CANTACT_PRO) is not implemented.
 *  - CAN_CTRLMODE_CC_LEN8_DLC (encoding a classic-CAN DLC of 9-15 while
 *    capping the actual payload at 8 bytes) is not implemented; classic CAN
 *    dlc is always == len, capped at 8.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb.h>

#include "../include/gsusb.h"
#include "gsusb_proto.h"
#include "bittiming.h"

#define GSUSB_CTRL_TIMEOUT_MS 1000u
#define GSUSB_RXQ_CAPACITY 512u
#define GSUSB_READER_POLL_MS 200

struct gsusb_channel {
	struct gsusb_dev *dev;
	unsigned int index;

	gsusb_bittiming_const bt_const;
	int has_data_bt_const;
	gsusb_bittiming_const data_bt_const;
	uint32_t feature;

	int started;
	int hw_ts_enabled;

	/* bounded RX queue, filled by the device's reader thread */
	pthread_mutex_t q_lock;
	pthread_cond_t q_cond;
	gsusb_frame *q_buf;
	unsigned int q_cap, q_head, q_len;
	unsigned long q_drops;
};

struct gsusb_dev {
	libusb_device_handle *handle;
	int interface_number;
	unsigned char ep_in, ep_out;

	gsusb_device_config devconf;
	unsigned int channel_count;
	struct gsusb_channel *channels;
	unsigned int hf_size_rx; /* fixed for the device's lifetime, see gsusb_open() */

	pthread_mutex_t start_lock; /* guards active_channels/reader_running/stop_flag */
	unsigned int active_channels;
	pthread_t reader_thread;
	int reader_running;
	int stop_flag;
};

/* --- error mapping / small helpers --- */

static int map_libusb_err(int rc)
{
	switch (rc) {
	case LIBUSB_SUCCESS:
		return 0;
	case LIBUSB_ERROR_TIMEOUT:
		return GSUSB_ERR_TIMEOUT;
	case LIBUSB_ERROR_ACCESS:
		return GSUSB_ERR_ACCESS;
	case LIBUSB_ERROR_NO_DEVICE:
	case LIBUSB_ERROR_NOT_FOUND:
		return GSUSB_ERR_NOT_FOUND;
	case LIBUSB_ERROR_BUSY:
		return GSUSB_ERR_BUSY;
	case LIBUSB_ERROR_NO_MEM:
		return GSUSB_ERR_NOMEM;
	case LIBUSB_ERROR_INVALID_PARAM:
		return GSUSB_ERR_INVALID;
	default:
		return GSUSB_ERR_IO;
	}
}

const char *gsusb_strerror(int err)
{
	switch (err) {
	case GSUSB_OK:
		return "success";
	case GSUSB_ERR_IO:
		return "I/O error";
	case GSUSB_ERR_NOMEM:
		return "out of memory";
	case GSUSB_ERR_NOT_FOUND:
		return "device not found";
	case GSUSB_ERR_ACCESS:
		return "permission denied (see udev/99-gsusb.rules, or run as root)";
	case GSUSB_ERR_TIMEOUT:
		return "operation timed out";
	case GSUSB_ERR_INVALID:
		return "invalid argument";
	case GSUSB_ERR_BUSY:
		return "device or channel busy";
	case GSUSB_ERR_NO_BITTIMING_SOLUTION:
		return "no bit-timing solution for requested bitrate";
	default:
		return "unknown error";
	}
}

static int ctrl_out(struct gsusb_dev *dev, uint8_t breq, uint16_t wValue,
		    uint16_t wIndex, const uint8_t *data, uint16_t len)
{
	int rc = libusb_control_transfer(dev->handle,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
		breq, wValue, wIndex, (unsigned char *)data, len, GSUSB_CTRL_TIMEOUT_MS);
	if (rc < 0)
		return map_libusb_err(rc);
	if (rc != (int)len)
		return GSUSB_ERR_IO;
	return 0;
}

static int ctrl_in(struct gsusb_dev *dev, uint8_t breq, uint16_t wValue,
		   uint16_t wIndex, uint8_t *data, uint16_t len)
{
	int rc = libusb_control_transfer(dev->handle,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
		breq, wValue, wIndex, data, len, GSUSB_CTRL_TIMEOUT_MS);
	if (rc < 0)
		return map_libusb_err(rc);
	if (rc != (int)len)
		return GSUSB_ERR_IO;
	return 0;
}

/* --- classic/FD dlc <-> len mapping --- */

uint8_t gsusb_len2dlc_cc(uint8_t len) { return len > 8 ? 8 : len; }
uint8_t gsusb_dlc2len_cc(uint8_t dlc) { return dlc > 8 ? 8 : dlc; }

static const uint8_t fd_dlc2len_tbl[16] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
};

uint8_t gsusb_dlc2len_fd(uint8_t dlc)
{
	if (dlc > 15)
		dlc = 15;
	return fd_dlc2len_tbl[dlc];
}

uint8_t gsusb_len2dlc_fd(uint8_t len)
{
	if (len <= 8) return len;
	if (len <= 12) return 9;
	if (len <= 16) return 10;
	if (len <= 20) return 11;
	if (len <= 24) return 12;
	if (len <= 32) return 13;
	if (len <= 48) return 14;
	return 15; /* covers len up to 64 */
}

/* --- struct (de)serialization: all wire fields are little-endian --- */

static void parse_bt_const(const uint8_t *p, gsusb_bittiming_const *btc)
{
	btc->feature   = gs_get_le32(p + 0);
	btc->fclk_can  = gs_get_le32(p + 4);
	btc->tseg1_min = gs_get_le32(p + 8);
	btc->tseg1_max = gs_get_le32(p + 12);
	btc->tseg2_min = gs_get_le32(p + 16);
	btc->tseg2_max = gs_get_le32(p + 20);
	btc->sjw_max   = gs_get_le32(p + 24);
	btc->brp_min   = gs_get_le32(p + 28);
	btc->brp_max   = gs_get_le32(p + 32);
	btc->brp_inc   = gs_get_le32(p + 36);
}

static void parse_bt_const_ext_data_phase(const uint8_t *p, gsusb_bittiming_const *btc,
					  uint32_t feature, uint32_t fclk_can)
{
	/* bytes 0..39 repeat the nominal bt_const layout (feature/fclk_can
	 * are shared); the data-phase fields start at byte 40. */
	btc->feature   = feature;
	btc->fclk_can  = fclk_can;
	btc->tseg1_min = gs_get_le32(p + 40);
	btc->tseg1_max = gs_get_le32(p + 44);
	btc->tseg2_min = gs_get_le32(p + 48);
	btc->tseg2_max = gs_get_le32(p + 52);
	btc->sjw_max   = gs_get_le32(p + 56);
	btc->brp_min   = gs_get_le32(p + 60);
	btc->brp_max   = gs_get_le32(p + 64);
	btc->brp_inc   = gs_get_le32(p + 68);
}

/* --- RX queue (per channel) --- */

static int rxq_init(struct gsusb_channel *ch)
{
	ch->q_cap = GSUSB_RXQ_CAPACITY;
	ch->q_buf = calloc(ch->q_cap, sizeof(*ch->q_buf));
	if (!ch->q_buf)
		return -1;
	ch->q_head = 0;
	ch->q_len = 0;
	ch->q_drops = 0;
	pthread_mutex_init(&ch->q_lock, NULL);
	pthread_cond_init(&ch->q_cond, NULL);
	return 0;
}

static void rxq_destroy(struct gsusb_channel *ch)
{
	pthread_mutex_destroy(&ch->q_lock);
	pthread_cond_destroy(&ch->q_cond);
	free(ch->q_buf);
	ch->q_buf = NULL;
}

static void rxq_push(struct gsusb_channel *ch, const gsusb_frame *f)
{
	pthread_mutex_lock(&ch->q_lock);
	if (ch->q_len == ch->q_cap) {
		/* drop oldest to make room; count it */
		ch->q_head = (ch->q_head + 1) % ch->q_cap;
		ch->q_len--;
		ch->q_drops++;
	}
	unsigned int tail = (ch->q_head + ch->q_len) % ch->q_cap;
	ch->q_buf[tail] = *f;
	ch->q_len++;
	pthread_cond_signal(&ch->q_cond);
	pthread_mutex_unlock(&ch->q_lock);
}

/* --- RX frame parsing (from a single bulk-IN packet) --- */

static void handle_rx_packet(struct gsusb_dev *dev, const uint8_t *buf, unsigned int actual_length)
{
	if (actual_length < GS_HOST_FRAME_HDR_SIZE)
		return; /* short/garbage read */

	uint32_t echo_id = gs_get_le32(buf + 0);
	uint32_t can_id  = gs_get_le32(buf + 4);
	uint8_t  can_dlc = gs_get_u8(buf + 8);
	uint8_t  channel = gs_get_u8(buf + 9);
	uint8_t  flags   = gs_get_u8(buf + 10);

	if (channel >= dev->channel_count)
		return;
	struct gsusb_channel *ch = &dev->channels[channel];

	if (echo_id != GS_HOST_FRAME_ECHO_ID_RX)
		return; /* our own tx-echo/ack - not a received frame */

	int is_fd = (flags & GS_CAN_FLAG_FD) != 0;
	uint8_t max_dlen = is_fd ? GS_FD_DLEN : GS_CLASSIC_DLEN;

	uint8_t data_len;
	if (is_fd)
		data_len = gsusb_dlc2len_fd(can_dlc);
	else if (can_id & GSUSB_RTR_FLAG)
		data_len = 0;
	else
		data_len = gsusb_dlc2len_cc(can_dlc);
	if (data_len > max_dlen)
		data_len = max_dlen;

	uint32_t ts = 0;
	if (ch->hw_ts_enabled) {
		unsigned int need = GS_HOST_FRAME_HDR_SIZE + max_dlen + GS_HOST_FRAME_TS_SIZE;
		if (actual_length < need)
			return;
		ts = gs_get_le32(buf + GS_HOST_FRAME_HDR_SIZE + max_dlen);
	} else {
		unsigned int need = GS_HOST_FRAME_HDR_SIZE + data_len;
		if (actual_length < need)
			return;
	}

	gsusb_frame f;
	memset(&f, 0, sizeof(f));
	f.can_id = can_id;
	f.len = data_len;
	if (is_fd)
		f.flags |= GSUSB_FRAME_FD;
	if (flags & GS_CAN_FLAG_BRS)
		f.flags |= GSUSB_FRAME_BRS;
	if (flags & GS_CAN_FLAG_ESI)
		f.flags |= GSUSB_FRAME_ESI;
	if (data_len)
		memcpy(f.data, buf + GS_HOST_FRAME_HDR_SIZE, data_len);
	f.timestamp_us = ts;

	rxq_push(ch, &f);
}

static void *reader_thread_fn(void *arg)
{
	struct gsusb_dev *dev = arg;
	uint8_t *buf = malloc(dev->hf_size_rx);
	if (!buf)
		return NULL;

	for (;;) {
		pthread_mutex_lock(&dev->start_lock);
		int stop = dev->stop_flag;
		pthread_mutex_unlock(&dev->start_lock);
		if (stop)
			break;

		int transferred = 0;
		int rc = libusb_bulk_transfer(dev->handle, dev->ep_in, buf,
					      (int)dev->hf_size_rx, &transferred,
					      GSUSB_READER_POLL_MS);
		if (rc == 0 && transferred > 0) {
			handle_rx_packet(dev, buf, (unsigned int)transferred);
		} else if (rc == LIBUSB_ERROR_TIMEOUT) {
			continue;
		} else if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
			break; /* device unplugged or dead */
		}
		/* other transient libusb errors: just retry */
	}

	free(buf);
	return NULL;
}

/* --- device open/close --- */

gsusb_dev *gsusb_open(void *ctx, uint16_t vid, uint16_t pid, int bus, int addr,
		      char *errbuf, size_t errbuf_len)
{
#define SETERR(...) do { if (errbuf && errbuf_len) snprintf(errbuf, errbuf_len, __VA_ARGS__); } while (0)

	libusb_device **list = NULL;
	ssize_t n = libusb_get_device_list((libusb_context *)ctx, &list);
	if (n < 0) {
		SETERR("libusb_get_device_list failed: %s", libusb_strerror((int)n));
		return NULL;
	}

	libusb_device *match = NULL;
	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;
		if (desc.idVendor != vid || desc.idProduct != pid)
			continue;
		if (bus >= 0 && libusb_get_bus_number(list[i]) != (uint8_t)bus)
			continue;
		if (addr >= 0 && libusb_get_device_address(list[i]) != (uint8_t)addr)
			continue;
		match = list[i];
		break;
	}

	if (!match) {
		libusb_free_device_list(list, 1);
		SETERR("no device matching %04x:%04x found", vid, pid);
		return NULL;
	}

	libusb_device_handle *handle = NULL;
	int rc = libusb_open(match, &handle);
	libusb_free_device_list(list, 1);
	if (rc != 0) {
		SETERR("libusb_open failed: %s", libusb_strerror(rc));
		return NULL;
	}

	libusb_set_auto_detach_kernel_driver(handle, 1); /* best effort, ignore result */

	const int interface_number = 0; /* every known gs_usb device id uses interface 0 */
	rc = libusb_claim_interface(handle, interface_number);
	if (rc != 0) {
		SETERR("libusb_claim_interface failed: %s (device busy or insufficient permissions)",
		       libusb_strerror(rc));
		libusb_close(handle);
		return NULL;
	}

	libusb_device *usbdev = libusb_get_device(handle);
	struct libusb_config_descriptor *config = NULL;
	rc = libusb_get_active_config_descriptor(usbdev, &config);
	if (rc != 0) {
		SETERR("libusb_get_active_config_descriptor failed: %s", libusb_strerror(rc));
		libusb_release_interface(handle, interface_number);
		libusb_close(handle);
		return NULL;
	}

	unsigned char ep_in = 0, ep_out = 0;
	for (int i = 0; i < config->bNumInterfaces && (!ep_in || !ep_out); i++) {
		const struct libusb_interface *intf = &config->interface[i];
		const struct libusb_interface_descriptor *alt = &intf->altsetting[0];
		if (alt->bInterfaceNumber != interface_number)
			continue;
		for (int e = 0; e < alt->bNumEndpoints; e++) {
			const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
			int type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
			if (type != LIBUSB_TRANSFER_TYPE_BULK)
				continue;
			if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
				if (!ep_in)
					ep_in = ep->bEndpointAddress;
			} else {
				if (!ep_out)
					ep_out = ep->bEndpointAddress;
			}
		}
	}
	libusb_free_config_descriptor(config);

	if (!ep_in || !ep_out) {
		SETERR("required bulk IN/OUT endpoints not found");
		libusb_release_interface(handle, interface_number);
		libusb_close(handle);
		return NULL;
	}

	/* send host config: candleLight firmware ignores byte order and is
	 * always little-endian; the original GS USB firmware honors this to
	 * select little-endian too (0x0000beef), which matches every
	 * real-world little-endian Linux host. */
	uint8_t hostcfg[GS_HOST_CONFIG_SIZE];
	gs_put_le32(hostcfg, 0x0000beef);

	struct gsusb_dev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		SETERR("out of memory");
		libusb_release_interface(handle, interface_number);
		libusb_close(handle);
		return NULL;
	}
	dev->handle = handle;
	dev->interface_number = interface_number;
	dev->ep_in = ep_in;
	dev->ep_out = ep_out;

	rc = ctrl_out(dev, GS_USB_BREQ_HOST_FORMAT, 1, (uint16_t)interface_number,
		     hostcfg, sizeof(hostcfg));
	if (rc) {
		SETERR("HOST_FORMAT request failed: %s", gsusb_strerror(rc));
		goto fail_free_dev;
	}

	uint8_t devcfg_raw[GS_DEVICE_CONFIG_SIZE];
	rc = ctrl_in(dev, GS_USB_BREQ_DEVICE_CONFIG, 1, (uint16_t)interface_number,
		    devcfg_raw, sizeof(devcfg_raw));
	if (rc) {
		SETERR("DEVICE_CONFIG request failed: %s", gsusb_strerror(rc));
		goto fail_free_dev;
	}
	uint8_t icount = devcfg_raw[3];
	dev->devconf.sw_version = gs_get_le32(devcfg_raw + 4);
	dev->devconf.hw_version = gs_get_le32(devcfg_raw + 8);
	dev->devconf.channel_count = (uint8_t)(icount + 1);
	dev->channel_count = dev->devconf.channel_count;

	dev->channels = calloc(dev->channel_count, sizeof(*dev->channels));
	if (!dev->channels) {
		SETERR("out of memory");
		goto fail_free_dev;
	}

	dev->hf_size_rx = 0;
	for (unsigned int i = 0; i < dev->channel_count; i++) {
		struct gsusb_channel *ch = &dev->channels[i];
		ch->dev = dev;
		ch->index = i;

		uint8_t btc_raw[GS_DEVICE_BT_CONST_SIZE];
		rc = ctrl_in(dev, GS_USB_BREQ_BT_CONST, (uint16_t)i, 0, btc_raw, sizeof(btc_raw));
		if (rc) {
			SETERR("BT_CONST request failed for channel %u: %s", i, gsusb_strerror(rc));
			goto fail_free_channels;
		}
		parse_bt_const(btc_raw, &ch->bt_const);
		ch->feature = ch->bt_const.feature & GS_CAN_FEATURE_MASK;

		if ((ch->feature & GS_CAN_FEATURE_FD) && (ch->feature & GS_CAN_FEATURE_BT_CONST_EXT)) {
			uint8_t ext_raw[GS_DEVICE_BT_CONST_EXT_SIZE];
			rc = ctrl_in(dev, GS_USB_BREQ_BT_CONST_EXT, (uint16_t)i, 0,
				    ext_raw, sizeof(ext_raw));
			if (rc) {
				SETERR("BT_CONST_EXT request failed for channel %u: %s", i, gsusb_strerror(rc));
				goto fail_free_channels;
			}
			parse_bt_const_ext_data_phase(ext_raw, &ch->data_bt_const,
						     ch->bt_const.feature, ch->bt_const.fclk_can);
			ch->has_data_bt_const = 1;
		}

		if (rxq_init(ch) != 0) {
			SETERR("out of memory (rx queue)");
			goto fail_free_channels;
		}

		/* Worst case wire size this channel could ever produce: FD
		 * doubles the payload, hw timestamps add a trailing u32. We
		 * always enable hw timestamps in gsusb_channel_start() when
		 * the feature is available, so size for that up front - the
		 * device-wide rx buffer size is fixed for the object's
		 * lifetime, which keeps the reader thread simple. */
		unsigned int max_dlen = (ch->feature & GS_CAN_FEATURE_FD) ? GS_FD_DLEN : GS_CLASSIC_DLEN;
		unsigned int with_ts = (ch->feature & GS_CAN_FEATURE_HW_TIMESTAMP) ? 1 : 0;
		unsigned int size = gs_frame_size(max_dlen, (int)with_ts);
		if (size > dev->hf_size_rx)
			dev->hf_size_rx = size;
	}

	pthread_mutex_init(&dev->start_lock, NULL);
	dev->active_channels = 0;
	dev->reader_running = 0;
	dev->stop_flag = 0;

	return dev;

fail_free_channels:
	for (unsigned int i = 0; i < dev->channel_count; i++) {
		if (dev->channels[i].q_buf)
			rxq_destroy(&dev->channels[i]);
	}
	free(dev->channels);
fail_free_dev:
	libusb_release_interface(handle, interface_number);
	libusb_close(handle);
	free(dev);
	return NULL;

#undef SETERR
}

void gsusb_close(gsusb_dev *dev)
{
	if (!dev)
		return;

	pthread_mutex_lock(&dev->start_lock);
	int was_running = dev->reader_running;
	dev->stop_flag = 1;
	pthread_mutex_unlock(&dev->start_lock);

	if (was_running)
		pthread_join(dev->reader_thread, NULL);

	for (unsigned int i = 0; i < dev->channel_count; i++) {
		struct gsusb_channel *ch = &dev->channels[i];
		if (ch->started) {
			uint8_t modebuf[GS_DEVICE_MODE_SIZE];
			gs_put_le32(modebuf + 0, GS_CAN_MODE_RESET);
			gs_put_le32(modebuf + 4, 0);
			ctrl_out(dev, GS_USB_BREQ_MODE, (uint16_t)ch->index, 0,
				modebuf, sizeof(modebuf)); /* best effort */
		}
		rxq_destroy(ch);
	}

	pthread_mutex_destroy(&dev->start_lock);
	free(dev->channels);
	libusb_release_interface(dev->handle, dev->interface_number);
	libusb_close(dev->handle);
	free(dev);
}

int gsusb_init(void **ctx_out)
{
	libusb_context *ctx = NULL;
	int rc = libusb_init(&ctx);
	if (rc != 0)
		return map_libusb_err(rc);
	*ctx_out = ctx;
	return 0;
}

void gsusb_exit(void *ctx)
{
	if (ctx)
		libusb_exit((libusb_context *)ctx);
}

unsigned int gsusb_channel_count(const gsusb_dev *dev) { return dev->channel_count; }

const gsusb_device_config *gsusb_get_device_config(const gsusb_dev *dev) { return &dev->devconf; }

gsusb_channel *gsusb_channel_get(gsusb_dev *dev, unsigned int channel)
{
	if (channel >= dev->channel_count)
		return NULL;
	return &dev->channels[channel];
}

const gsusb_bittiming_const *gsusb_channel_bt_const(const gsusb_channel *ch) { return &ch->bt_const; }

const gsusb_bittiming_const *gsusb_channel_data_bt_const(const gsusb_channel *ch)
{
	return ch->has_data_bt_const ? &ch->data_bt_const : NULL;
}

uint32_t gsusb_channel_features(const gsusb_channel *ch) { return ch->feature; }

/* --- bit timing --- */

static int send_bittiming(struct gsusb_dev *dev, uint8_t breq, unsigned int channel,
			  uint32_t prop_seg, uint32_t phase_seg1, uint32_t phase_seg2,
			  uint32_t sjw, uint32_t brp)
{
	uint8_t buf[GS_DEVICE_BITTIMING_SIZE];
	gs_put_le32(buf + 0, prop_seg);
	gs_put_le32(buf + 4, phase_seg1);
	gs_put_le32(buf + 8, phase_seg2);
	gs_put_le32(buf + 12, sjw);
	gs_put_le32(buf + 16, brp);
	return ctrl_out(dev, breq, (uint16_t)channel, 0, buf, sizeof(buf));
}

int gsusb_channel_set_bittiming_raw(gsusb_channel *ch, uint32_t prop_seg, uint32_t phase_seg1,
				    uint32_t phase_seg2, uint32_t sjw, uint32_t brp)
{
	return send_bittiming(ch->dev, GS_USB_BREQ_BITTIMING, ch->index,
			      prop_seg, phase_seg1, phase_seg2, sjw, brp);
}

int gsusb_channel_set_data_bittiming_raw(gsusb_channel *ch, uint32_t prop_seg, uint32_t phase_seg1,
					 uint32_t phase_seg2, uint32_t sjw, uint32_t brp)
{
	return send_bittiming(ch->dev, GS_USB_BREQ_DATA_BITTIMING, ch->index,
			      prop_seg, phase_seg1, phase_seg2, sjw, brp);
}

int gsusb_channel_set_bitrate(gsusb_channel *ch, uint32_t bitrate, double sample_point)
{
	uint32_t prop_seg, phase_seg1, phase_seg2, sjw, brp;
	int rc = gsusb_calc_bittiming(ch->bt_const.fclk_can, bitrate, sample_point,
				      &ch->bt_const, &prop_seg, &phase_seg1, &phase_seg2, &sjw, &brp);
	if (rc)
		return rc;
	return gsusb_channel_set_bittiming_raw(ch, prop_seg, phase_seg1, phase_seg2, sjw, brp);
}

int gsusb_channel_set_data_bitrate(gsusb_channel *ch, uint32_t bitrate, double sample_point)
{
	const gsusb_bittiming_const *btc = ch->has_data_bt_const ? &ch->data_bt_const : &ch->bt_const;
	uint32_t prop_seg, phase_seg1, phase_seg2, sjw, brp;
	int rc = gsusb_calc_bittiming(btc->fclk_can, bitrate, sample_point,
				      btc, &prop_seg, &phase_seg1, &phase_seg2, &sjw, &brp);
	if (rc)
		return rc;
	return gsusb_channel_set_data_bittiming_raw(ch, prop_seg, phase_seg1, phase_seg2, sjw, brp);
}

/* --- start/stop --- */

int gsusb_channel_start(gsusb_channel *ch, uint32_t mode_flags)
{
	struct gsusb_dev *dev = ch->dev;

	pthread_mutex_lock(&dev->start_lock);
	if (ch->started) {
		pthread_mutex_unlock(&dev->start_lock);
		return GSUSB_ERR_BUSY;
	}

	if ((mode_flags & GSUSB_MODE_FD) && !(ch->feature & GS_CAN_FEATURE_FD)) {
		pthread_mutex_unlock(&dev->start_lock);
		return GSUSB_ERR_INVALID;
	}

	uint32_t hw_flags = 0;
	if (mode_flags & GSUSB_MODE_LISTEN_ONLY)    hw_flags |= GS_CAN_MODE_LISTEN_ONLY;
	if (mode_flags & GSUSB_MODE_LOOPBACK)       hw_flags |= GS_CAN_MODE_LOOP_BACK;
	if (mode_flags & GSUSB_MODE_TRIPLE_SAMPLE)  hw_flags |= GS_CAN_MODE_TRIPLE_SAMPLE;
	if (mode_flags & GSUSB_MODE_ONE_SHOT)       hw_flags |= GS_CAN_MODE_ONE_SHOT;
	if (mode_flags & GSUSB_MODE_BERR_REPORTING) hw_flags |= GS_CAN_MODE_BERR_REPORTING;
	if (mode_flags & GSUSB_MODE_FD)             hw_flags |= GS_CAN_MODE_FD;

	int hw_ts = (ch->feature & GS_CAN_FEATURE_HW_TIMESTAMP) != 0;
	if (hw_ts)
		hw_flags |= GS_CAN_MODE_HW_TIMESTAMP;

	uint8_t modebuf[GS_DEVICE_MODE_SIZE];
	gs_put_le32(modebuf + 0, GS_CAN_MODE_START);
	gs_put_le32(modebuf + 4, hw_flags);
	int rc = ctrl_out(dev, GS_USB_BREQ_MODE, (uint16_t)ch->index, 0, modebuf, sizeof(modebuf));
	if (rc) {
		pthread_mutex_unlock(&dev->start_lock);
		return rc;
	}

	ch->hw_ts_enabled = hw_ts;
	ch->started = 1;
	dev->active_channels++;

	if (!dev->reader_running) {
		dev->stop_flag = 0;
		if (pthread_create(&dev->reader_thread, NULL, reader_thread_fn, dev) != 0) {
			ch->started = 0;
			dev->active_channels--;
			pthread_mutex_unlock(&dev->start_lock);
			return GSUSB_ERR_IO;
		}
		dev->reader_running = 1;
	}

	pthread_mutex_unlock(&dev->start_lock);
	return 0;
}

int gsusb_channel_stop(gsusb_channel *ch)
{
	struct gsusb_dev *dev = ch->dev;

	pthread_mutex_lock(&dev->start_lock);
	if (!ch->started) {
		pthread_mutex_unlock(&dev->start_lock);
		return 0;
	}
	ch->started = 0;
	if (dev->active_channels > 0)
		dev->active_channels--;
	int stop_reader = (dev->active_channels == 0);
	if (stop_reader)
		dev->stop_flag = 1;
	pthread_mutex_unlock(&dev->start_lock);

	uint8_t modebuf[GS_DEVICE_MODE_SIZE];
	gs_put_le32(modebuf + 0, GS_CAN_MODE_RESET);
	gs_put_le32(modebuf + 4, 0);
	ctrl_out(dev, GS_USB_BREQ_MODE, (uint16_t)ch->index, 0, modebuf, sizeof(modebuf));

	if (stop_reader) {
		pthread_join(dev->reader_thread, NULL);
		pthread_mutex_lock(&dev->start_lock);
		dev->reader_running = 0;
		pthread_mutex_unlock(&dev->start_lock);
	}
	return 0;
}

/* --- state / identify / termination --- */

int gsusb_channel_get_state(gsusb_channel *ch, uint32_t *state, uint32_t *rxerr, uint32_t *txerr)
{
	uint8_t buf[GS_DEVICE_STATE_SIZE];
	int rc = ctrl_in(ch->dev, GS_USB_BREQ_GET_STATE, (uint16_t)ch->index, 0, buf, sizeof(buf));
	if (rc)
		return rc;
	if (state) *state = gs_get_le32(buf + 0);
	if (rxerr) *rxerr = gs_get_le32(buf + 4);
	if (txerr) *txerr = gs_get_le32(buf + 8);
	return 0;
}

int gsusb_channel_set_identify(gsusb_channel *ch, int on)
{
	uint8_t buf[GS_IDENTIFY_MODE_SIZE];
	gs_put_le32(buf, on ? GS_CAN_IDENTIFY_ON : GS_CAN_IDENTIFY_OFF);
	return ctrl_out(ch->dev, GS_USB_BREQ_IDENTIFY, (uint16_t)ch->index, 0, buf, sizeof(buf));
}

int gsusb_channel_get_termination(gsusb_channel *ch, int *enabled_120ohm)
{
	uint8_t buf[GS_TERMINATION_STATE_SIZE];
	int rc = ctrl_in(ch->dev, GS_USB_BREQ_GET_TERMINATION, (uint16_t)ch->index, 0, buf, sizeof(buf));
	if (rc)
		return rc;
	*enabled_120ohm = (gs_get_le32(buf) == GS_CAN_TERMINATION_STATE_ON);
	return 0;
}

int gsusb_channel_set_termination(gsusb_channel *ch, int enable_120ohm)
{
	uint8_t buf[GS_TERMINATION_STATE_SIZE];
	gs_put_le32(buf, enable_120ohm ? GS_CAN_TERMINATION_STATE_ON : GS_CAN_TERMINATION_STATE_OFF);
	return ctrl_out(ch->dev, GS_USB_BREQ_SET_TERMINATION, (uint16_t)ch->index, 0, buf, sizeof(buf));
}

/* --- send / recv --- */

int gsusb_channel_send(gsusb_channel *ch, const gsusb_frame *frame, unsigned int timeout_ms)
{
	if (!ch->started)
		return GSUSB_ERR_INVALID;

	int is_fd = (frame->flags & GSUSB_FRAME_FD) != 0;
	if (is_fd && !(ch->feature & GS_CAN_FEATURE_FD))
		return GSUSB_ERR_INVALID;

	uint8_t max_dlen = is_fd ? GS_FD_DLEN : GS_CLASSIC_DLEN;
	uint8_t len = frame->len;
	if (len > max_dlen)
		len = max_dlen;
	uint8_t dlc = is_fd ? gsusb_len2dlc_fd(len) : gsusb_len2dlc_cc(len);

	unsigned int bufsz = GS_HOST_FRAME_HDR_SIZE + max_dlen;
	uint8_t buf[GS_HOST_FRAME_HDR_SIZE + GS_FD_DLEN];
	memset(buf, 0, bufsz);

	gs_put_le32(buf + 0, 0); /* echo_id: fixed, we don't track tx contexts */
	gs_put_le32(buf + 4, frame->can_id);
	buf[8] = dlc;
	buf[9] = (uint8_t)ch->index;
	uint8_t wflags = 0;
	if (is_fd) wflags |= GS_CAN_FLAG_FD;
	if (frame->flags & GSUSB_FRAME_BRS) wflags |= GS_CAN_FLAG_BRS;
	if (frame->flags & GSUSB_FRAME_ESI) wflags |= GS_CAN_FLAG_ESI;
	buf[10] = wflags;
	buf[11] = 0;
	if (len)
		memcpy(buf + GS_HOST_FRAME_HDR_SIZE, frame->data, len);

	int transferred = 0;
	int rc = libusb_bulk_transfer(ch->dev->handle, ch->dev->ep_out, buf, (int)bufsz,
				      &transferred, timeout_ms ? timeout_ms : GSUSB_CTRL_TIMEOUT_MS);
	if (rc == LIBUSB_ERROR_TIMEOUT)
		return GSUSB_ERR_TIMEOUT;
	if (rc != 0)
		return map_libusb_err(rc);
	if ((unsigned int)transferred != bufsz)
		return GSUSB_ERR_IO;
	return 0;
}

int gsusb_channel_recv(gsusb_channel *ch, gsusb_frame *frame, unsigned int timeout_ms)
{
	pthread_mutex_lock(&ch->q_lock);

	struct timespec deadline;
	if (timeout_ms > 0) {
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += timeout_ms / 1000;
		deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
	}

	while (ch->q_len == 0) {
		if (!ch->started) {
			pthread_mutex_unlock(&ch->q_lock);
			return GSUSB_ERR_INVALID;
		}
		if (timeout_ms == 0) {
			pthread_mutex_unlock(&ch->q_lock);
			return 0;
		}
		int rc = pthread_cond_timedwait(&ch->q_cond, &ch->q_lock, &deadline);
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&ch->q_lock);
			return 0;
		}
	}

	*frame = ch->q_buf[ch->q_head];
	ch->q_head = (ch->q_head + 1) % ch->q_cap;
	ch->q_len--;

	pthread_mutex_unlock(&ch->q_lock);
	return 1;
}
