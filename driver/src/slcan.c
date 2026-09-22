/* slcan.c - see include/slcan.h. Faithful userspace port of the protocol
 * in drivers/net/can/slcan/slcan-core.c (Linux kernel), reimplemented over
 * plain POSIX termios instead of a tty line discipline: no `slcan_attach`,
 * no N_SLCAN ldisc, no kernel module -- open the /dev/ttyACM* node the
 * kernel's generic cdc_acm driver already created, and speak the ASCII
 * protocol to it directly.
 *
 * Simplifications versus the kernel driver (documented, not accidental):
 *  - Command transmission does not wait for or parse a device response,
 *    matching the reference: slcan_transmit_cmd() only waits for the
 *    *local* tty write to flush, not for the adapter to acknowledge it.
 *    (The reference driver also never delivers received bytes to its
 *    parser while the netdev is down, i.e. during this same handshake --
 *    so on both sides, any reply to a setup command goes unexamined.)
 *  - 'e' (bus error) lines are recorded as their raw letters (see
 *    slcan_get_last_error()), not decoded into the kernel's CAN_ERR_* skb
 *    bit flags -- that encoding is SocketCAN-specific.
 *  - No 'Z' hardware-timestamp extension: the reference driver never
 *    sends or parses it, so neither do we.
 */
#define _POSIX_C_SOURCE 200809L
/* CRTSCTS (hardware flow control) is a glibc/BSD termios extension, not
 * POSIX -- needs this to be declared, unlike the rest of the flags used
 * here which are plain POSIX. */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../include/slcan.h"

#define SLCAN_CTRL_TIMEOUT_MS 1000u
#define SLCAN_RXQ_CAPACITY 512u
#define SLCAN_READER_POLL_MS 200

/* Matches the reference driver's slcan_bitrate_const[] exactly. */
static const uint32_t slcan_bitrate_table[] = {
	10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000
};
#define SLCAN_BITRATE_COUNT (sizeof(slcan_bitrate_table) / sizeof(slcan_bitrate_table[0]))

/* Longest possible line: type + 8 hex id (EFF) + 1 dlc + 16 hex data,
 * same idiom as the reference driver's SLCAN_MTU (there, "+ \r + NUL"; a
 * bare byte count is all we need here). */
#define SLCAN_LINE_MAX (1 + 8 + 1 + 16)

static const char hex_upper[] = "0123456789ABCDEF";

struct slcan_dev {
	int fd;

	pthread_mutex_t start_lock; /* guards reader_running/stop_flag */
	pthread_t reader_thread;
	int reader_running;
	int stop_flag;

	/* RX queue: the only thing that needs real synchronization, since
	 * frames must not be torn or lost. Mirrors gsusb.c's per-channel
	 * ring buffer (drop-oldest on overflow). */
	pthread_mutex_t q_lock;
	pthread_cond_t q_cond;
	slcan_frame *q_buf;
	unsigned int q_cap, q_head, q_len;

	/* State/error tracking and RX diagnostics: reader thread is the only
	 * writer, read without synchronization -- fine for best-effort
	 * diagnostics, not control flow (same convention as gsusb.c's
	 * gsusb_rx_stats). */
	enum slcan_state state;
	uint32_t rxerr, txerr;
	char last_error[16];
	unsigned long st_rx_frames, st_queue_drops, st_state_frames;
	unsigned long st_error_frames, st_decode_errors, st_line_overflows;

	/* line assembly buffer, reader thread only */
	unsigned char line[SLCAN_LINE_MAX];
	unsigned int line_len;
	int line_error; /* overflowed: discard until next terminator */
};

const char *slcan_strerror(int err)
{
	switch (err) {
	case SLCAN_OK: return "success";
	case SLCAN_ERR_IO: return "I/O error";
	case SLCAN_ERR_NOMEM: return "out of memory";
	case SLCAN_ERR_NOT_FOUND: return "device not found";
	case SLCAN_ERR_ACCESS: return "permission denied";
	case SLCAN_ERR_TIMEOUT: return "operation timed out";
	case SLCAN_ERR_INVALID: return "invalid argument";
	case SLCAN_ERR_BUSY: return "device or channel busy";
	default: return "unknown error";
	}
}

