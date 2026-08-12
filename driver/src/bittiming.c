#include <math.h>
#include <stdint.h>

#include "bittiming.h"

static double default_sample_point(uint32_t bitrate)
{
	if (bitrate > 800000)
		return 0.75;
	if (bitrate > 500000)
		return 0.80;
	return 0.875;
}

int gsusb_calc_bittiming(uint32_t clock_hz, uint32_t bitrate, double sample_point,
			 const gsusb_bittiming_const *btc,
			 uint32_t *prop_seg, uint32_t *phase_seg1,
			 uint32_t *phase_seg2, uint32_t *sjw, uint32_t *brp)
{
	if (!btc || !prop_seg || !phase_seg1 || !phase_seg2 || !sjw || !brp)
		return GSUSB_ERR_INVALID;
	if (clock_hz == 0 || bitrate == 0)
		return GSUSB_ERR_INVALID;
	if (btc->brp_min == 0 || btc->brp_max < btc->brp_min)
		return GSUSB_ERR_INVALID;

	if (sample_point <= 0.0 || sample_point >= 1.0)
		sample_point = default_sample_point(bitrate);

	uint32_t brp_inc = btc->brp_inc ? btc->brp_inc : 1;

	int have_best = 0;
	double best_bitrate_err = 1e300;
	double best_sp_err = 1e300;
	uint32_t best_tseg1 = 0, best_tseg2 = 0, best_brp = 0;

	for (uint32_t brp = btc->brp_min; brp <= btc->brp_max; brp += brp_inc) {
		if (brp == 0)
			continue;

		uint64_t denom = (uint64_t)brp * (uint64_t)bitrate;
		if (denom == 0)
			continue;

		/* total time quanta per bit, including the 1 tq sync segment,
		 * rounded to the nearest integer */
		uint64_t tq64 = ((uint64_t)clock_hz + denom / 2) / denom;
		if (tq64 < 3 || tq64 > 0xffffffffu)
			continue;
		uint32_t tq = (uint32_t)tq64;

		uint32_t tseg_total = tq - 1; /* to be split into tseg1+tseg2 */
		if (tseg_total < btc->tseg1_min + btc->tseg2_min)
			continue;

		double actual_bitrate = (double)clock_hz / ((double)brp * (double)tq);
		double bitrate_err = fabs(actual_bitrate - (double)bitrate) / (double)bitrate;

		/* sample point = (1 + tseg1) / tq -> tseg1 = round(sp*tq) - 1 */
		long tseg1 = lround(sample_point * (double)tq) - 1;
		if (tseg1 < (long)btc->tseg1_min)
			tseg1 = btc->tseg1_min;
		if (tseg1 > (long)btc->tseg1_max)
			tseg1 = btc->tseg1_max;

		long tseg2 = (long)tseg_total - tseg1;
		if (tseg2 < (long)btc->tseg2_min) {
			tseg2 = btc->tseg2_min;
			tseg1 = (long)tseg_total - tseg2;
		} else if (tseg2 > (long)btc->tseg2_max) {
			tseg2 = btc->tseg2_max;
			tseg1 = (long)tseg_total - tseg2;
		}

		if (tseg1 < (long)btc->tseg1_min || tseg1 > (long)btc->tseg1_max)
			continue;
		if (tseg2 < (long)btc->tseg2_min || tseg2 > (long)btc->tseg2_max)
			continue;

		double achieved_sp = (1.0 + (double)tseg1) / (double)tq;
		double sp_err = fabs(achieved_sp - sample_point);

		int better;
		if (!have_best) {
			better = 1;
		} else if (bitrate_err + 1e-12 < best_bitrate_err) {
			better = 1;
		} else if (bitrate_err > best_bitrate_err + 1e-12) {
			better = 0;
		} else {
			better = sp_err < best_sp_err;
		}

		if (better) {
			have_best = 1;
			best_bitrate_err = bitrate_err;
			best_sp_err = sp_err;
			best_tseg1 = (uint32_t)tseg1;
			best_tseg2 = (uint32_t)tseg2;
			best_brp = brp;
		}
	}

	if (!have_best)
		return GSUSB_ERR_NO_BITTIMING_SOLUTION;

	*prop_seg = 0; /* device firmware sums prop_seg+phase_seg1 internally;
			* it only reports/accepts a combined tseg1 range, so
			* the split between the two is arbitrary. */
	*phase_seg1 = best_tseg1;
	*phase_seg2 = best_tseg2;
	*brp = best_brp;

	uint32_t sjw_val = best_tseg2;
	if (sjw_val > btc->sjw_max)
		sjw_val = btc->sjw_max;
	if (sjw_val == 0)
		sjw_val = 1;
	*sjw = sjw_val;

	return 0;
}
