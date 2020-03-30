/*
 * Copyright © 2006-2009 Simon Thum
 * Copyright © 2012 Jonas Ådahl
 * Copyright © 2014-2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "filter.h"
#include "libinput-util.h"
#include "filter-private.h"

/* Once normalized, touchpads see the same acceleration as mice. that is
 * technically correct but subjectively wrong, we expect a touchpad to be a
 * lot slower than a mouse. Apply a magic factor to slow down all movements
 */
#define TP_MAGIC_SLOWDOWN 0.2968 /* unitless factor */

struct touchpad_accelerator {
	struct motion_filter base;

	accel_profile_func_t profile;

	double velocity;	/* units/us */
	double last_velocity;	/* units/us */

	struct pointer_trackers trackers;

	double threshold;	/* units/us */
	double accel;		/* unitless factor */

	int dpi;

	double speed_factor;    /* factor based on speed setting */
};

/**
 * Calculate the acceleration factor for the given delta with the timestamp.
 *
 * @param accel The acceleration filter
 * @param unaccelerated The raw delta in the device's dpi
 * @param data Caller-specific data
 * @param time Current time in µs
 *
 * @return A unitless acceleration factor, to be applied to the delta
 */
static inline double
calculate_acceleration_factor(struct touchpad_accelerator *accel,
			      const struct device_float_coords *unaccelerated,
			      void *data,
			      uint64_t time)
{
	double velocity; /* units/us in device-native dpi*/
	double accel_factor;

	trackers_feed(&accel->trackers, unaccelerated, time);
	velocity = trackers_velocity(&accel->trackers, time);
	accel_factor = calculate_acceleration_simpsons(&accel->base,
						       accel->profile,
						       data,
						       velocity,
						       accel->last_velocity,
						       time);
	accel->last_velocity = velocity;

	return accel_factor;
}

/**
 * Generic filter that calculates the acceleration factor and applies it to
 * the coordinates.
 *
 * @param filter The acceleration filter
 * @param unaccelerated The raw delta in the device's dpi
 * @param data Caller-specific data
 * @param time Current time in µs
 *
 * @return An accelerated tuple of coordinates representing accelerated
 * motion, still in device units.
 */
static struct device_float_coords
accelerator_filter_generic(struct motion_filter *filter,
			   const struct device_float_coords *unaccelerated,
			   void *data, uint64_t time)
{
	struct touchpad_accelerator *accel =
		(struct touchpad_accelerator *) filter;
	double accel_value; /* unitless factor */
	struct device_float_coords accelerated;

	accel_value = calculate_acceleration_factor(accel,
						    unaccelerated,
						    data,
						    time);

	accelerated.x = accel_value * unaccelerated->x;
	accelerated.y = accel_value * unaccelerated->y;

	return accelerated;
}

static struct normalized_coords
accelerator_filter_post_normalized(struct motion_filter *filter,
				   const struct device_float_coords *unaccelerated,
				   void *data, uint64_t time)
{
	struct touchpad_accelerator *accel =
		(struct touchpad_accelerator *) filter;
	struct device_float_coords accelerated;

	/* Accelerate for device units, normalize afterwards */
	accelerated = accelerator_filter_generic(filter,
						 unaccelerated,
						 data,
						 time);
	return normalize_for_dpi(&accelerated, accel->dpi);
}

/* Maps the [-1, 1] speed setting into a constant acceleration
 * range. This isn't a linear scale, we keep 0 as the 'optimized'
 * mid-point and scale down to 0 for setting -1 and up to 5 for
 * setting 1. On the premise that if you want a faster cursor, it
 * doesn't matter as much whether you have 0.56789 or 0.56790,
 * but for lower settings it does because you may lose movements.
 * *shrug*.
 *
 * Magic numbers calculated by MyCurveFit.com, data points were
 *  0.0 0.0
 *  0.1 0.1 (because we need 4 points)
 *  1   1
 *  2   5
 *
 *  This curve fits nicely into the range necessary.
 */
