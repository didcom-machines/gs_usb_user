/* common.h - small shared helpers for the gsusb CLI tools. Not part of the
 * public library API. */
#ifndef GSUSB_TOOLS_COMMON_H
#define GSUSB_TOOLS_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/gsusb.h"

struct gsusb_known_id {
	uint16_t vid, pid;
	const char *name;
};

static const struct gsusb_known_id gsusb_known_ids[] = {
	{ GSUSB_VID_GS_USB_1,        GSUSB_PID_GS_USB_1,        "gs_usb (original GS USB firmware)" },
	{ GSUSB_VID_CANDLELIGHT,     GSUSB_PID_CANDLELIGHT,     "candleLight" },
	{ GSUSB_VID_CES_CANEXT_FD,   GSUSB_PID_CES_CANEXT_FD,   "CES CANext FD" },
	{ GSUSB_VID_ABE_CANDEBUGGER, GSUSB_PID_ABE_CANDEBUGGER, "ABE canDebugger FD" },
	{ GSUSB_VID_XYLANTA_SAINT3,  GSUSB_PID_XYLANTA_SAINT3,  "Xylanta Saint3" },
	{ GSUSB_VID_CANNECTIVITY,    GSUSB_PID_CANNECTIVITY,    "CANnectivity" },
};
#define GSUSB_KNOWN_ID_COUNT (sizeof(gsusb_known_ids) / sizeof(gsusb_known_ids[0]))

/* Opens vid:pid if both are non-zero, otherwise tries every known id in
 * turn. bus/addr may be -1 to match any. Prints an error and exits the
 * process on failure -- meant for these small CLI tools, not library use. */
static inline gsusb_dev *gsusb_tool_open(void *ctx, int vid, int pid, int bus, int addr)
{
	char err[256];

	if (vid > 0 && pid > 0) {
		gsusb_dev *d = gsusb_open(ctx, (uint16_t)vid, (uint16_t)pid, bus, addr, err, sizeof(err));
		if (!d) {
			fprintf(stderr, "gsusb: %s\n", err);
			exit(1);
		}
		return d;
	}

	for (size_t i = 0; i < GSUSB_KNOWN_ID_COUNT; i++) {
		gsusb_dev *d = gsusb_open(ctx, gsusb_known_ids[i].vid, gsusb_known_ids[i].pid,
					  bus, addr, err, sizeof(err));
		if (d)
			return d;
	}

	fprintf(stderr, "gsusb: no supported device found "
			"(use --vid/--pid to target a specific device; see gsusb_info --scan)\n");
	exit(1);
}

static inline int gsusb_parse_hex_u16(const char *s, uint16_t *out)
{
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 16);
	if (!s[0] || (end && *end) || v > 0xffff)
		return -1;
	*out = (uint16_t)v;
	return 0;
}

/* Parses a cansend(1)-style CAN id, e.g. "123", "1FFFFFFF" (>0x7FF implies
 * extended unless forced by the caller), or with an 'R' data field for RTR
 * frames handled by the caller. Just the numeric id here. */
static inline int gsusb_parse_hex_u32(const char *s, uint32_t *out)
{
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 16);
	if (!s[0] || (end && *end))
		return -1;
	*out = (uint32_t)v;
	return 0;
}

#endif /* GSUSB_TOOLS_COMMON_H */
