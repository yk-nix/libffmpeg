/*
 * media.h
 *
 *  Created on: 2023-02-01 15:20:19
 *      Author: yui
 */

#ifndef INCLUDE_MEDIA_H_
#define INCLUDE_MEDIA_H_

#include <ffmpeg_config.h>
#include <frame.h>
#include <packet.h>
#include <clock.h>

enum {
		AV_SYNC_AUDIO_MASTER,   /* default choice */
		AV_SYNC_VIDEO_MASTER,
		AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define MEDIA_FLAG_VIDEO_DECODER_FINISHED         0x0001
#define MEDIA_FLAG_AUDIO_DECODER_FINISHED         0x0002
#define MEDIA_FLAG_SUBTITLE_DECODER_FINISHED      0x0004
#define MEDIA_FLAG_GRABBER_FINISHED               0x0008
#define MEDIA_FLAG_DECODER_FINISHED  (MEDIA_FLAG_VIDEO_DECODER_FINISHED | MEDIA_FLAG_AUDIO_DECODER_FINISHED | MEDIA_FLAG_SUBTITLE_DECODER_FINISHED)
#define MEDIA_FLAG_NO_VIDEO                       0x0100
#define MEDIA_FLAG_NO_AUDIO                       0x0200
#define MEDIA_FLAG_NO_SUBTITLE                    0x0400

typedef struct exAVPacketQueue {
 struct list *list;
 int serial;
} exAVPacketQueue;

typedef struct exAVFrameQueue {
	struct list *list;
	exAVFrame *last, *next;
	int serial;
} exAVFrameQueue;

typedef struct exAudioParams {
    int sample_rate;
    int channels;
    enum AVSampleFormat sample_fmt;
    int frame_size;
    int64_t channel_layout;
    int bytes_per_sec;
} exAudioParams;

typedef struct exAVMedia {
	struct list_head list;
	atomic_t refcount;
	pthread_rwlock_t rwlock;
	const char *url;

	/* Operations */
	void (*play)(struct exAVMedia *self);
	void (*stop)(struct exAVMedia *self);
	void (*start_play)(struct exAVMedia *self);             /* this function must be called in the main thread. */
	void (*pause_play)(struct exAVMedia *self);             /* pause decoding */
	void (*stop_play)(struct exAVMedia *self);
	void (*start_decode)(struct exAVMedia *self);
	void (*stop_decode)(struct exAVMedia *self);
	struct exAVMedia *(*get)(struct exAVMedia *self);
	void (*put)(struct exAVMedia *self);
	int (*open)(struct exAVMedia *self, const char *url, int open_flags);     /* open the 'url' media file */
	void (*close)(struct exAVMedia *self);                                    /* close the media file */

	/* Caches */
#define PACKET_QUEUE_SIZE  128
#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBTITLE_PICTURE_QUEUE_SIZE 16
#define AUDIO_SAMPLE_QUEUE_SIZE 64
	exAVPacketQueue vpackets, apackets, spackets;
	exAVFrameQueue vframes, aframes, sframes;

	/* Point to the opened media file */
	AVFormatContext *ic;

	/* Flags */
	int decode_started, play_started, play_paused;

	/* Indexes of those streams opened; valid >= 0  and  invalid < 0 */
	int video_idx, audio_idx, subtitle_idx;

	/* Threads for decoding this opened media */
	pthread_t packet_grabber, video_decoder, audio_decoder, subtitle_decoder;

	/* Clocks */
	double video_clock, audio_clock, external_clock;
	exAVClock vidclk, audclk, extclk;
	int audio_clock_serial;

	/* for playing media */
#if HAVE_SDL2
	enum ShowMode {
		SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
	} show_mode;
# define CURSOR_HIDE_DELAY 1000000    /* hide cursor after 1 second with there is no any operations */
# define AUDIO_BUFFER_SIZE  192000
	int play_flags;
	int window_default_width, window_default_height;
	int screen_left, screen_top, screen_width, screen_height;
	int flip;
	int av_sync_type;
	double refresh_rate;
	int cursor_hidden;
	int volume;
	int64_t cursor_last_shown;


	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;

	int muted;
	int16_t sample_array[SAMPLE_ARRAY_SIZE];
	int sample_array_index;
	SDL_AudioDeviceID audio_dev;
	SDL_AudioSpec audio_spec;
	int audio_dev_buf_size;
	int64_t audio_callback_time;
	int audio_callback_timeout_count;      /* callback_timeout_value = one_callback_time_length /  callback_timeout_count */
	exAudioParams audio_dev_params, audio_frame_params;
	uint8_t *audio_buf;                    /* point to audio-frame data which has been re-sampled */
	uint8_t *audio_cache;                  /* used to re-sample audio frame, should be freed via av_freep when it is no longer used. */
	size_t audio_buf_size, audio_buf_write_size, audio_buf_index, audio_cache_size;
#endif

	/* Context used to convert picture frame */
	struct SwsContext *sws_ctx;

	/* Context used to convert audio frame */
	struct SwrContext *swr_ctx;

	/* Video Stream informations */
	int video_width, video_height, video_pix_fmt;
	AVRational video_sar, video_time_base;
	uint64_t video_start_time;

	/* Audio Stream informations */
	int audio_sample_rate, audio_sample_fmt, audio_channels, audio_frame_size;
	uint64_t audio_channel_layout;
	AVRational audio_time_base;
	uint64_t audio_start_time;

	/* Subtitle Stream informations */
	uint64_t subtitle_start_time;

} exAVMedia;

static inline int ex_av_media_packet_grabber_stopped(exAVMedia *m) {
	return !!pthread_kill(m->packet_grabber, 0);
}

static inline int ex_av_media_video_decoder_stopped(exAVMedia *m) {
	return !!pthread_kill(m->video_decoder, 0);
}

static inline int ex_av_media_audio_decoder_stopped(exAVMedia *m) {
	return !!pthread_kill(m->audio_decoder, 0);
}

static inline int ex_av_media_subtitle_decoder_stopped(exAVMedia *m) {
	return !!pthread_kill(m->subtitle_decoder, 0);
}


#define MEDIA_OPEN_VIDEO_ONLY          (MEDIA_FLAG_NO_AUDIO|MEDIA_FLAG_NO_SUBTITLE)
#define MEDIA_OPEN_AUDIO_ONLY          (MEDIA_FLAG_NO_VIDEO|MEDIA_FLAG_NO_SUBTITLE)
#define MEDIA_OPEN_SUBTILE_ONLY        (MEDIA_FLAG_NO_VIDEO|MEIDA_FLAG_NO_AUDIO)
#define MEDIA_OPEN_NO_VIDEO            MEDIA_FLAG_NO_VIDEO
#define MEDIA_OPEN_NO_AUDIO            MEDIA_FLAG_NO_AUDIO
#define MEDIA_OPEN_NO_SUBTITLE         MEDIA_FLAG_NO_SUBTITLE
/*
 * Open a file located by 'url'.
 * You must free the returned media via its 'put' function.
 * Return NULL, if anything wrong.
 */
extern exAVMedia *ex_av_media_open(const char *url, int flags);

/*
 * Allocate a media.
 * Return NULL if failed, otherwise, return the newly allocated media.
 */
extern exAVMedia *ex_av_media_alloc(void);


#endif /* INCLUDE_MEDIA_H_ */
