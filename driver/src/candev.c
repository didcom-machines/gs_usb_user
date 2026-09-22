/* candev.c - see include/candev.h. */
#define _POSIX_C_SOURCE 200809L

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/candev.h"
#include "../include/gsusb.h"
#include "../include/slcan.h"

struct candev {
	enum candev_backend backend;
	union {
		struct {
			void *ctx;
			gsusb_dev *dev;
			gsusb_channel *ch;
		} g;
		struct {
			slcan_dev *dev;
		} s;
	} u;
};

const char *candev_backend_name(enum candev_backend backend)
{
	switch (backend) {
	case CANDEV_BACKEND_GSUSB: return "gs_usb";
	case CANDEV_BACKEND_SLCAN: return "slcan";
	default: return "none";
	}
}

enum candev_backend candev_which_backend(const candev *dev)
{
	return dev ? dev->backend : CANDEV_BACKEND_NONE;
}

const char *candev_strerror(int err)
{
	switch (err) {
	case CANDEV_OK: return "success";
	case CANDEV_ERR_IO: return "I/O error";
	case CANDEV_ERR_NOMEM: return "out of memory";
	case CANDEV_ERR_NOT_FOUND: return "device not found";
	case CANDEV_ERR_ACCESS: return "permission denied";
	case CANDEV_ERR_TIMEOUT: return "operation timed out";
	case CANDEV_ERR_INVALID: return "invalid argument";
	case CANDEV_ERR_BUSY: return "device or channel busy";
	default: return "unknown error";
	}
}

/* The shared error-code subset is numerically identical between gsusb.h and
 * slcan.h (both -1..-7, same meaning), but mapped explicitly rather than
 * passed through raw so the backends stay free to diverge later (e.g.
 * gsusb's -8 GSUSB_ERR_NO_BITTIMING_SOLUTION, which has no slcan
 * equivalent and folds into CANDEV_ERR_INVALID here). */
static int map_err(int rc)
{
	switch (rc) {
	case 0: return CANDEV_OK;
	case -1: return CANDEV_ERR_IO;
	case -2: return CANDEV_ERR_NOMEM;
	case -3: return CANDEV_ERR_NOT_FOUND;
	case -4: return CANDEV_ERR_ACCESS;
	case -5: return CANDEV_ERR_TIMEOUT;
	case -6: return CANDEV_ERR_INVALID;
	case -7: return CANDEV_ERR_BUSY;
	default: return rc < 0 ? CANDEV_ERR_INVALID : rc;
	}
}

/* --- gs_usb discovery --- */

static const struct { uint16_t vid, pid; } gsusb_candidates[] = {
	{ GSUSB_VID_GS_USB_1,        GSUSB_PID_GS_USB_1 },
	{ GSUSB_VID_CANDLELIGHT,     GSUSB_PID_CANDLELIGHT },
	{ GSUSB_VID_CES_CANEXT_FD,   GSUSB_PID_CES_CANEXT_FD },
	{ GSUSB_VID_ABE_CANDEBUGGER, GSUSB_PID_ABE_CANDEBUGGER },
	{ GSUSB_VID_XYLANTA_SAINT3,  GSUSB_PID_XYLANTA_SAINT3 },
	{ GSUSB_VID_CANNECTIVITY,    GSUSB_PID_CANNECTIVITY },
};
#define GSUSB_CANDIDATE_COUNT (sizeof(gsusb_candidates) / sizeof(gsusb_candidates[0]))

