/*
 * media.c
 *
 *  Created on: 2023-02-01 15:20:08
 *      Author: yui
 */

#include <pthread.h>

#include <libavutil/time.h>
#include <libavutil/common.h>
#include <libswresample/swresample.h>
#include <libavutil/error.h>
#include <libavutil/avstring.h>

#define EVENT_HANDLER_RESULT_EXIT   1
#define EVENT_HANDLER_RESULT_OK     0
#define EVENT_HANDLER_REUSLT_ERROR -1

#include <media.h>

typedef struct exFFFrame {
	exAVFrame frame;
	AVSubtitle sub;
	int serial;
	double pts;                        /* presentation timestamp for the frame */
	double duration;                   /* estimated duration of the frame */
	int64_t pos;                       /* byte position of the frame in the input file */
	int width;
	int height;
	int format;
	AVRational sar;
	int uploaded;
	int flip_v;
} exFFFrame;

typedef struct exFFPacket {
	exAVPacket packet;
	int serial;
} exFFPacket;

#if HAVE_SDL2
static exFFFrame *to_exffframe(exAVMedia *m, exAVFrame *f, enum AVMediaType type) {
	exFFFrame *ff = (exFFFrame *)f;
	ff->serial = m->vframes.serial;
	ff->pos = f->avframe->pkt_pos;
	ff->height = f->avframe->height;
	ff->width = f->avframe->width;
	if (type == AVMEDIA_TYPE_VIDEO) {
		ff->duration = (m->video_frame_rate.num && m->video_frame_rate.den ? av_q2d((AVRational){m->video_frame_rate.den, m->video_frame_rate.num}) : 0);
		ff->pts = (f->avframe->pts == AV_NOPTS_VALUE) ? NAN : f->avframe->pts * av_q2d(m->video_time_base);
	}
	else if (type == AVMEDIA_TYPE_AUDIO) {
		ff->duration = av_q2d((AVRational){f->avframe->nb_samples, f->avframe->sample_rate});
		ff->pts = (f->avframe->pts == AV_NOPTS_VALUE) ? NAN : f->avframe->pts * av_q2d(m->audio_time_base);
	}
	ff->format = f->avframe->format;
	ff->sar = f->avframe->sample_aspect_ratio;
	return ff;
}

static exAVFrame *_ex_av_frame_queue_peek(exAVFrameQueue *q, int idx) {
	exAVFrame *f = NULL;
	struct list_head *n = q->list->peek(q->list, idx);
	if (n) {
		f = list_entry(n, exAVFrame, list);
	}
	return f;
}

static inline exAVFrame *ex_av_frame_queue_peek(exAVFrameQueue *q) {
	return _ex_av_frame_queue_peek(q, 0);
}

//static inline exAVFrame *ex_av_frame_queue_peek_next(exAVFrameQueue *q) {
//	return _ex_av_frame_queue_peek(q, 1);
//}

static exAVFrame *ex_av_frame_queue_pop(exAVFrameQueue *q) {
	exAVFrame *f = NULL;
	struct list_head *n = q->list->pop_front(q->list);
	if (n)
		f = list_entry(n, exAVFrame, list);
	return f;
}

extern int sdl_update_texture(SDL_Renderer *render, SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx);

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop,
																	 int scr_width, int scr_height,
                                   int pic_width, int pic_height,
																	 AVRational pic_sar) {
	AVRational aspect_ratio = pic_sar;
	int64_t width, height, x, y;

	if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
		aspect_ratio = av_make_q(1, 1);

	aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

	/* XXX: we suppose the screen has a 1.0 pixel ratio */
	height = scr_height;
	width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
	if (width > scr_width) {
		width = scr_width;
		height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
	}
	x = (scr_width - width) / 2;
	y = (scr_height - height) / 2;
	rect->x = scr_xleft + x;
	rect->y = scr_ytop  + y;
	rect->w = FFMAX((int)width,  1);
	rect->h = FFMAX((int)height, 1);
}

static void set_default_window_size(exAVMedia *media) {
	SDL_Rect rect;
	int max_width  = media->screen_width  ? media->screen_width  : INT_MAX;
	int max_height = media->screen_height ? media->screen_height : INT_MAX;
	if (max_width == INT_MAX && max_height == INT_MAX)
		max_height = media->video_height;
	calculate_display_rect(&rect, 0, 0, max_width, max_height, media->video_width, media->video_height, media->video_sar);
	media->screen_width  = media->window_default_width  = rect.w;
	media->screen_height = media->window_default_height = rect.h;
}

static void video_image_display(exAVMedia *m, exAVFrame *f) {
	SDL_Rect rect;
	calculate_display_rect(&rect,
													0, 0,
													m->screen_width, m->screen_height,
													m->video_width, m->video_height,
													m->video_sar);
	if (f)
		sdl_update_texture(m->renderer, &m->texture, f->avframe, &m->sws_ctx);
	SDL_ShowWindow(m->window);
	SDL_SetRenderDrawColor(m->renderer, 0, 0, 0, 255);
	SDL_RenderClear(m->renderer);
	SDL_RenderCopyEx(m->renderer, m->texture, NULL, &rect, 0, NULL, m->flip ? SDL_FLIP_VERTICAL : 0);
	SDL_RenderPresent(m->renderer);
}

static int get_master_sync_type(exAVMedia *m) {
	if (m->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		if (m->video_idx >= 0)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTER;
	}
	else if (m->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		if (m->audio_idx >= 0)
			return AV_SYNC_AUDIO_MASTER;
		else
			return AV_SYNC_EXTERNAL_CLOCK;
	}
	else {
			return AV_SYNC_EXTERNAL_CLOCK;
	}
}

double get_master_clock(exAVMedia *m) {
	double val;
	switch (get_master_sync_type(m)) {
	case AV_SYNC_VIDEO_MASTER:
		val = ex_av_clock_get(&m->video_avclock);
		break;
	case AV_SYNC_AUDIO_MASTER:
		val = ex_av_clock_get(&m->audio_avclock);
		break;
	default:
		val = ex_av_clock_get(&m->external_avclock);
		break;
	}
	return val;
}

static double frame_duration(exAVMedia *m, exFFFrame *last, exFFFrame *cur) {
	if (last && last->serial != cur->serial)
		return 0.0;
  double duration = cur->pts - (last ? last->pts : 0.0);
	if (last && (isnan(duration) || duration <= 0 || duration > m->max_frame_duration))
		return last->duration;
	else
		return duration;
}