/* --- open/close --- */

#define SETERR(...) do { if (errbuf && errbuf_len) snprintf(errbuf, errbuf_len, __VA_ARGS__); } while (0)

/* Shared by slcan_open() and slcan_probe(): open device_path and put it in
 * raw mode with no flow control, ready to speak the SLCAN ASCII protocol.
 * Returns the fd, or -1 with a message in errbuf (if non-NULL). */
static int open_configured_tty(const char *device_path, char *errbuf, size_t errbuf_len)
{
	if (!device_path) {
		SETERR("no device path given");
		return -1;
	}

	int fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			SETERR("%s: no such device", device_path);
		else if (errno == EACCES)
			SETERR("%s: permission denied", device_path);
		else if (errno == EBUSY)
			SETERR("%s: busy (already open elsewhere?)", device_path);
		else
			SETERR("%s: open failed: %s", device_path, strerror(errno));
		return -1;
	}

	/* Drop O_NONBLOCK now that open() has succeeded (some ttys need it
	 * set just to open without blocking on modem-control lines); the
	 * reader thread uses poll() for timeouts instead. */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	struct termios tio;
	if (tcgetattr(fd, &tio) != 0) {
		SETERR("%s: tcgetattr failed: %s", device_path, strerror(errno));
		close(fd);
		return -1;
	}

	/* Manual equivalent of cfmakeraw() (see termios(3)) -- spelled out
	 * because cfmakeraw() itself needs _DEFAULT_SOURCE/_BSD_SOURCE,
	 * which would conflict with the strict _POSIX_C_SOURCE this driver
	 * builds with throughout. */
	tio.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	tio.c_oflag &= ~(tcflag_t)OPOST;
	tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
	tio.c_cflag |= CS8;

	/* No flow control of any kind: this is a virtual (USB CDC-ACM) serial
	 * port with no real handshake lines, and tcgetattr() above may have
	 * come back with CRTSCTS (or software XON/XOFF) already set from
	 * whatever the port's prior state was -- if so, writes silently block
	 * forever waiting for a CTS that will never be asserted. Found by
	 * hitting exactly that hang against real hardware. */
	tio.c_iflag &= ~(tcflag_t)(IXOFF | IXANY);
	tio.c_cflag &= ~(tcflag_t)CRTSCTS;

	/* USB CDC-ACM has no real UART to clock -- the device ignores this,
	 * but termios still requires a value; 115200 is the conventional
	 * placeholder every SLCAN client uses. */
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) != 0) {
		SETERR("%s: tcsetattr failed: %s", device_path, strerror(errno));
		close(fd);
		return -1;
	}
	tcflush(fd, TCIOFLUSH);
	return fd;
}

/* Best-effort liveness probe for SLCAN auto-detection (see candev.h): opens
 * device_path, sends the 'V' (version) command, and returns 1 if any bytes
 * come back within timeout_ms, 0 otherwise (including on open failure).
 * This is not a real protocol identification -- SLCAN has no standard way
 * to self-identify over a plain serial port, so any device that replies or
 * echoes to a stray "V\r" will pass. */
int slcan_probe(const char *device_path, unsigned int timeout_ms)
{
	int fd = open_configured_tty(device_path, NULL, 0);
	if (fd < 0)
		return 0;

	if (write(fd, "V\r", 2) != 2) {
		close(fd);
		return 0;
	}

	struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
	int rc = poll(&pfd, 1, (int)timeout_ms);
	int alive = 0;
	if (rc > 0 && (pfd.revents & POLLIN)) {
		unsigned char buf[32];
		ssize_t n = read(fd, buf, sizeof(buf));
		alive = n > 0;
	}
	close(fd);
	return alive;
}

