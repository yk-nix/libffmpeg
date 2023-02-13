/*
 * clock.c
 *
 *  Created on: 2023-02-06 14:49:58
 *      Author: yui
 */

#include <math.h>

#include <libavutil/time.h>

#include <clock.h>

double ex_av_clock_get(exAVClock *c) {
	if (c->paused) {
		return c->pts;
	}
	else {
		double now = av_gettime_relative() / 1000000.0;
		return c->pts_drift + now - (now - c->last_updated) * (1.0 - c->speed);
	}
}

void ex_av_clock_set_at(exAVClock *c, double pts, int serial, double last_updated) {
	c->pts = pts;
	c->last_updated = last_updated;
	c->pts_drift = c->pts - last_updated;
	c->serial = serial;
}

void ex_av_clock_set(exAVClock *c, double pts, int serial) {
	double now = av_gettime_relative() / 1000000.0;
	ex_av_clock_set_at(c, pts, serial, now);
}

void ex_av_clock_set_speed(exAVClock *c, double speed) {
	ex_av_clock_set(c, ex_av_clock_get(c), c->serial);
	c->speed = speed;
}

void ex_av_clock_init(exAVClock *c) {
	c->speed = 1.0;
	c->paused = 0;
	ex_av_clock_set(c, NAN, -1);
}

void ex_av_clock_sync_to_slave(exAVClock *c, exAVClock *slave) {
	double clock = ex_av_clock_get(c);
	double slave_clock = ex_av_clock_get(slave);
	if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
		ex_av_clock_set(c, slave_clock, slave->serial);
}

