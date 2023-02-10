/*
 * packet.h
 *
 *  Created on: 2023-02-01 15:02:57
 *      Author: yui
 */

#ifndef INCLUDE_PACKET_H_
#define INCLUDE_PACKET_H_

#include <pthread.h>

#include <libavcodec/packet.h>

/*
 *  AVPacket wrapper. You must use 'ex_av_packet_alloc' to create a new packet,
 *  and call its 'put' function to free it when it is not used any more.
 */
typedef struct exAVPacket {
	AVPacket *avpkt;
	struct list_head list;
	pthread_rwlock_t rwlock;
	atomic_t refcount;

	int serial;

	struct exAVPacket *(*get)(struct exAVPacket *self);
	void (*put)(struct exAVPacket *self);
} exAVPacket;

/*
 * Allocate a packet.
 * Return NULL if failed, otherwise, return the newly allocated packet.
 */
extern exAVPacket *ex_av_packet_alloc(void);

/*
 * Delete the packet from the list, and do 'put' operation on this packet.
 */
extern void ex_av_packet_free_list_entry(struct list_head *n);

#endif /* INCLUDE_PACKET_H_ */
