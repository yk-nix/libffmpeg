/*
 * media.h
 *
 *  Created on: 2023-02-01 15:20:19
 *      Author: yui
 */

#ifndef INCLUDE_MEDIA_H_
#define INCLUDE_MEDIA_H_

#include <SDL2/SDL.h>

#include <frame.h>
#include <packet.h>

#define MEDIA_FLAG_VIDEO_DECODER_FINISHED         0x0001
#define MEDIA_FLAG_AUDIO_DECODER_FINISHED         0x0002
#define MEDIA_FLAG_SUBTITLE_DECODER_FINISHED      0x0004
#define MEDIA_FLAG_GRABBER_FINISHED               0x0008
#define MEDIA_FLAG_DECODER_FINISHED  (MEDIA_FLAG_VIDEO_DECODER_FINISHED | MEDIA_FLAG_AUDIO_DECODER_FINISHED | MEDIA_FLAG_SUBTITLE_DECODER_FINISHED)
#define MEDIA_FLAG_NO_VIDEO                       0x0100
#define MEDIA_FLAG_NO_AUDIO                       0x0200
#define MEDIA_FLAG_NO_SUBTITLE                    0x0400

typedef struct exAVMedia {
	/* To support multiple medias */
	struct list_head list;

	/* Caches used to decode media */
	struct list *vpackets, *apackets, *spackets, *vframes, *aframes, *sframes;
	pthread_t packet_grabber, video_decoder, audio_decoder, subtitle_decoder;

	int flags;  /* indicators to show media status */

	/* Media informations */
	AVFormatContext *avfmt_ctx;               /* context of the opened media */
	int video_idx, audio_idx, subtitle_idx;   /* video, audio, and subtitle stream index */
	int width, height, pix_fmt;
	AVRational sample_aspect_ratio, video_time_base, audio_time_base, subtitle_time_base;
	uint64_t video_start_time, audio_start_time, subtitle_start_time;
	uint64_t frame_start_time;
	double last_frame_shown;
	int frame_poped;
	int sample_rate, sample_fmt, channel_layout, channels;

	/* Used to show media */
	exAVFrame *frame;
	int default_width, default_height;
	int screen_left, screen_top, screen_width, screen_height;
	enum ShowMode {
		SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
	} show_mode;
	int paused, force_refresh;
#define REFRESH_RATE 0.01            /* unit: second */
#define CURSOR_HIDE_DELAY 1000000    /* hide cursor after 1 second with there is no any operations */
	int cursor_hidden;
	int64_t cursor_last_shown;
	int flip, is_last_frame;
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	struct SwsContext *img_convert_ctx;

	/* Operations */
	void (*play)(struct exAVMedia *self);   /* this function must be called in the main thread. */
} exAVMedia;

static inline int ex_av_media_decoder_is_finished(exAVMedia *m) {
	return (m->flags & MEDIA_FLAG_VIDEO_DECODER_FINISHED) &&
			   (m->flags & MEDIA_FLAG_AUDIO_DECODER_FINISHED) &&
				 (m->flags & MEDIA_FLAG_SUBTITLE_DECODER_FINISHED);
}

static inline void ex_av_media_caches_free(exAVMedia *m) {
	if (m->vpackets)
		m->vpackets->ops->put(m->vpackets, ex_av_packet_free_list_entry);
	if (m->apackets)
		m->apackets->ops->put(m->apackets, ex_av_packet_free_list_entry);
	if (m->spackets)
		m->spackets->ops->put(m->spackets, ex_av_packet_free_list_entry);
	if (m->vframes)
		m->vframes->ops->put(m->vframes, ex_av_frame_free_list_entry);
	if (m->aframes)
		m->aframes->ops->put(m->aframes, ex_av_packet_free_list_entry);
	if (m->sframes)
		m->sframes->ops->put(m->sframes, ex_av_packet_free_list_entry);
	m->vpackets = NULL;
	m->apackets = NULL;
	m->spackets = NULL;
	m->vframes = NULL;
	m->aframes = NULL;
	m->sframes = NULL;
}

static inline int ex_av_media_caches_init(exAVMedia *m) {
	int ret = -1;
	m->vpackets = LIST.new(MUTEX, LIST_DEFAULT_MAX_SIZE);
	m->apackets = LIST.new(MUTEX, LIST_DEFAULT_MAX_SIZE);
	m->spackets = LIST.new(MUTEX, LIST_DEFAULT_MAX_SIZE);
	m->vframes  = LIST.new(MUTEX, LIST_DEFAULT_MAX_SIZE);
	m->aframes  = LIST.new(MUTEX, LIST_DEFAULT_MAX_SIZE);
	m->sframes  = LIST.new(MUTEX, LIST_DEFAULT_MAX_SIZE);
	if (!m->vpackets || !m->apackets || !m->spackets || !m->vframes || !m->aframes || !m->sframes) {
		av_log(NULL, AV_LOG_ERROR, "unable to create caches: no memory\n");
		goto err;
	}
	return 0;
err:
	ex_av_media_caches_free(m);
	return ret;
}


#define MEDIA_OPEN_VIDEO_ONLY          (MEDIA_FLAG_NO_AUDIO|MEDIA_FLAG_NO_SUBTITLE)
#define MEDIA_OPEN_AUDIO_ONLY          (MEDIA_FLAG_NO_VIDEO|MEDIA_FLAG_NO_SUBTITLE)
#define MEDIA_OPEN_SUBTILE_ONLY        (MEDIA_FLAG_NO_VIDEO|MEIDA_FLAG_NO_AUDIO)
#define MEDIA_OPEN_NO_VIDEO            MEDIA_FLAG_NO_VIDEO
#define MEDIA_OPEN_NO_AUDIO            MEDIA_FLAG_NO_AUDIO
#define MEDIA_OPEN_NO_SUBTITLE         MEDIA_FLAG_NO_SUBTITLE

/*
 * Same as 'ex_av_media_open' but no packet-grabber and stream-decoders will be created.
 * You must free the returned media via 'ex_av_media_close'.
 * return NULL, if anything wrong.
 */
extern exAVMedia *ex_av_url_open(const char *url, int flags);

/*
 * Open a file located by 'url', and create threads dedicated to grab packets and to decode streams.
 * If failed to create any threads, the corresponding variable of type of 'phread_t' will be set as 0.
 * You must free the returned media via 'ex_av_media_close'.
 * Return NULL, if anything wrong.
 */
extern exAVMedia *ex_av_media_open(const char *url, int flags);

/*
 * Close an opened media. Free it and all its contents
 * and set *media to NULL.
 */
extern void ex_av_media_close(exAVMedia **media);

#endif /* INCLUDE_MEDIA_H_ */
