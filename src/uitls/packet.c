/*
 * packet.c
 *
 *  Created on: 2023-02-01 15:02:49
 *      Author: yui
 */

#include <stdlib.h>

#include <list.h>

#include <packet.h>

static exAVPacket *get(exAVPacket *self) {
	if (atomic_inc_and_test(&self->refcount)) {
		/* the packet is being released */
		atomic_dec(&self->refcount);
		return NULL;
	}
	return self;
}

static void put(exAVPacket *self) {
	if (atomic_dec_and_test(&self->refcount)) {
		if (pthread_rwlock_trywrlock(&self->rwlock))
			return; /* there is someone else has 'get' this packet, so let that caller free the packet */
		atomic_dec(&self->refcount);
		list_del(&self->list);
		av_packet_free(&self->avpkt);
		pthread_rwlock_unlock(&self->rwlock);
		pthread_rwlock_destroy(&self->rwlock);
		free(self);
	}
}

static void ex_av_packet_ops_init(exAVPacket *p) {
	p->get = get;
	p->put = put;
}

static int ex_av_packet_init(exAVPacket *p) {
	int ret = -1;
	INIT_LIST_HEAD(&p->list);
	atomic_set(&p->refcount, 1);
	ex_av_packet_ops_init(p);
	pthread_rwlockattr_t rwlockattr;
	if(pthread_rwlockattr_init(&rwlockattr))
		return ret;
	ret = pthread_rwlock_init(&p->rwlock, &rwlockattr);
	pthread_rwlockattr_destroy(&rwlockattr);
	return ret;
}

exAVPacket *ex_av_packet_alloc(size_t size) {
	exAVPacket *p = NULL;
	if (size < sizeof(exAVPacket))
		size = sizeof(exAVPacket);
	p = calloc(1, size);
	if (p == NULL)
		goto err0;
	p->avpkt = av_packet_alloc();
	if(p->avpkt == NULL)
		goto err1;
	ex_av_packet_init(p);
	return p;
err1:
	free(p);
err0:
	return NULL;
}


void ex_av_packet_free_list_entry(struct list_head *n) {
	exAVPacket *self = list_entry(n, exAVPacket, list);
	put(self);
}