slcan_dev *slcan_open(const char *device_path, char *errbuf, size_t errbuf_len)
{
	int fd = open_configured_tty(device_path, errbuf, errbuf_len);
	if (fd < 0)
		return NULL;

	struct slcan_dev *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		SETERR("out of memory");
		close(fd);
		return NULL;
	}
	dev->fd = fd;
	dev->state = SLCAN_STATE_UNKNOWN;

	dev->q_cap = SLCAN_RXQ_CAPACITY;
	dev->q_buf = calloc(dev->q_cap, sizeof(*dev->q_buf));
	if (!dev->q_buf) {
		SETERR("out of memory");
		close(fd);
		free(dev);
		return NULL;
	}
	pthread_mutex_init(&dev->start_lock, NULL);
	pthread_mutex_init(&dev->q_lock, NULL);
	pthread_cond_init(&dev->q_cond, NULL);

	return dev;
#undef SETERR
}

void slcan_close(slcan_dev *dev)
{
	if (!dev)
		return;
	slcan_stop(dev); /* no-op if never started */
	pthread_mutex_destroy(&dev->q_lock);
	pthread_cond_destroy(&dev->q_cond);
	pthread_mutex_destroy(&dev->start_lock);
	free(dev->q_buf);
	close(dev->fd);
	free(dev);
}

/* --- writing commands/frames --- */

static int write_all(struct slcan_dev *dev, const char *buf, size_t len, unsigned int timeout_ms)
{
	size_t written = 0;
	while (written < len) {
		struct pollfd pfd = { .fd = dev->fd, .events = POLLOUT, .revents = 0 };
		int rc = poll(&pfd, 1, (int)timeout_ms);
		if (rc == 0)
			return SLCAN_ERR_TIMEOUT;
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return SLCAN_ERR_IO;
		}
		ssize_t n = write(dev->fd, buf + written, len - written);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return SLCAN_ERR_IO;
		}
		written += (size_t)n;
	}
	return 0;
}

static int send_cmd(struct slcan_dev *dev, const char *cmd)
{
	return write_all(dev, cmd, strlen(cmd), SLCAN_CTRL_TIMEOUT_MS);
}

int slcan_set_bitrate(slcan_dev *dev, uint32_t bitrate)
{
	if (!dev)
		return SLCAN_ERR_INVALID;

	int idx = -1;
	for (size_t i = 0; i < SLCAN_BITRATE_COUNT; i++) {
		if (slcan_bitrate_table[i] == bitrate) {
			idx = (int)i;
			break;
		}
	}
	if (idx < 0)
		return SLCAN_ERR_INVALID;

	/* Mirrors slcan_netdev_open()'s "C\rS%d\r": close first, in case a
	 * previous session left the channel open, then set the rate. */
	char cmd[16];
	snprintf(cmd, sizeof(cmd), "C\rS%d\r", idx);
	return send_cmd(dev, cmd);
}

/* Mirrors slcan_encaps() exactly, including the lower-case-via-0x20 trick
 * for SFF ids. out must be at least SLCAN_LINE_MAX+1 bytes; returns the
 * number of bytes written (excluding the trailing '\r', which IS
 * included in the count). */
static int encode_frame(const slcan_frame *f, char *out)
{
	int eff = (f->can_id & SLCAN_EFF_FLAG) != 0;
	int rtr = (f->can_id & SLCAN_RTR_FLAG) != 0;
	uint32_t id = f->can_id & (eff ? SLCAN_EFF_MASK : SLCAN_SFF_MASK);
	uint8_t len = f->len > SLCAN_MAX_DLEN ? SLCAN_MAX_DLEN : f->len;
	int id_len = eff ? 8 : 3;

	char *pos = out;
	*pos = (char)(rtr ? 'R' : 'T');
	if (!eff)
		*pos = (char)(*pos | 0x20); /* R->r, T->t */
	pos++;

	char *endpos = pos + id_len - 1;
	for (char *p = endpos; p >= pos; p--) {
		*p = hex_upper[id & 0xf];
		id >>= 4;
	}
	pos += id_len;

	*pos++ = (char)('0' + len);

	if (!rtr) {
		for (uint8_t i = 0; i < len; i++) {
			*pos++ = hex_upper[(f->data[i] >> 4) & 0xf];
			*pos++ = hex_upper[f->data[i] & 0xf];
		}
	}
	*pos++ = '\r';
	return (int)(pos - out);
}

