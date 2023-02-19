/*
 * hash.c
 *
 *  Created on: 2023-02-01 14:49:51
 *      Author: yui
 */

#include <libavformat/avio.h>
#include <libavutil/hash.h>
#include <libavutil/mem.h>

int av_hash_file(const char *hash_type, const char *url, uint8_t **out) {
	int ret = 0, hash_size = 0;
	uint8_t buf[4096] = { 0 }, *dst = NULL;
	AVIOContext *avio_ctx = NULL;
	struct AVHashContext *hash_ctx = NULL;
	ret = avio_open(&avio_ctx, url, AVIO_FLAG_READ);
	if(ret < 0)
		goto end;
	ret = av_hash_alloc(&hash_ctx, hash_type);
	if(ret < 0)
		goto err;
	av_hash_init(hash_ctx);
	hash_size = av_hash_get_size(hash_ctx);
	if(*out == NULL) {
		dst = av_malloc(hash_size);
		if(dst == NULL)
			goto err;
	}
	else {
		dst = *out;
	}
	while(!avio_feof(avio_ctx)) {
		ret = avio_read(avio_ctx, buf, sizeof(buf) - 1);
		if(ret < 0) {
			if(*out == NULL)
				av_freep(dst);
			goto err;
		}
		av_hash_update(hash_ctx, buf, ret);
		memset(buf, 0, ret);
	}
	av_hash_final(hash_ctx, dst);
	ret = hash_size;
	*out = dst;
err:
	avio_close(avio_ctx);
	avio_ctx = NULL;
end:
	av_hash_freep(&hash_ctx);
	av_freep(&avio_ctx);
	return ret;
}

int av_hash_msg(const char *hash_type, const char *msg, size_t len, uint8_t **out) {
	int ret = 0, hash_size = 0;
	struct AVHashContext *hash_ctx = NULL;
	uint8_t *dst = NULL;
	ret = av_hash_alloc(&hash_ctx, hash_type);
	if(ret < 0)
		goto end;
	av_hash_init(hash_ctx);
	hash_size = av_hash_get_size(hash_ctx);
	if(*out == NULL) {
		dst = av_malloc(hash_size);
		if(dst == NULL)
			goto end;
	}
	else {
		dst = *out;
	}
	av_hash_update(hash_ctx, (const uint8_t *)msg, len);
	av_hash_final(hash_ctx, dst);
	ret = hash_size;
	*out = dst;
end:
	av_hash_freep(&hash_ctx);
	return ret;
}