static inline double
speed_factor(double s)
{
	s += 1; /* map to [0, 2] */
	return 435837.2 + (0.04762636 - 435837.2)/(1 + pow(s/240.4549,
							   2.377168));
}

static bool
touchpad_accelerator_set_speed(struct motion_filter *filter,
		      double speed_adjustment)
{
	struct touchpad_accelerator *accel_filter =
		(struct touchpad_accelerator *)filter;

	assert(speed_adjustment >= -1.0 && speed_adjustment <= 1.0);

	filter->speed_adjustment = speed_adjustment;
	accel_filter->speed_factor = speed_factor(speed_adjustment);

	return true;
}

static struct normalized_coords
touchpad_constant_filter(struct motion_filter *filter,
			 const struct device_float_coords *unaccelerated,
			 void *data, uint64_t time)
{
	struct touchpad_accelerator *accel =
		(struct touchpad_accelerator *)filter;
	struct normalized_coords normalized;
	/* We need to use the same baseline here as the accelerated code,
	 * otherwise our unaccelerated speed is different to the accelerated
	 * speed on the plateau.
	 *
	 * This is a hack, the baseline should be incorporated into the
	 * TP_MAGIC_SLOWDOWN so we only have one number here but meanwhile
	 * this will do.
	 */
	const double baseline = 0.9;

	normalized = normalize_for_dpi(unaccelerated, accel->dpi);
	normalized.x = baseline * TP_MAGIC_SLOWDOWN * normalized.x;
	normalized.y = baseline * TP_MAGIC_SLOWDOWN * normalized.y;

	return normalized;
}

static void
touchpad_accelerator_restart(struct motion_filter *filter,
			     void *data,
			     uint64_t time)
{
	struct touchpad_accelerator *accel =
		(struct touchpad_accelerator *) filter;

	trackers_reset(&accel->trackers, time);
}

static void
touchpad_accelerator_destroy(struct motion_filter *filter)
{
	struct touchpad_accelerator *accel =
		(struct touchpad_accelerator *) filter;

	trackers_free(&accel->trackers);
	free(accel);
}

double
touchpad_accel_profile_linear(struct motion_filter *filter,
			      void *data,
			      double speed_in, /* in device units/µs */
			      uint64_t time)
{
	struct touchpad_accelerator *accel_filter =
		(struct touchpad_accelerator *)filter;
	double factor; /* unitless */

	/* Convert to mm/s because that's something one can understand */
	speed_in = v_us2s(speed_in) * 25.4/accel_filter->dpi;

	/*
	   Our acceleration function calculates a factor to accelerate input
	   deltas with. The basic concept is that the acceleration factor should
	   increase monotonically with speed_in, but should not explode with
	   high speed_in value.
	*/
	factor = 0.1 * pow(speed_in, 0.55);

	factor *= accel_filter->speed_factor;
	return factor * TP_MAGIC_SLOWDOWN;
}

struct motion_filter_interface accelerator_interface_touchpad = {
	.type = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE,
	.filter = accelerator_filter_post_normalized,
	.filter_constant = touchpad_constant_filter,
	.restart = touchpad_accelerator_restart,
	.destroy = touchpad_accelerator_destroy,
	.set_speed = touchpad_accelerator_set_speed,
};

struct motion_filter *
create_pointer_accelerator_filter_touchpad(int dpi,
	uint64_t event_delta_smooth_threshold,
	uint64_t event_delta_smooth_value,
	bool use_velocity_averaging)
{
	struct touchpad_accelerator *filter;
	struct pointer_delta_smoothener *smoothener;

	filter = zalloc(sizeof *filter);
	filter->last_velocity = 0.0;

	trackers_init(&filter->trackers, use_velocity_averaging ? 16 : 2);

	filter->threshold = 130;
	filter->dpi = dpi;

	filter->base.interface = &accelerator_interface_touchpad;
	filter->profile = touchpad_accel_profile_linear;

	smoothener = zalloc(sizeof(*smoothener));
	smoothener->threshold = event_delta_smooth_threshold,
	smoothener->value = event_delta_smooth_value,
	filter->trackers.smoothener = smoothener;

	return &filter->base;
}