int slcan_send(slcan_dev *dev, const slcan_frame *frame, unsigned int timeout_ms)
{
	if (!dev || !frame)
		return SLCAN_ERR_INVALID;
	if (frame->len > SLCAN_MAX_DLEN)
		return SLCAN_ERR_INVALID;

	char buf[SLCAN_LINE_MAX + 1];
	int n = encode_frame(frame, buf);
	return write_all(dev, buf, (size_t)n, timeout_ms ? timeout_ms : SLCAN_CTRL_TIMEOUT_MS);
}

/* --- start/stop --- */

static void *reader_thread_fn(void *arg);

int slcan_start(slcan_dev *dev, int listen_only)
{
	if (!dev)
		return SLCAN_ERR_INVALID;

	pthread_mutex_lock(&dev->start_lock);
	if (dev->reader_running) {
		pthread_mutex_unlock(&dev->start_lock);
		return SLCAN_ERR_BUSY;
	}

	int rc = send_cmd(dev, listen_only ? "L\r" : "O\r");
	if (rc) {
		pthread_mutex_unlock(&dev->start_lock);
		return rc;
	}

	dev->stop_flag = 0;
	if (pthread_create(&dev->reader_thread, NULL, reader_thread_fn, dev) != 0) {
		pthread_mutex_unlock(&dev->start_lock);
		return SLCAN_ERR_IO;
	}
	dev->reader_running = 1;
	pthread_mutex_unlock(&dev->start_lock);
	return 0;
}

int slcan_stop(slcan_dev *dev)
{
	if (!dev)
		return SLCAN_ERR_INVALID;

	pthread_mutex_lock(&dev->start_lock);
	if (!dev->reader_running) {
		pthread_mutex_unlock(&dev->start_lock);
		return 0;
	}
	dev->stop_flag = 1;
	pthread_mutex_unlock(&dev->start_lock);

	pthread_join(dev->reader_thread, NULL);

	pthread_mutex_lock(&dev->start_lock);
	dev->reader_running = 0;
	pthread_mutex_unlock(&dev->start_lock);

	send_cmd(dev, "C\r"); /* best effort */
	return 0;
}

/* --- receive-side decoding --- */

static void rxq_push(struct slcan_dev *dev, const slcan_frame *f)
{
	pthread_mutex_lock(&dev->q_lock);
	if (dev->q_len == dev->q_cap) {
		dev->q_head = (dev->q_head + 1) % dev->q_cap;
		dev->q_len--;
		dev->st_queue_drops++;
	}
	unsigned int tail = (dev->q_head + dev->q_len) % dev->q_cap;
	dev->q_buf[tail] = *f;
	dev->q_len++;
	pthread_cond_signal(&dev->q_cond);
	pthread_mutex_unlock(&dev->q_lock);
	dev->st_rx_frames++;
}

static int hexval(unsigned char c, int *out)
{
	if (c >= '0' && c <= '9') { *out = c - '0'; return 0; }
	if (c >= 'a' && c <= 'f') { *out = c - 'a' + 10; return 0; }
	if (c >= 'A' && c <= 'F') { *out = c - 'A' + 10; return 0; }
	return -1;
}