static candev *try_gsusb(void)
{
	void *ctx;
	if (gsusb_init(&ctx) != 0)
		return NULL;

	gsusb_dev *gdev = NULL;
	char errbuf[256];
	for (size_t i = 0; i < GSUSB_CANDIDATE_COUNT && !gdev; i++)
		gdev = gsusb_open(ctx, gsusb_candidates[i].vid, gsusb_candidates[i].pid,
				   -1, -1, errbuf, sizeof(errbuf));

	if (!gdev) {
		gsusb_exit(ctx);
		return NULL;
	}

	gsusb_channel *ch = gsusb_channel_get(gdev, 0);
	if (!ch) {
		gsusb_close(gdev);
		gsusb_exit(ctx);
		return NULL;
	}

	candev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		gsusb_close(gdev);
		gsusb_exit(ctx);
		return NULL;
	}
	dev->backend = CANDEV_BACKEND_GSUSB;
	dev->u.g.ctx = ctx;
	dev->u.g.dev = gdev;
	dev->u.g.ch = ch;
	return dev;
}

/* --- SLCAN discovery (best-effort, see candev_open()'s doc comment) --- */

#define SLCAN_PROBE_TIMEOUT_MS 300u

static candev *open_slcan_path(const char *path)
{
	char errbuf[256];
	slcan_dev *sd = slcan_open(path, errbuf, sizeof(errbuf));
	if (!sd)
		return NULL;

	candev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		slcan_close(sd);
		return NULL;
	}
	dev->backend = CANDEV_BACKEND_SLCAN;
	dev->u.s.dev = sd;
	return dev;
}

static candev *try_slcan(const char *slcan_hint)
{
	if (slcan_hint)
		return open_slcan_path(slcan_hint);

	glob_t g;
	if (glob("/dev/ttyACM*", 0, NULL, &g) != 0)
		return NULL;

	candev *found = NULL;
	for (size_t i = 0; i < g.gl_pathc && !found; i++) {
		if (slcan_probe(g.gl_pathv[i], SLCAN_PROBE_TIMEOUT_MS))
			found = open_slcan_path(g.gl_pathv[i]);
	}
	globfree(&g);
	return found;
}

candev *candev_open(const char *slcan_hint, char *errbuf, size_t errbuf_len)
{
	candev *dev = try_gsusb();
	if (!dev)
		dev = try_slcan(slcan_hint);

	if (!dev && errbuf && errbuf_len)
		snprintf(errbuf, errbuf_len,
			 "no CAN adapter found (checked gs_usb VID:PID list and %s)",
			 slcan_hint ? slcan_hint : "/dev/ttyACM*");
	return dev;
}

void candev_close(candev *dev)
{
	if (!dev)
		return;
	switch (dev->backend) {
	case CANDEV_BACKEND_GSUSB:
		gsusb_close(dev->u.g.dev);
		gsusb_exit(dev->u.g.ctx);
		break;
	case CANDEV_BACKEND_SLCAN:
		slcan_close(dev->u.s.dev);
		break;
	default:
		break;
	}
	free(dev);
}

/* --- control --- */

int candev_set_bitrate(candev *dev, uint32_t bitrate)
{
	if (!dev)
		return CANDEV_ERR_INVALID;
	if (dev->backend == CANDEV_BACKEND_GSUSB)
		return map_err(gsusb_channel_set_bitrate(dev->u.g.ch, bitrate, 0.0));
	if (dev->backend == CANDEV_BACKEND_SLCAN)
		return map_err(slcan_set_bitrate(dev->u.s.dev, bitrate));
	return CANDEV_ERR_INVALID;
}

int candev_start(candev *dev, int listen_only)
{
	if (!dev)
		return CANDEV_ERR_INVALID;
	if (dev->backend == CANDEV_BACKEND_GSUSB)
		return map_err(gsusb_channel_start(dev->u.g.ch,
						    listen_only ? GSUSB_MODE_LISTEN_ONLY : 0));
	if (dev->backend == CANDEV_BACKEND_SLCAN)
		return map_err(slcan_start(dev->u.s.dev, listen_only));
	return CANDEV_ERR_INVALID;
}

