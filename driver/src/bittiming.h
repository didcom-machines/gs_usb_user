/* bittiming.h - CAN bit-timing register calculator.
 *
 * This is NOT a port of the kernel's can_calc_bittiming(); it is an
 * independent, straightforward brute-force search written so its
 * correctness can be checked directly against the CAN bit-timing spec
 * (ISO 11898-1) rather than trusted from memory of kernel internals.
 */
#ifndef GSUSB_BITTIMING_H
#define GSUSB_BITTIMING_H

#include <stdint.h>
#include "../include/gsusb.h"

/* Searches brp/tseg1/tseg2 combinations allowed by btc that produce
 * `bitrate` bps from a `clock_hz` CAN clock, picking the combination with
 * the smallest bitrate error, then (as a tiebreak) the sample point closest
 * to `sample_point` (0 selects a CiA-recommended default based on bitrate:
 * 0.75 above 800kbit/s, 0.8 above 500kbit/s, else 0.875).
 *
 * On success writes prop_seg (always 0 - the combined tseg1 budget is put
 * entirely into phase_seg1, since the device only reports/accepts a
 * combined tseg1_min/max and sums the two fields internally), phase_seg1,
 * phase_seg2, sjw and brp, and returns 0. Returns GSUSB_ERR_NO_BITTIMING_SOLUTION
 * if no combination satisfies btc's constraints, or GSUSB_ERR_INVALID for
 * bad arguments.
 */
int gsusb_calc_bittiming(uint32_t clock_hz, uint32_t bitrate, double sample_point,
			 const gsusb_bittiming_const *btc,
			 uint32_t *prop_seg, uint32_t *phase_seg1,
			 uint32_t *phase_seg2, uint32_t *sjw, uint32_t *brp);

#endif /* GSUSB_BITTIMING_H */