/* Mirrors slcan_bump_frame(). line[0] is one of 't','r','T','R'. */
static int decode_frame_line(const unsigned char *line, unsigned int len, slcan_frame *f)
{
	int eff = (line[0] == 'T' || line[0] == 'R');
	int rtr = (line[0] == 'r' || line[0] == 'R');
	unsigned int id_len = eff ? 8 : 3;

	if (len < 1 + id_len + 1)
		return -1;

	uint32_t id = 0;
	for (unsigned int i = 0; i < id_len; i++) {
		int v;
		if (hexval(line[1 + i], &v) != 0)
			return -1;
		id = (id << 4) | (uint32_t)v;
	}

	unsigned char dlc_ch = line[1 + id_len];
	if (dlc_ch < '0' || dlc_ch > '8')
		return -1;
	uint8_t dlc = (uint8_t)(dlc_ch - '0');

	memset(f, 0, sizeof(*f));
	f->can_id = id | (uint32_t)(eff ? SLCAN_EFF_FLAG : 0) | (uint32_t)(rtr ? SLCAN_RTR_FLAG : 0);

	if (rtr) {
		/* RTR frames may carry a dlc > 0 but never any data bytes. */
		f->len = dlc;
		return 0;
	}

	unsigned int data_start = 1 + id_len + 1;
	if (len < data_start + (unsigned int)dlc * 2)
		return -1;

	for (uint8_t i = 0; i < dlc; i++) {
		int hi, lo;
		if (hexval(line[data_start + 2 * i], &hi) != 0 ||
		    hexval(line[data_start + 2 * i + 1], &lo) != 0)
			return -1;
		f->data[i] = (uint8_t)((hi << 4) | lo);
	}
	f->len = dlc;
	return 0;
}

/* Mirrors slcan_bump_state(): "s<a|w|p|b><rxcnt:3digit><txcnt:3digit>",
 * e.g. "sb256256" (bus-off, rx=256, tx=256). */
static void decode_state_line(struct slcan_dev *dev, const unsigned char *line, unsigned int len)
{
	if (len != 1 + 3 + 3) /* state-char + 3-digit rx + 3-digit tx (line[0]=='s' already matched) */
		return;

	enum slcan_state state;
	switch (line[1]) {
	case 'a': state = SLCAN_STATE_ERROR_ACTIVE; break;
	case 'w': state = SLCAN_STATE_ERROR_WARNING; break;
	case 'p': state = SLCAN_STATE_ERROR_PASSIVE; break;
	case 'b': state = SLCAN_STATE_BUS_OFF; break;
	default: return;
	}

	uint32_t rxerr = 0, txerr = 0;
	for (int i = 0; i < 3; i++) {
		unsigned char c = line[2 + i];
		if (c < '0' || c > '9')
			return;
		rxerr = rxerr * 10 + (uint32_t)(c - '0');
	}
	for (int i = 0; i < 3; i++) {
		unsigned char c = line[5 + i];
		if (c < '0' || c > '9')
			return;
		txerr = txerr * 10 + (uint32_t)(c - '0');
	}

	dev->state = state;
	dev->rxerr = rxerr;
	dev->txerr = txerr;
	dev->st_state_frames++;
}

/* Mirrors slcan_bump_err(): "e<len_digit><len chars>", e.g. "e1a" (ACK
 * error), "e3bcO" (Bit0, CRC, Tx overrun error). See slcan.h's doc comment
 * on slcan_get_last_error() for why we don't decode these further. */
static void decode_error_line(struct slcan_dev *dev, const unsigned char *line, unsigned int len)
{
	if (len < 2) /* len-digit + at least 1 char (line[0]=='e' already matched) */
		return;
	unsigned char len_ch = line[1];
	if (len_ch < '0' || len_ch > '9')
		return;
	unsigned int elen = (unsigned int)(len_ch - '0');
	if (2 + elen > len)
		return;

	unsigned int n = elen < sizeof(dev->last_error) - 1 ? elen : sizeof(dev->last_error) - 1;
	memcpy(dev->last_error, line + 2, n);
	dev->last_error[n] = '\0';
	dev->st_error_frames++;
}