int candev_stop(candev *dev)
{
	if (!dev)
		return CANDEV_ERR_INVALID;
	if (dev->backend == CANDEV_BACKEND_GSUSB)
		return map_err(gsusb_channel_stop(dev->u.g.ch));
	if (dev->backend == CANDEV_BACKEND_SLCAN)
		return map_err(slcan_stop(dev->u.s.dev));
	return CANDEV_ERR_INVALID;
}

static enum candev_state map_state(uint32_t raw)
{
	switch (raw) {
	case 0: return CANDEV_STATE_ERROR_ACTIVE;
	case 1: return CANDEV_STATE_ERROR_WARNING;
	case 2: return CANDEV_STATE_ERROR_PASSIVE;
	case 3: return CANDEV_STATE_BUS_OFF;
	default: return CANDEV_STATE_UNKNOWN;
	}
}

int candev_get_state(candev *dev, enum candev_state *state, uint32_t *rxerr, uint32_t *txerr)
{
	if (!dev || !state)
		return CANDEV_ERR_INVALID;

	if (dev->backend == CANDEV_BACKEND_GSUSB) {
		uint32_t raw = 0, rx = 0, tx = 0;
		int rc = gsusb_channel_get_state(dev->u.g.ch, &raw, &rx, &tx);
		if (rc)
			return map_err(rc);
		*state = map_state(raw);
		if (rxerr) *rxerr = rx;
		if (txerr) *txerr = tx;
		return CANDEV_OK;
	}
	if (dev->backend == CANDEV_BACKEND_SLCAN) {
		enum slcan_state raw;
		uint32_t rx = 0, tx = 0;
		slcan_get_state(dev->u.s.dev, &raw, &rx, &tx);
		*state = map_state((uint32_t)raw);
		if (rxerr) *rxerr = rx;
		if (txerr) *txerr = tx;
		return CANDEV_OK;
	}
	return CANDEV_ERR_INVALID;
}

/* --- data --- */

int candev_send(candev *dev, const candev_frame *frame, unsigned int timeout_ms)
{
	if (!dev || !frame)
		return CANDEV_ERR_INVALID;
	if (frame->len > CANDEV_MAX_DLEN)
		return CANDEV_ERR_INVALID;

	if (dev->backend == CANDEV_BACKEND_GSUSB) {
		gsusb_frame gf = { .can_id = frame->can_id, .len = frame->len };
		memcpy(gf.data, frame->data, frame->len);
		return map_err(gsusb_channel_send(dev->u.g.ch, &gf, timeout_ms));
	}
	if (dev->backend == CANDEV_BACKEND_SLCAN) {
		slcan_frame sf = { .can_id = frame->can_id, .len = frame->len };
		memcpy(sf.data, frame->data, frame->len);
		return map_err(slcan_send(dev->u.s.dev, &sf, timeout_ms));
	}
	return CANDEV_ERR_INVALID;
}

int candev_recv(candev *dev, candev_frame *frame, unsigned int timeout_ms)
{
	if (!dev || !frame)
		return CANDEV_ERR_INVALID;

	if (dev->backend == CANDEV_BACKEND_GSUSB) {
		gsusb_frame gf;
		int r = gsusb_channel_recv(dev->u.g.ch, &gf, timeout_ms);
		if (r == 0)
			return 0; /* timeout */
		if (r < 0)
			return map_err(r);
		frame->can_id = gf.can_id;
		frame->len = gf.len > CANDEV_MAX_DLEN ? CANDEV_MAX_DLEN : gf.len;
		memcpy(frame->data, gf.data, frame->len);
		return 1;
	}
	if (dev->backend == CANDEV_BACKEND_SLCAN) {
		slcan_frame sf;
		int r = slcan_recv(dev->u.s.dev, &sf, timeout_ms);
		if (r == 0)
			return 0;
		if (r < 0)
			return map_err(r);
		frame->can_id = sf.can_id;
		frame->len = sf.len;
		memcpy(frame->data, sf.data, sf.len);
		return 1;
	}
	return CANDEV_ERR_INVALID;
}
