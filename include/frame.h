/*
 * frame.h
 *
 *  Created on: 2022-12-06 15:14:04
 *      Author: yui
 */

#ifndef SRC_UTILS_FRAME_H_
#define SRC_UTILS_FRAME_H_

#include <pthread.h>

#include <libavutil/frame.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include <list.h>
#include <atomic.h>

/*
 * Check if pixel format 'format' is the supported format for a 'codec'.
 */
static inline int avcodec_is_supported_pix_format(const AVCodec *codec, const enum AVPixelFormat format) {
	if (format == AV_PIX_FMT_NONE)
		return 0;
	for (int i = 0; codec->pix_fmts[i] != -1; i++) {
		if(codec->pix_fmts[i] == format)
			return 1;
	}
	return 0;
}

/*
 * Check if sample format 'format' is the supported format for a 'codec'.
 */
static inline int avcodec_is_supported_sample_format(const AVCodec *codec, const enum AVSampleFormat format) {
	if (format == AV_SAMPLE_FMT_NONE)
		return 0;
	for (int i = 0; codec->sample_fmts[i] != -1; i++) {
		if(codec->sample_fmts[i] == format)
			return 1;
	}
	return 0;
}

/*
 * Return a frame which will contain the picture specified by 'url';
 * the first frame will be returned if the picture is a .gif picture.
 * Return NULL if failed.
 */
extern AVFrame *av_frame_load_picture(const char *url);

/*
 * Save a picture-frame as a picture file located by 'url'(including file extension).
 * Return 0 success, otherwise, return a negative code.
 */
extern int av_frame_save(AVFrame *frame, const char *url);

/*
 * Convert a picture-frame into specified width, height and pixel-format.
 * Return 0 success, otherwise, return a negative code.
 */
extern int av_frame_convert(AVFrame *frame, int width, int height, int format);

/*
 * Resize o picture-frame into specified width and height.
 * Return 0 success, otherwise, return a negative code.
 */
extern int av_frame_resize(AVFrame *frame, int width, int height);

/*
 * Convert picture-frame into specified pixel format.
 * Return 0 success, otherwise, return a negative code.
 */
extern int av_frame_convert_pix_format(AVFrame *frame, int format);

/*
 * Show a picture-frame. (the window title is fixed as 'frame-displayer')
 */
extern void av_frame_show(AVFrame *frame);


/*
 *  AVFrame wrapper. You must use 'ex_av_frame_alloc' to create a new frame,
 *  and call its 'put' function to free it when it is not used any more.
 */
typedef struct exAVFrame {
	AVFrame *avframe;
	struct list_head list;
	atomic_t refcount;
	pthread_rwlock_t rwlock;

	struct exAVFrame *(*get)(struct exAVFrame *self);
	void (*put)(struct exAVFrame *self);
	int (*save)(struct exAVFrame *self, const char *url);
	int (*resize)(struct exAVFrame *self, int width, int height);
	int (*convert)(struct exAVFrame *self, int pix_format);
	void (*show)(struct exAVFrame *self);
} exAVFrame;

/*
 * Allocate a frame with specified size. The 'size' must be greater than the
 * value of 'sizeof(exAVFrame)', otherwise, the size will be set as it.
 * Return NULL if failed, otherwise, return the newly allocated frame.
 */
extern exAVFrame *ex_av_frame_alloc(size_t size);

/*
 * Same as 'av_frame_load_picture', but return frame of type of 'exAVFrame'.
 */
exAVFrame *ex_av_frame_load_picture(const char *url);

/*
 * Delete the frame from the list, and do 'put' operation on this frame.
 */
extern void ex_av_frame_free_list_entry(struct list_head *);

#endif /* SRC_UTILS_FRAME_H_ */