/* Mirrors slcan_bump(). */
static void handle_line(struct slcan_dev *dev, const unsigned char *line, unsigned int len)
{
	if (len == 0)
		return;

	switch (line[0]) {
	case 't': case 'r': case 'T': case 'R': {
		slcan_frame f;
		if (decode_frame_line(line, len, &f) == 0)
			rxq_push(dev, &f);
		else
			dev->st_decode_errors++;
		break;
	}
	case 's':
		decode_state_line(dev, line, len);
		break;
	case 'e':
		decode_error_line(dev, line, len);
		break;
	default:
		break; /* command ack or unrecognized line, ignored like upstream */
	}
}

/* Mirrors slcan_unesc(): assembles bytes into a line up to '\r' or '\a',
 * with the same overflow-then-discard-until-terminator behavior. */
static void *reader_thread_fn(void *arg)
{
	struct slcan_dev *dev = arg;
	unsigned char buf[256];

	for (;;) {
		pthread_mutex_lock(&dev->start_lock);
		int stop = dev->stop_flag;
		pthread_mutex_unlock(&dev->start_lock);
		if (stop)
			break;

		struct pollfd pfd = { .fd = dev->fd, .events = POLLIN, .revents = 0 };
		int rc = poll(&pfd, 1, SLCAN_READER_POLL_MS);
		if (rc == 0)
			continue; /* idle timeout, re-check stop flag */
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break; /* fd broken */
		}
		if (!(pfd.revents & POLLIN)) {
			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
				break;
			continue;
		}

		ssize_t n = read(dev->fd, buf, sizeof(buf));
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			break; /* EOF or real error: device gone */
		}

		for (ssize_t i = 0; i < n; i++) {
			unsigned char c = buf[i];
			if (c == '\r' || c == '\a') {
				if (!dev->line_error)
					handle_line(dev, dev->line, dev->line_len);
				dev->line_len = 0;
				dev->line_error = 0;
			} else if (!dev->line_error) {
				if (dev->line_len < SLCAN_LINE_MAX) {
					dev->line[dev->line_len++] = c;
				} else {
					dev->line_error = 1;
					dev->st_line_overflows++;
				}
			}
		}
	}

	return NULL;
}

int slcan_recv(slcan_dev *dev, slcan_frame *frame, unsigned int timeout_ms)
{
	if (!dev || !frame)
		return SLCAN_ERR_INVALID;

	pthread_mutex_lock(&dev->q_lock);

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

	while (dev->q_len == 0) {
		if (timeout_ms == 0) {
			pthread_mutex_unlock(&dev->q_lock);
			return 0;
		}
		int rc = pthread_cond_timedwait(&dev->q_cond, &dev->q_lock, &deadline);
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&dev->q_lock);
			return 0;
		}
	}

	*frame = dev->q_buf[dev->q_head];
	dev->q_head = (dev->q_head + 1) % dev->q_cap;
	dev->q_len--;

	pthread_mutex_unlock(&dev->q_lock);
	return 1;
}

/* --- state / diagnostics getters --- */

void slcan_get_state(slcan_dev *dev, enum slcan_state *state, uint32_t *rxerr, uint32_t *txerr)
{
	if (!dev)
		return;
	if (state) *state = dev->state;
	if (rxerr) *rxerr = dev->rxerr;
	if (txerr) *txerr = dev->txerr;
}

void slcan_get_rx_stats(slcan_dev *dev, slcan_rx_stats *out)
{
	if (!dev || !out)
		return;
	out->rx_frames = dev->st_rx_frames;
	out->queue_drops = dev->st_queue_drops;
	out->state_frames = dev->st_state_frames;
	out->error_frames = dev->st_error_frames;
	out->decode_errors = dev->st_decode_errors;
	out->line_overflows = dev->st_line_overflows;
}

void slcan_get_last_error(slcan_dev *dev, char *out, size_t out_len)
{
	if (!out || out_len == 0)
		return;
	if (!dev) {
		out[0] = '\0';
		return;
	}
	snprintf(out, out_len, "%s", dev->last_error);
}