static double compute_target_delay(double delay, exAVMedia *m) {
	double sync_threshold, diff = 0;

	/* update delay to follow master synchronization source */
	if (get_master_sync_type(m) != AV_SYNC_VIDEO_MASTER) {
		/* if video is slave, we try to correct big delays by
			 duplicating or deleting a frame */
		diff = ex_av_clock_get(&m->video_avclock) - get_master_clock(m);

		/* skip or repeat frame. We take into account the
			 delay to compute the threshold. I still don't know
			 if it is the best guess */
		sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < m->max_frame_duration) {
			if (diff <= -sync_threshold)
					delay = FFMAX(0, delay + diff);
			else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
					delay = delay + diff;
			else if (diff >= sync_threshold)
					delay = 2 * delay;
		}
	}
	av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",	delay, -diff);

	return delay;
}

static void update_video_avclock(exAVMedia *m, double pts, int64_t pos, int serial) {
	ex_av_clock_set(&m->video_avclock, pts, serial);
	pthread_rwlock_wrlock(&m->rwlock);
	ex_av_clock_sync_to_slave(&m->external_avclock, &m->video_avclock);
	pthread_rwlock_unlock(&m->rwlock);
}

static void video_refresh(exAVMedia *m, double *remaining_time) {
	exAVFrame *f = NULL;
	exFFFrame *ff = NULL, *last = NULL;;
	double now, last_duration, delay;

	if (m->paused)
		goto refresh;

retry:
	if (m->vframes.list->size(m->vframes.list) == 0)
		goto refresh;
	f = ex_av_frame_queue_peek(&m->vframes);
	ff = to_exffframe(m, f, AVMEDIA_TYPE_VIDEO);
	if (m->vframes.serial != ff->serial) {
		f = ex_av_frame_queue_pop(&m->vframes);
		f->put(f);
		goto retry;
	}
	last = (exFFFrame *)m->vframes.last;
	if (last && last->serial != ff->serial)
		m->frame_timer = av_gettime_relative() / 1000000.0;
	last_duration = frame_duration(m, last, ff);
	delay = compute_target_delay(last_duration, m);
	now = av_gettime_relative()/1000000.0;
	if (now < m->frame_timer + delay) {
		*remaining_time = FFMIN(m->frame_timer + delay - now, *remaining_time);
		goto refresh;
	}
	m->frame_timer += delay;
	if (delay > 0 && now - m->frame_timer > AV_SYNC_THRESHOLD_MAX)
		m->frame_timer = now;
	if (!isnan(ff->pts))
		update_video_avclock(m, ff->pts, ff->pos, ff->serial);
	if (m->vframes.last)
		m->vframes.last->put(m->vframes.last);
	m->vframes.last = ex_av_frame_queue_pop(&m->vframes);

refresh:
  video_image_display(m, m->vframes.last);
}

static void refresh_loop_wait_event(exAVMedia *media, SDL_Event *event) {
	double remaining_time = 0.0;
	SDL_PumpEvents();
	while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
		if (!media->cursor_hidden && av_gettime_relative() - media->cursor_last_shown > CURSOR_HIDE_DELAY) {
			SDL_ShowCursor(0);
			media->cursor_hidden = 1;
		}
		if (remaining_time > 0.0)
			av_usleep((int64_t)(remaining_time * 1000000.0));
		remaining_time = media->refresh_rate;
		if (media->show_mode != SHOW_MODE_NONE && (!media->paused || media->force_refresh))
			video_refresh(media, &remaining_time);
		SDL_PumpEvents();
	}
}

