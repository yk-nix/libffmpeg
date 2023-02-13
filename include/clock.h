/*
 * clock.h
 *
 *  Created on: 2023-02-06 14:49:50
 *      Author: yui
 */

#ifndef INCLUDE_CLOCK_H_
#define INCLUDE_CLOCK_H_

#define AV_NOSYNC_THRESHOLD 10.0

typedef struct exAVClock {
	double pts;           /* clock base */
	double pts_drift;     /* clock base minus time at which we updated the clock */
	double last_updated;
	double speed;
	int serial;           /* clock is based on a packet with this serial */
	int paused;
} exAVClock;

double ex_av_clock_get(exAVClock *c);
void ex_av_clock_set_at(exAVClock *c, double pts, int serial, double last_updated);
void ex_av_clock_set(exAVClock *c, double pts, int serial);
void ex_av_clock_init(exAVClock *c);
void ex_av_clock_sync_to_slave(exAVClock *c, exAVClock *slave);

#endif /* INCLUDE_CLOCK_H_ */