static void toggle_full_screen(exAVMedia *m) {
    m->is_full_screen = !m->is_full_screen;
    SDL_SetWindowFullscreen(m->window, m->is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_pause(exAVMedia *m) {
	if (m->paused) {
		m->frame_timer += av_gettime_relative() / 1000000.0 - m->video_avclock.last_updated;
		ex_av_clock_set(&m->video_avclock, ex_av_clock_get(&m->video_avclock), m->video_avclock.serial);
  }
	ex_av_clock_set(&m->external_avclock, ex_av_clock_get(&m->external_avclock), m->external_avclock.serial);
  m->paused = m->audio_avclock.paused = m->video_avclock.paused = m->external_avclock.paused = !m->paused;
}

static void update_volume(exAVMedia *m, int sign, double step) {
	double volume_level = m->volume ? (20 * log(m->volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
	int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
	m->volume = av_clip(m->volume == new_volume ? (m->volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
}

static void update_start_time(exAVMedia *m, int is_add) {
	if (is_add)
		m->start_time = get_master_clock(m) + m->seek_step;
	else
		m->start_time = get_master_clock(m) - m->seek_step;
	m->seek_requested = 1;
}

static int key_event_handler(exAVMedia *m, SDL_Event *e) {
	switch (e->key.keysym.sym) {
	case SDLK_SPACE:
		toggle_pause(m);
		break;
	case SDLK_KP_MULTIPLY:
		update_volume(m, 1, SDL_VOLUME_STEP);
		break;
	case SDLK_KP_DIVIDE:
		update_volume(m, -1, SDL_VOLUME_STEP);
		break;
	case SDLK_LEFT:
		update_start_time(m, 0);
		break;
	case SDLK_RIGHT:
		update_start_time(m, 1);
		break;
	default:
		break;
	}
	return EVENT_HANDLER_RESULT_OK;
}

static int mouse_event_handler(exAVMedia *m, SDL_Event *e) {
	if (e->type == SDL_MOUSEBUTTONDOWN) {
		if (e->button.button == SDL_BUTTON_LEFT) {
				static int64_t last_mouse_left_click = 0;
				if (av_gettime_relative() - last_mouse_left_click <= 500000) {
					toggle_full_screen(m);
					m->force_refresh = 1;
					last_mouse_left_click = 0;
				}
				else {
					last_mouse_left_click = av_gettime_relative();
				}
			}
	}
	if (m->cursor_hidden) {
			SDL_ShowCursor(1);
			m->cursor_hidden = 0;
	}
	m->cursor_last_shown = av_gettime_relative();
	return EVENT_HANDLER_RESULT_OK;
}

static void window_event_resize(exAVMedia *m, SDL_Event *e) {
	if(e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
		m->screen_width  = e->window.data1;
		m->screen_height = e->window.data2;
		if (m->texture) {
			SDL_DestroyTexture(m->texture);
			m->texture = NULL;
		}
	}
}

static int window_event_handler(exAVMedia *m, SDL_Event *e) {
	fflush(stdout);
	switch (e->window.event) {
	case SDL_WINDOWEVENT_SIZE_CHANGED:
	case SDL_WINDOWEVENT_EXPOSED:
		window_event_resize(m, e);
		m->force_refresh = 1;
		break;
	default:
		break;
	}
	return EVENT_HANDLER_RESULT_OK;
}

static int quit_event_handler(exAVMedia *m, SDL_Event *e) {
	return EVENT_HANDLER_RESULT_EXIT;
}

static int event_handlers(exAVMedia *m, SDL_Event *e) {
	int ret = 0;
	switch (e->type) {
		case SDL_QUIT:
			ret = quit_event_handler(m, e);
			break;
		case SDL_KEYDOWN:
			ret = key_event_handler(m, e);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEMOTION:
			ret = mouse_event_handler(m, e);
			break;
		case SDL_WINDOWEVENT:
			ret = window_event_handler(m, e);
			break;
		default:
			break;
		}
	return ret;
}

static void event_loop(exAVMedia *m) {
	SDL_Event event;
	do {
		refresh_loop_wait_event(m, &event);
	} while(!event_handlers(m, &event));
}

static int synchronize_audio(exAVMedia *m, int nb_samples) {
	int wanted_nb_samples = nb_samples;

//	/* if not master, then we try to remove or add samples to correct the clock */
//	if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
//			double diff, avg_diff;
//			int min_nb_samples, max_nb_samples;
//
//			diff = get_clock(&is->audclk) - get_master_clock(is);
//
//			if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
//					is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
//					if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
//							/* not enough measures to have a correct estimate */
//							is->audio_diff_avg_count++;
//					} else {
//							/* estimate the A-V difference */
//							avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
//
//							if (fabs(avg_diff) >= is->audio_diff_threshold) {
//									wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
//									min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
//									max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
//									wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
//							}
//							av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
//											diff, avg_diff, wanted_nb_samples - nb_samples,
//											is->audio_clock, is->audio_diff_threshold);
//					}
//			} else {
//					/* too big difference : may be initial PTS errors, so
//						 reset A-V filter */
//					is->audio_diff_avg_count = 0;
//					is->audio_diff_cum       = 0;
//			}
//	}

	return wanted_nb_samples;
}

static void update_audio_avclock(exAVMedia *m) {
  /* We assume the audio driver that is used by SDL has two periods. */
  if (!isnan(m->audio_clock)) {
		ex_av_clock_set_at(&m->audio_avclock,
								       m->audio_clock - (double)(2 * m->audio_dev_buf_size + m->audio_buf_write_size) / m->audio_dev_params.bytes_per_sec,
								       m->audio_clock_serial,
								       m->audio_callback_time / 1000000.0);
		pthread_rwlock_wrlock(&m->rwlock);
		ex_av_clock_sync_to_slave(&m->external_avclock, &m->audio_avclock);
		pthread_rwlock_unlock(&m->rwlock);
  }
}

static void update_sample_display(exAVMedia *m, short *samples, int samples_size) {
	int size, len;
	size = samples_size / sizeof(short);
	while (size > 0) {
		len = SAMPLE_ARRAY_SIZE - m->sample_array_index;
		if (len > size)
			len = size;
		memcpy(m->sample_array + m->sample_array_index, samples, len * sizeof(short));
		samples += len;
		m->sample_array_index += len;
		if (m->sample_array_index >= SAMPLE_ARRAY_SIZE)
			m->sample_array_index = 0;
		size -= len;
	}
}

/*
 * Return non-zero value if audio_callback is timeout waiting for new audio-frames.
 */
static inline int audio_callback_wait_is_timeout(exAVMedia *m) {
	uint64_t timeout_value = m->audio_dev_buf_size / m->audio_dev_params.bytes_per_sec / m->audio_callback_timeout_count;
	return av_gettime_relative() - m->audio_callback_time > timeout_value;
}

static int _audio_convert_frame(exAVMedia *m, exAVFrame *f, int wanted_nb_samples) {
	int len = 0, ret = 0;
	const uint8_t **in = (const uint8_t **)f->avframe->extended_data;
	uint8_t **out = &m->audio_cache;
	int out_count = (int64_t)wanted_nb_samples * m->audio_dev_params.sample_rate / f->avframe->sample_rate + 256;
	int out_size  = av_samples_get_buffer_size(NULL, m->audio_dev_params.channels, out_count, m->audio_dev_params.sample_fmt, 0);
	if (out_size < 0) {
		av_log(NULL, AV_LOG_ERROR, "audio_convert_frame: av_samples_get_buffer_size error: %s\n", av_err2str(out_size));
		return -1;
	}
	if (wanted_nb_samples != f->avframe->nb_samples) {
		if ((ret = swr_set_compensation(m->swr_ctx,
				                            (wanted_nb_samples - f->avframe->nb_samples) * m->audio_dev_params.sample_rate / f->avframe->sample_rate,
														        wanted_nb_samples * m->audio_dev_params.sample_rate / f->avframe->sample_rate) < 0)) {
			av_log(NULL, AV_LOG_ERROR, "audio_convert_frame: swr_set_compensation error: %s\n", av_err2str(ret));
			return -1;
		}
	}
	av_fast_malloc(&m->audio_cache, (unsigned int *)&m->audio_cache_size, out_size);
	if (!m->audio_cache)
		return AVERROR(ENOMEM);
	len = swr_convert(m->swr_ctx, out, out_count, in, f->avframe->nb_samples);
	if (len < 0) {
		av_log(NULL, AV_LOG_ERROR, "audio_convert_frame: swr_convert error: %s\n", av_err2str(ret));
		return -1;
	}
	if (len == out_count) {
		av_log(NULL, AV_LOG_WARNING, "audio_convert_frame: audio buffer is probably too small\n");
		if (swr_init(m->swr_ctx) < 0)
			swr_free(&m->swr_ctx);
	}
	m->audio_buf = m->audio_cache;
	return len * m->audio_dev_params.channels * av_get_bytes_per_sample(m->audio_dev_params.sample_fmt);
}

static int audio_convert_frame(exAVMedia *m, exAVFrame *f) {
	int64_t channel_layout;
	int wanted_nb_samples = synchronize_audio(m, f->avframe->nb_samples);

	if (f->avframe->channel_layout) {
		if (f->avframe->channels == av_get_channel_layout_nb_channels(f->avframe->channel_layout))
			channel_layout = f->avframe->channel_layout;
		else
			channel_layout = av_get_default_channel_layout(f->avframe->channels);
	}
	/* If the frame changed, we would reconstruct a new swr_ctx to fit the new frame */
	if (channel_layout          != m->audio_frame_params.channel_layout ||
			f->avframe->format      != m->audio_frame_params.sample_fmt     ||
			f->avframe->sample_rate != m->audio_frame_params.sample_rate    ||
			(f->avframe->nb_samples != wanted_nb_samples && !m->swr_ctx)) {
		swr_free(&m->swr_ctx);
		m->swr_ctx = swr_alloc_set_opts(NULL,
																	  m->audio_dev_params.channel_layout,
																		m->audio_dev_params.sample_fmt,
																		m->audio_dev_params.sample_rate,
																		channel_layout,
																		f->avframe->format,
																		f->avframe->sample_rate,
																		0,
																		NULL);
		if (!m->swr_ctx || swr_init(m->swr_ctx) < 0) {
			av_log(NULL,
					   AV_LOG_ERROR,
						 "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
						 f->avframe->sample_rate,
						 av_get_sample_fmt_name(f->avframe->format),
						 f->avframe->channels,
						 m->audio_dev_params.sample_rate,
						 av_get_sample_fmt_name(m->audio_dev_params.sample_fmt),
						 m->audio_dev_params.channels);
			swr_free(&m->swr_ctx);
			return -1;
		}
		/* update the m->audio_frame_params, which recorded the status of the newly created swr_ctx */
		m->audio_frame_params.channel_layout = channel_layout;
		m->audio_frame_params.channels       = f->avframe->channels;
		m->audio_frame_params.sample_fmt     = f->avframe->format;
		m->audio_frame_params.sample_rate    = f->avframe->sample_rate;
	}

	/* There is no necessary to do conversion if m->swr_ctx is NULL */
	if (m->swr_ctx) {
		return _audio_convert_frame(m, f, wanted_nb_samples);
	}
	m->audio_buf = f->avframe->data[0];
	return av_samples_get_buffer_size(NULL, f->avframe->channels, f->avframe->nb_samples, f->avframe->format, 1);
}

/*
 * Convert an audio-frame to be the playable format which is supported by the audio-device.
 * Return the size in bytes of the audio-frame after successful conversion, otherwise,
 * return a negative value.
 */
static int audio_decode_frame(exAVMedia *m) {
	int resampled_data_size;
	exAVFrame *f = NULL;
	exFFFrame *ff = NULL;
	struct list_head *n = NULL;

	if (m->paused)
		return -1;

  /* get a valid frame */
	do {
retry: /* try to get a frame */
		n = m->aframes.list->pop_front(m->aframes.list);
		if (n == NULL) {
			if (audio_callback_wait_is_timeout(m))
				return -1;
			av_usleep(1000);
			goto retry;
		}
		f = list_entry(n, exAVFrame, list);
		ff = to_exffframe(m, f, AVMEDIA_TYPE_AUDIO);
		if (m->aframes.last) {
			m->aframes.last->put(m->aframes.last);
			m->aframes.last = f;
			f->get(f);
		}
	} while (ff->serial != m->aframes.serial);

	/* convert the frame to fit the opened audio-device */
	if ((resampled_data_size = audio_convert_frame(m, f)) < 0)
		return -1;

	/* update the audio clock with the PTS */
	if (!isnan(ff->pts))
		m->audio_clock = ff->pts + (double) f->avframe->nb_samples / f->avframe->sample_rate;
	else
		m->audio_clock = NAN;
	m->audio_clock_serial = ff->serial;
	f->put(f);
	return resampled_data_size;
}

static void audio_callback(void *opaque, Uint8 *stream, int len) {
  exAVMedia *m = opaque;
  int audio_size, bytes;

  /* update callback_time */
  m->audio_callback_time = av_gettime_relative();

  while (len > 0) {
		if (m->audio_buf_index >= m->audio_buf_size) {
		 audio_size = audio_decode_frame(m);
		 if (audio_size < 0) {
				/* if error, just output silence */
			 m->audio_buf = NULL;
			 m->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / m->audio_dev_params.frame_size * m->audio_dev_params.frame_size;
		 }
		 else {
			 if (m->show_mode != SHOW_MODE_VIDEO)
				 update_sample_display(m, (int16_t *)m->audio_buf, audio_size);
			 m->audio_buf_size = audio_size;
		 }
		 m->audio_buf_index = 0;
		}
		bytes = m->audio_buf_size - m->audio_buf_index;
		if (bytes > len)
			bytes = len;
		if (!m->muted && m->audio_buf && m->volume == SDL_MIX_MAXVOLUME)
			memcpy(stream, (uint8_t *)m->audio_buf + m->audio_buf_index, bytes);
		else {
			memset(stream, 0, bytes);
			if (!m->muted && m->audio_buf)
				SDL_MixAudioFormat(stream, (uint8_t *)m->audio_buf + m->audio_buf_index, AUDIO_S16SYS, bytes, m->volume);
		}
		len -= bytes;
		stream += bytes;
		m->audio_buf_index += bytes;
  }
  m->audio_buf_write_size = m->audio_buf_size - m->audio_buf_index;
  update_audio_avclock(m);
}

static void audio_spec_init(exAVMedia *m) {
	const char *env;
	env = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (env) {
		m->audio_channels = atoi(env);
		m->audio_channel_layout = av_get_default_channel_layout(m->audio_channels);
	}
	if (!m->audio_channel_layout || m->audio_channels != av_get_channel_layout_nb_channels(m->audio_channel_layout)) {
		m->audio_channel_layout = av_get_default_channel_layout(m->audio_channels);
		m->audio_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
	}
	m->audio_channels = av_get_channel_layout_nb_channels(m->audio_channel_layout);
	m->audio_spec.channels = m->audio_channels;
	m->audio_spec.freq = m->audio_sample_rate;
	m->audio_spec.format = AUDIO_S16SYS;
	m->audio_spec.silence = 0;
	m->audio_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(m->audio_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
	m->audio_spec.userdata = m;
	m->audio_spec.callback = audio_callback;
	m->audio_dev_params.channel_layout = m->audio_channel_layout;
}

static int audio_open(exAVMedia *m) {
	int ret = -1;
	SDL_AudioSpec spec = { 0 };
	static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
	static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
	int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
	while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= m->audio_spec.freq)
		next_sample_rate_idx--;
	while (!(m->audio_dev = SDL_OpenAudioDevice(NULL, 0, &m->audio_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
		av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
				m->audio_spec.channels, m->audio_spec.freq, SDL_GetError());
		m->audio_spec.channels = next_nb_channels[FFMIN(7, m->audio_spec.channels)];
		if (!m->audio_spec.channels) {
			m->audio_spec.freq = next_sample_rates[next_sample_rate_idx--];
			m->audio_spec.channels = m->audio_channels;
			if (!m->audio_spec.freq) {
				av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio device open failed\n");
				goto err;
			}
		}
		m->audio_dev_params.channel_layout = av_get_default_channel_layout(m->audio_spec.channels);
	}
	if (spec.format != AUDIO_S16SYS) {
		av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
		goto err;
	}
	if (spec.channels != m->audio_spec.channels) {
		m->audio_dev_params.channel_layout = av_get_default_channel_layout(spec.channels);
		if (!m->audio_dev_params.channel_layout) {
			av_log(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
			goto err;
		}
	}
	m->audio_dev_params.sample_fmt = AV_SAMPLE_FMT_S16;
	m->audio_dev_params.sample_rate = spec.freq;
	m->audio_dev_params.channels =  spec.channels;
	m->audio_dev_params.frame_size = av_samples_get_buffer_size(NULL, m->audio_dev_params.channels, 1, m->audio_dev_params.sample_fmt, 1);
	m->audio_dev_params.bytes_per_sec = av_samples_get_buffer_size(NULL, m->audio_dev_params.channels, m->audio_dev_params.sample_rate, m->audio_dev_params.sample_fmt, 1);
	if (m->audio_dev_params.bytes_per_sec <= 0 || m->audio_dev_params.frame_size <= 0) {
		av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		goto err;
	}
	m->audio_dev_buf_size = spec.size;
	m->audio_frame_params = m->audio_dev_params;
	return 0;
err:
	return ret;
}

static void ex_av_media_prepare_play_audio(exAVMedia *m) {
	m->volume = 100;
	m->audio_callback_timeout_count = 2;
	ex_av_clock_set(&m->audio_avclock, 0, 0);
	m->audio_clock = ex_av_clock_get(&m->audio_avclock);
	audio_spec_init(m);
}

static void ex_av_media_start_play_audio(exAVMedia *m) {
	audio_spec_init(m);
	if (audio_open(m))
		return;
	/*
	 * If the video stream exists, transfer the control to video-player
	 */
	SDL_PauseAudioDevice(m->audio_dev, 0);
	if (m->video_idx >= 0)
		return;
	while (!ex_av_media_audio_decoder_stopped(m)) {
		SDL_Delay(1);
	}
	SDL_Delay(1000);
}

static void ex_av_media_prepare_stop_audio(exAVMedia *m) {
	av_freep(&m->audio_cache);
	m->audio_cache_size = 0;
}

static void ex_av_media_stop_play_audio(exAVMedia *m) {
	if (m->audio_dev > 0)
		SDL_CloseAudioDevice(m->audio_dev);
}

static void ex_av_media_prepare_play_video(exAVMedia *m) {
	m->flip = 0;
	m->refresh_rate = 0.01;
	m->cursor_hidden = 0;
	m->window_default_width = 640;
	m->window_default_height = 480;
	m->screen_left = SDL_WINDOWPOS_CENTERED;
	m->screen_top = SDL_WINDOWPOS_CENTERED;
	set_default_window_size(m);

	m->window = SDL_CreateWindow("meida-player",
															 m->screen_left, m->screen_top,
															 m->window_default_width, m->window_default_height,
															 SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
	m->renderer = SDL_CreateRenderer(m->window, -1, 0);
	m->frame_timer = av_gettime_relative() / 1000000.0;
	ex_av_clock_set(&m->video_avclock, 0, 0);
	m->video_clock = ex_av_clock_get(&m->video_avclock);
	m->max_frame_duration = (m->ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
}

static void ex_av_media_start_play_video(exAVMedia *m) {
	if (!m->window) {
		av_log(NULL, AV_LOG_ERROR, "ex_av_media_start_play_video error: failed to create SDL_Window.\n");
		return;
	}
	if (!m->renderer) {
		av_log(NULL, AV_LOG_ERROR, "ex_av_media_start_play_video error: failed to create SDL_Renderer.\n");
		return;
	}
	event_loop(m);
}

static void ex_av_media_prepare_stop_video(exAVMedia *m) {
	if (m->window) {
		SDL_DestroyWindow(m->window);
		m->window = NULL;
	}
	if (m->renderer) {
		SDL_DestroyRenderer(m->renderer);
		m->renderer = NULL;
	}
	if (m->texture) {
		SDL_DestroyTexture(m->texture);
		m->texture = NULL;
	}
	if (m->sws_ctx) {
		sws_freeContext(m->sws_ctx);
		m->sws_ctx = NULL;
	}
}

static void ex_av_media_stop_play_video(exAVMedia *m) {

}

static void ex_av_media_stop_play(exAVMedia *m) {
	if (m->audio_idx >= 0) {
		ex_av_media_prepare_stop_audio(m);
		ex_av_media_stop_play_audio(m);
	}
	if (m->video_idx >= 0) {
		ex_av_media_prepare_stop_video(m);
		ex_av_media_stop_play_video(m);
	}
	m->play_started = 0;
	SDL_Quit();
}

static void ex_av_media_start_play(exAVMedia *m) {
	if (m->audio_idx >= 0)
		m->play_flags |= SDL_INIT_AUDIO;
	if (m->video_idx >= 0)
		m->play_flags |= SDL_INIT_VIDEO;
	m->play_flags |= SDL_INIT_TIMER;
	SDL_Init(m->play_flags);
	m->play_started = 1;
	if (m->audio_idx >= 0) {
		ex_av_media_prepare_play_audio(m);
		ex_av_media_start_play_audio(m);
	}
	if (m->video_idx >= 0) {
		ex_av_media_prepare_play_video(m);
		ex_av_media_start_play_video(m);
	}
}
#else
static void ex_av_media_start_play(exAVMedia *m) {}
static void ex_av_media_stop_play(exAVMedia *m) {}
#endif

static inline int skip_packet(exAVPacket *pkt, struct list *list) {
	pkt->put(pkt);
	return 0;
}

static inline int insert_packet(exAVPacket *pkt, struct list *list) {
	while (list->insert_tail(list, &pkt->list)) {
		av_usleep(100000); /* the list maybe full, then wait a bit */
	}
	return 0;
}

static int do_seek(exAVMedia *m) {
	int ret = 0;
	int64_t seek_target = av_clip64(m->start_time * AV_TIME_BASE, 0, INT64_MAX);
	int64_t seek_min    = m->seek_rel > 0 ? seek_target - m->seek_rel + 2: INT64_MIN;
	int64_t seek_max    = m->seek_rel < 0 ? seek_target - m->seek_rel - 2: INT64_MAX;
	if ((ret = avformat_seek_file(m->ic, -1, seek_min, seek_target, seek_max, m->seek_flags)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "avformat_seek_file error: %s\n", av_err2str(ret));
		return ret;
	}
	else {
		if (m->audio_idx >= 0) {
			m->apackets.list->clear(m->apackets.list, ex_av_packet_free_list_entry);
			m->apackets.serial++;
			m->aframes.list->clear(m->aframes.list, ex_av_frame_free_list_entry);
			m->aframes.serial++;
		}
		if (m->subtitle_idx >= 0) {
			m->spackets.list->clear(m->spackets.list, ex_av_packet_free_list_entry);
			m->spackets.serial++;
			m->sframes.list->clear(m->sframes.list, ex_av_frame_free_list_entry);
			m->sframes.serial++;
		}
		if (m->video_idx >= 0) {
			m->vpackets.list->clear(m->vpackets.list, ex_av_packet_free_list_entry);
			m->vpackets.serial++;
			m->vframes.list->clear(m->vframes.list, ex_av_frame_free_list_entry);
			m->vframes.serial++;
		}
		if (m->seek_flags & AVSEEK_FLAG_BYTE) {
			ex_av_clock_set(&m->external_avclock, NAN, 0);
		}
		else {
			ex_av_clock_set(&m->external_avclock, seek_target / (double)AV_TIME_BASE, 0);
		}
	}
	m->seek_requested = 0;
	return 0;
}

/*
 * Grab a packet and insert it into the list if success.
 */
static int grab_packet(exAVMedia *m) {
	int ret = -1;
	struct list *pkt_list = NULL;

	if (m->seek_requested)
		if (do_seek(m))
			return -1;

	exAVPacket *pkt = ex_av_packet_alloc(sizeof(exFFPacket));
	exFFPacket *ffpkt = (exFFPacket *)pkt;
	if (pkt == NULL) {
		av_log(NULL, AV_LOG_FATAL, "grab_packet error: unable to create packet: no memory\n");
		return AVERROR(ENOMEM);
	}
	ret = av_read_frame(m->ic, pkt->avpkt);
	if (ret == 0) {
		if (pkt->avpkt->stream_index == m->video_idx) {
			ffpkt->serial = m->vpackets.serial;
			pkt_list = m->vpackets.list;
		}
		else if (pkt->avpkt->stream_index == m->audio_idx) {
			ffpkt->serial = m->apackets.serial;
			pkt_list = m->apackets.list;
		}
		else if (pkt->avpkt->stream_index == m->subtitle_idx) {
			ffpkt->serial = m->spackets.serial;
			pkt_list = m->spackets.list;
		}
		if (pkt_list)
			ret = insert_packet(pkt, pkt_list);
		else
			ret = skip_packet(pkt, pkt_list);
		return ret;
	}
	pkt->put(pkt);
	return ret;
}

static void run_grabber_routine(exAVMedia *m) {
	while (!grab_packet(m));
}

/*
 * Packet-grabber routine
 */
static void *packet_grabber(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_grabber_routine(m);
	return NULL;
}

/*
 * Insert a frame into the list after successfully decoding.
 */
static int grab_frame(AVCodecContext *ic, exAVFrameQueue *q) {
	int ret = -1;
	exAVFrame *f = ex_av_frame_alloc(sizeof(exFFFrame));
	if (f == NULL) {
		av_log(NULL, AV_LOG_FATAL, "grab_frame error: unable to allocate frame: no memory\n");
		goto err0;
	}
	ret = avcodec_receive_frame(ic, f->avframe);
	if (ret == 0) { /* successfully received a frame, insert it into the list */
retry:
		if (q->list->insert_tail(q->list, &f->list)) {
			av_usleep(1000000);
			goto retry;
		}
	}
	else { /* failed to received a frame, then release the memory */
		f->put(f);
	}
err0:
	return ret;
}

static int decode(AVCodecContext *codec_ctx, exAVPacket *pkt, exAVFrameQueue *q) {
	int ret = AVERROR(EAGAIN);
	while (ret == AVERROR(EAGAIN)) {
		ret = avcodec_send_packet(codec_ctx, pkt->avpkt);
		if (ret == 0 || ret == AVERROR(EAGAIN))
			grab_frame(codec_ctx, q);
	}
	return ret;
}

static inline int get_stream_idx(exAVMedia *m, enum AVMediaType type) {
	if (type == AVMEDIA_TYPE_VIDEO && m->video_idx >= 0)
		return m->video_idx;
	if (type == AVMEDIA_TYPE_AUDIO && m->audio_idx >= 0)
		return m->audio_idx;
	if (type == AVMEDIA_TYPE_SUBTITLE && m->subtitle_idx >= 0)
		return m->subtitle_idx;
	return -1;
}

static void run_decode_routine(exAVMedia *m, enum AVMediaType type) {
	int ret = -1;
	AVCodec *codec = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVStream *stream = NULL;
	struct list_head *n = NULL;
	struct list *pkt_list = NULL;
	exAVFrameQueue *q = NULL;
	/*
	 * if there is no such stream of type 'type', then it's unnecessary to create
	 * its corresponding decoder
	 */
	int stream_idx = get_stream_idx(m, type);
	if (stream_idx < 0)
		return;
	switch (type) {
	case AVMEDIA_TYPE_VIDEO:
		pkt_list = m->vpackets.list; q = &m->vframes; break;
	case AVMEDIA_TYPE_AUDIO:
		pkt_list = m->apackets.list; q = &m->aframes; break;
	case AVMEDIA_TYPE_SUBTITLE:
		pkt_list = m->spackets.list; q = &m->sframes; break;
	default: break;
	}
	stream = m->ic->streams[stream_idx];
	codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (codec == NULL) {
		av_log(NULL, AV_LOG_ERROR, "decode_routine(%s) error: no such avcodec for %s\n", av_get_media_type_string(type), avcodec_get_name(stream->codecpar->codec_id));
		goto err0;
	}
	codec_ctx = avcodec_alloc_context3(codec);
	if (codec_ctx == NULL) {
		av_log(NULL, AV_LOG_ERROR, "decode_routine(%s) error: unable to allocate avcodec_context for %s\n", av_get_media_type_string(type), avcodec_get_name(stream->codecpar->codec_id));
		goto err0;
	}
	codec_ctx->pkt_timebase = stream->time_base;      // to fix the warning: Could not update timestamps for skipped samples
	if ((ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "decode_routine(%s) error: fill avcodec context: %s\n", av_get_media_type_string(type), av_err2str(ret));
		goto err1;
	}
	if ((ret = avcodec_open2(codec_ctx, codec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "decode_routine(%s) error: unable to open avcodec: %s\n", av_get_media_type_string(type), av_err2str(ret));
		goto err1;
	}
	while (1) {
		n = pkt_list->pop_front(pkt_list);
		if (n == NULL) {
			if(ex_av_media_packet_grabber_stopped(m))
				goto err2;
			/* the list maybe empty, then wait a bit moment */
			av_usleep(500000);
			continue;
		}
		exAVPacket *pkt = list_entry(n, exAVPacket, list);
		ret = decode(codec_ctx, pkt, q);
		pkt->put(pkt);
		if(ret != 0) /* fatal error on decoding, then exit this thread */
			break;
	}
err2:
	avcodec_close(codec_ctx);
err1:
	avcodec_free_context(&codec_ctx);
err0:
	return;
}

/*
 * video-decoder routine
 */
static void *video_decoder(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_decode_routine(m, AVMEDIA_TYPE_VIDEO);
	return NULL;
}

/*
 * audio-decoder routine
 */
static void *audio_decoder(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_decode_routine(m, AVMEDIA_TYPE_AUDIO);
	return NULL;
}

/*
 * subtitle-decoder routine
 */
static void *subtitle_decoder(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_decode_routine(m, AVMEDIA_TYPE_SUBTITLE);
	return NULL;
}

static void ex_av_media_free_packet_queue(exAVPacketQueue *q) {
	q->serial = -1;
	if (q->list) {
		q->list->put(q->list, ex_av_packet_free_list_entry);
		q->list = NULL;
	}
}

static void ex_av_media_free_frame_queue(exAVFrameQueue *q) {
	q->serial = -1;
	if (q->list) {
		q->list->put(q->list, ex_av_frame_free_list_entry);
		q->list = NULL;
	}
	if (q->last) {
		q->last->put(q->last);
		q->last = NULL;
	}
}

static void ex_av_media_free_caches(exAVMedia *m) {
	ex_av_media_free_packet_queue(&m->vpackets);
	ex_av_media_free_packet_queue(&m->apackets);
	ex_av_media_free_packet_queue(&m->spackets);
	ex_av_media_free_frame_queue(&m->vframes);
	ex_av_media_free_frame_queue(&m->aframes);
	ex_av_media_free_frame_queue(&m->sframes);
}

static void ex_av_media_stop_decode(exAVMedia *m) {
	if (!ex_av_media_packet_grabber_stopped(m))
		pthread_cancel(m->packet_grabber);
	if (!ex_av_media_video_decoder_stopped(m))
		pthread_cancel(m->video_decoder);
	if (!ex_av_media_audio_decoder_stopped(m))
		pthread_cancel(m->audio_decoder);
	if (!ex_av_media_subtitle_decoder_stopped(m))
		pthread_cancel(m->subtitle_decoder);
	/*
	 * Wait for the decoder or grabber to exit, if it exists.
	 */
	pthread_join(m->packet_grabber, NULL);
	pthread_join(m->video_decoder, NULL);
	pthread_join(m->audio_decoder, NULL);
	pthread_join(m->subtitle_decoder, NULL);
	m->decode_started = 0;
}

static void ex_av_media_start_decode(exAVMedia *m) {
	/* The media is not yet opened */
	if (m->ic == NULL)
		return;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr))
		return;
	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
		return;

	/* Start up packet-grabber and decoders */
	pthread_create(&m->packet_grabber, &attr, packet_grabber, m);
	if (m->video_idx >= 0)
		pthread_create(&m->video_decoder, &attr, video_decoder, m);
	if (m->audio_idx >= 0)
		pthread_create(&m->audio_decoder, &attr, audio_decoder, m);
	if (m->subtitle_idx >= 0)
		pthread_create(&m->subtitle_decoder, &attr, subtitle_decoder, m);
	m->decode_started = 1;
}

static void ex_av_media_find_stream_index(exAVMedia *m, int open_flags) {
	if (!(open_flags & MEDIA_FLAG_NO_VIDEO))
		m->video_idx = av_find_best_stream(m->ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (!(open_flags & MEDIA_FLAG_NO_AUDIO))
		m->audio_idx = av_find_best_stream(m->ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (!(open_flags & MEDIA_FLAG_NO_SUBTITLE))
		m->subtitle_idx = av_find_best_stream(m->ic, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
}

static int ex_av_media_init_caches(exAVMedia *m) {
	m->vpackets.list = list_create(MUTEX, VIDEO_PACKET_QUEUE_SIZE);
	m->apackets.list = list_create(MUTEX, AUDIO_PACKET_QUEUE_SIZE);
	m->spackets.list = list_create(MUTEX, SUBTITLE_PACKET_QUEUE_SIZE);
	m->vframes.list  = list_create(MUTEX, VIDEO_PICTURE_QUEUE_SIZE);
	m->aframes.list  = list_create(MUTEX, AUDIO_SAMPLE_QUEUE_SIZE);
	m->sframes.list  = list_create(MUTEX, SUBTITLE_PICTURE_QUEUE_SIZE);
	if (!m->vpackets.list || !m->apackets.list || !m->spackets.list ||
			!m->vframes.list  || !m->aframes.list  || !m->sframes.list) {
		av_log(NULL, AV_LOG_ERROR, "unable to create caches: no memory\n");
		goto err;
	}
	return 0;
err:
	ex_av_media_free_caches(m);
	return -1;
}

static void ex_av_media_read_video_stream_info(exAVMedia *m) {
	AVStream *stream = m->ic->streams[m->video_idx];
	AVCodecParameters *param = stream->codecpar;
	m->video_width = param->width;
	m->video_height = param->height;
	m->video_pix_fmt = param->format;
	m->video_sar = param->sample_aspect_ratio;
	m->video_time_base = stream->time_base;
	m->video_start_time = stream->start_time;
	m->video_frame_rate = av_guess_frame_rate(m->ic, stream, NULL);
}

static void ex_av_media_read_audio_stream_info(exAVMedia *m) {
	AVStream *stream = m->ic->streams[m->audio_idx];
	AVCodecParameters *param = stream->codecpar;
	m->audio_sample_rate = param->sample_rate;
	m->audio_sample_fmt = param->format;
	m->audio_channel_layout = param->channel_layout;
	m->audio_channels = param->channels;
	m->audio_time_base = stream->time_base;
	m->audio_start_time = stream->start_time;
	m->audio_frame_size = param->frame_size;
}

static void ex_av_media_read_subtile_stream_info(exAVMedia *media) {
	/* TODO */
}

static void ex_av_media_read_stream_info(exAVMedia *m) {
	if (m->video_idx < 0) {
check_audio:
		if (m->audio_idx < 0) {
check_subtile:
			if (m->subtitle_idx < 0) {
				return;
			}
			ex_av_media_read_subtile_stream_info(m);
			return;
		}
		ex_av_media_read_audio_stream_info(m);
		goto check_subtile;
	}
	ex_av_media_read_video_stream_info(m);
	goto check_audio;
}

static int _ex_av_media_open(exAVMedia *m, const char *url, int open_flags) {
	int ret = -1;

	if (m->ic) {
		/* The media is already opened */
		if (m->ic->url && !strcmp(m->ic->url, url))
			return 0;
		m->close(m);
	}

	if ((ret = avformat_open_input(&m->ic, url, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "ex_av_media_open: avformat_open_input error: %s: %s\n", av_err2str(ret), url);
		goto err0;
	}
	if ((ret = avformat_find_stream_info(m->ic, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "ex_av_media_open: avformat_find_stream_info error: %s: %s\n", av_err2str(ret), url);
		goto err1;
	}
	ex_av_media_find_stream_index(m, open_flags);
	if (ex_av_media_init_caches(m))
		goto err1;
	ex_av_media_read_stream_info(m);
	return 0;
err1:
	avformat_close_input(&m->ic);
err0:
	return ret;
}

static void ex_av_media_stop(exAVMedia *m) {
	if (m->ic) {
		if (m->play_started)
			m->stop_play(m);
		if (m->decode_started)
			m->stop_decode(m);
	}
}

static void ex_av_media_close(exAVMedia *m) {
	ex_av_media_stop(m);
	if (m->ic) {
		ex_av_media_free_caches(m);
		avformat_close_input(&m->ic);
	}
}

static exAVMedia *ex_av_media_get(exAVMedia *self) {
	if (atomic_inc_and_test(&self->refcount)) {
		/* the media is being released */
		atomic_dec(&self->refcount);
		return NULL;
	}
	return self;
}

static void ex_av_media_put(exAVMedia *self) {
	if (atomic_dec_and_test(&self->refcount)) {
		if (pthread_rwlock_trywrlock(&self->rwlock))
			return; /* there is someone else has 'get' this packet, so let that caller free the media */
		atomic_dec(&self->refcount);
		list_del(&self->list);
		ex_av_media_close(self);
		pthread_rwlock_unlock(&self->rwlock);
		pthread_rwlock_destroy(&self->rwlock);
		free(self);
	}
}

static void ex_av_media_play(exAVMedia *m) {
	if (m->ic) {
		if (!m->decode_started)
			m->start_decode(m);
		else if (ex_av_media_packet_grabber_stopped(m) &&
						 ex_av_media_video_decoder_stopped(m)  &&
						 ex_av_media_audio_decoder_stopped(m)  &&
						 ex_av_media_subtitle_decoder_stopped(m))
			m->start_decode(m);
		if (!m->play_started)
			m->start_play(m);
		/* stop decode and play when finished playing */
		m->stop(m);
	}
}

static void ex_av_media_init_ops(exAVMedia *m) {
	m->get          = ex_av_media_get;
	m->put          = ex_av_media_put;
	m->open         = _ex_av_media_open;
	m->close        = ex_av_media_close;
	m->start_decode = ex_av_media_start_decode;
	m->stop_decode  = ex_av_media_stop_decode;
	m->start_play   = ex_av_media_start_play;
	m->stop_play    = ex_av_media_stop_play;
	m->play         = ex_av_media_play;
	m->stop         = ex_av_media_stop;
}

static void ex_av_media_init_common(exAVMedia *m) {
	INIT_LIST_HEAD(&m->list);
	atomic_set(&m->refcount, 1);
	m->video_idx = -1;
	m->audio_idx = -1;
	m->subtitle_idx = -1;
	m->audio_clock_serial = -1;
	m->av_sync_type = AV_SYNC_AUDIO_MASTER;
	ex_av_clock_init(&m->audio_avclock);
	ex_av_clock_init(&m->video_avclock);
	ex_av_clock_init(&m->external_avclock);
	m->seek_step = 30.0;
}

static int ex_av_media_init(exAVMedia *m) {
	int ret = -1;
	ex_av_media_init_common(m);
	ex_av_media_init_ops(m);
	pthread_rwlockattr_t rwlockattr;
	if (pthread_rwlockattr_init(&rwlockattr))
		return ret;
	ret = pthread_rwlock_init(&m->rwlock, &rwlockattr);
	pthread_rwlockattr_destroy(&rwlockattr);
	return ret;
}

exAVMedia *ex_av_media_alloc(void) {
	exAVMedia *m = NULL;
	m = calloc(1, sizeof(exAVMedia));
	if (m == NULL)
		goto err;
	ex_av_media_init(m);
	return m;
err:
	return NULL;
}

exAVMedia *ex_av_media_open(const char *url, int flags) {
	exAVMedia *m = ex_av_media_alloc();
	if (m == NULL) {
		av_log(NULL, AV_LOG_ERROR, "ex_av_media_open error: %s\n", av_err2str(AVERROR(ENOMEM)));
		return NULL;
	}
	if (_ex_av_media_open(m, url, flags) < 0) {
		m->put(m);
		return NULL;
	}
	return m;
}
