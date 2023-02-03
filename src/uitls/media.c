/*
 * media.c
 *
 *  Created on: 2023-02-01 15:20:08
 *      Author: yui
 */

#include <libavutil/time.h>

#include <media.h>
#include <sdl.h>

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop,
																	 int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar) {
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
		max_height = media->height;
	calculate_display_rect(&rect, 0, 0, max_width, max_height, media->width, media->height, media->sample_aspect_ratio);
	media->screen_width  = media->default_width  = rect.w;
	media->screen_height = media->default_height = rect.h;
}

static inline int skip_packet(exAVPacket *pkt, struct list *list) {
	pkt->put(pkt);
	return 0;
}

static inline int insert_packet(exAVPacket *pkt, struct list *list) {
	while (list->ops->insert_tail(list, &pkt->list)) {
		av_usleep(100000); /* the list maybe full, then wait a bit */
	}
	return 0;
}

/*
 * grab a packet and insert it into the list if success.
 */
static int grab_packet(exAVMedia *m) {
	int ret = -1;
	struct list *pkt_list = NULL;
	exAVPacket *pkt = ex_av_packet_alloc();
	if (pkt == NULL) {
		av_log(NULL, AV_LOG_FATAL, "grab_packet error: unable to create packet: no memory\n");
		ret = AVERROR(ENOMEM);
		return ret;
	}
	ret = av_read_frame(m->avfmt_ctx, pkt->avpkt);
	if (ret == 0) {
		if (pkt->avpkt->stream_index == m->video_idx)
			pkt_list = m->vpackets;
		else if (pkt->avpkt->stream_index == m->audio_idx)
			pkt_list = m->apackets;
		else if (pkt->avpkt->stream_index == m->subtitle_idx)
			pkt_list = m->spackets;
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
 * packet-grabber routine
 */
static void *packet_grabber(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_grabber_routine(m);
	m->flags |= MEDIA_FLAG_GRABBER_FINISHED;
	return NULL;
}

/*
 * grab one frame and insert it into the list.
 */
static int grab_frame(AVCodecContext *codec_ctx, struct list *list) {
	int ret = -1;
	exAVFrame *frame = ex_av_frame_alloc();
	if (frame == NULL) {
		av_log(NULL, AV_LOG_FATAL, "unable to allocate frame: no memory\n");
		goto err0;
	}
	ret = avcodec_receive_frame(codec_ctx, frame->avframe);
	if (ret == 0) { /* successfully received a frame, insert it into the list */
retry:
		if (list->ops->insert_tail(list, &frame->list)) {
			av_usleep(1000000);
			goto retry;
		}
	}
	else { /* failed to received a frame, then release the memeory */
		frame->put(frame);
	}
err0:
	return ret;
}

/*
 * decode a packet into a frame, and insert it into the list.
 */
static int decode(AVCodecContext *codec_ctx, exAVPacket *pkt, struct list *list) {
	int ret = AVERROR(EAGAIN);
	while (ret == AVERROR(EAGAIN)) {
		ret = avcodec_send_packet(codec_ctx, pkt->avpkt);
		if (ret == 0 || ret == AVERROR(EAGAIN))
			grab_frame(codec_ctx, list);
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
	struct list *pkt_list = NULL, *frame_list = NULL;
	/*
	 * if there is no such stream of type 'type', then it's unnecessary to create
	 * its corresponding decoder
	 */
	int stream_idx = get_stream_idx(m, type);
	if (stream_idx < 0)
		return;
	switch (type) {
	case AVMEDIA_TYPE_VIDEO:
		pkt_list = m->vpackets; frame_list = m->vframes; break;
	case AVMEDIA_TYPE_AUDIO:
		pkt_list = m->apackets; frame_list = m->aframes; break;
	case AVMEDIA_TYPE_SUBTITLE:
		pkt_list = m->spackets; frame_list = m->sframes; break;
	default: break;
	}
	stream = m->avfmt_ctx->streams[stream_idx];
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
		n = pkt_list->ops->pop_front(pkt_list);
		if (n == NULL) {
			if(m->flags & MEDIA_FLAG_GRABBER_FINISHED)
				goto err2;
			/* the list maybe empty, then wait a bit moment */
			av_usleep(500000);
			continue;
		}
		exAVPacket *pkt = list_entry(n, exAVPacket, list);
		ret = decode(codec_ctx, pkt, frame_list);
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
	m->flags |= MEDIA_FLAG_VIDEO_DECODER_FINISHED;
	return NULL;
}

/*
 * audio-decoder routine
 */
static void *audio_decoder(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_decode_routine(m, AVMEDIA_TYPE_AUDIO);
	m->flags |= MEDIA_FLAG_AUDIO_DECODER_FINISHED;
	return NULL;
}

/*
 * subtitle-decoder routine
 */
static void *subtitle_decoder(void *arg) {
	exAVMedia *m = (exAVMedia *)arg;
	run_decode_routine(m, AVMEDIA_TYPE_SUBTITLE);
	m->flags |= MEDIA_FLAG_SUBTITLE_DECODER_FINISHED;
	return NULL;
}

static void video_image_display(exAVMedia *media, exAVFrame *frame, double *remaining_time) {
	SDL_Rect rect;
	calculate_display_rect(&rect,
													0, 0,
													media->screen_width, media->screen_height,
													media->width, media->height,
													media->sample_aspect_ratio);
	if (frame)
		sdl_update_texture(media->renderer, &media->texture, frame->avframe, &media->img_convert_ctx);
	SDL_RenderCopyEx(media->renderer, media->texture, NULL, &rect, 0, NULL, media->flip ? SDL_FLIP_VERTICAL : 0);
}

static void video_refresh(exAVMedia *media, double *remaining_time) {
	exAVFrame *frame = NULL;
	struct list_head *n = media->frame_poped ? NULL : media->vframes->ops->pop_front(media->vframes);
	if (n)
		frame = list_entry(n, exAVFrame, list);
	if (frame) {
		double pts = av_q2d(media->video_time_base) * (frame->avframe->pts - media->video_start_time);
		uint64_t now = av_gettime_relative();
		printf("%lld   %lld   %lld  %f  %f\n", media->frame_start_time, now, now - media->frame_start_time, media->last_frame_shown, pts);
		if (pts > media->last_frame_shown) {
			media->frame_poped = 1;
			if (pts - media->last_frame_shown < REFRESH_RATE)
				*remaining_time = pts - media->last_frame_shown;
			return;
		}
		media->frame = frame;
	}
	if (ex_av_media_decoder_is_finished(media))
		media->is_last_frame = 1;
	SDL_ShowWindow(media->window);
	SDL_SetRenderDrawColor(media->renderer, 0, 0, 0, 255);
  SDL_RenderClear(media->renderer);
  video_image_display(media, media->frame, remaining_time);
  SDL_RenderPresent(media->renderer);
  media->last_frame_shown = (av_gettime_relative() - media->frame_start_time) / 1000000.0;
  if (media->frame_start_time == 0)
  	media->frame_start_time = av_gettime_relative();
  if (frame) {
  	frame->put(frame);
  	media->frame = NULL;
  }
  media->frame_poped = 0;
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
		remaining_time = REFRESH_RATE;
		if (media->show_mode != SHOW_MODE_NONE && (!media->paused || media->force_refresh))
			video_refresh(media, &remaining_time);
		SDL_PumpEvents();
	}
}

static int key_event_handler(exAVMedia *media, SDL_Event *event) {
	return 0;
}

static int mouse_event_handler(exAVMedia *media, SDL_Event *event) {
	if (media->cursor_hidden) {
			SDL_ShowCursor(1);
			media->cursor_hidden = 0;
	}
	media->cursor_last_shown = av_gettime_relative();
	return 0;
}

static void window_event_resize(exAVMedia *media, SDL_Event *event) {
	if(event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
		media->screen_width  = event->window.data1;
		media->screen_height = event->window.data2;
		if (media->texture && !media->is_last_frame) {
			SDL_DestroyTexture(media->texture);
			media->texture = NULL;
		}
	}
}

static int window_event_handler(exAVMedia *media, SDL_Event *event) {
	fflush(stdout);
	switch (event->window.event) {
	case SDL_WINDOWEVENT_SIZE_CHANGED:
	case SDL_WINDOWEVENT_EXPOSED:
		window_event_resize(media, event);
		media->force_refresh = 1;
		break;
	default:
		break;
	}
	return 0;
}

static int quit_event_handler(exAVMedia *media, SDL_Event *event) {
	return 1;
}

static int event_handlers(exAVMedia *media, SDL_Event *event) {
	int ret = 0;
	switch (event->type) {
		case SDL_QUIT:
			ret = quit_event_handler(media, event);
			break;
		case SDL_KEYDOWN:
			ret = key_event_handler(media, event);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEMOTION:
			ret = mouse_event_handler(media, event);
			break;
		case SDL_WINDOWEVENT:
			ret = window_event_handler(media, event);
			break;
		default:
			break;
		}
	return ret;
}

static void event_loop(exAVMedia *media) {
	SDL_Event event;
	do {
		refresh_loop_wait_event(media, &event);
	} while(!event_handlers(media, &event));
}
static void ex_av_media_stop(exAVMedia *media) {
	if (media->window) {
		SDL_DestroyWindow(media->window);
		media->window = NULL;
	}
	if (media->renderer) {
		SDL_DestroyRenderer(media->renderer);
		media->renderer = NULL;
	}
	if (media->texture) {
		SDL_DestroyTexture(media->texture);
		media->texture = NULL;
	}
	if (media->img_convert_ctx) {
		sws_freeContext(media->img_convert_ctx);
		media->img_convert_ctx = NULL;
	}
	SDL_Quit();
}

static void ex_av_media_play(exAVMedia *media) {
	SDL_Init(SDL_INIT_VIDEO);

	set_default_window_size(media);

	media->window = SDL_CreateWindow("media-player",
																		media->screen_left, media->screen_top,
																		media->default_width, media->default_height,
																		SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
	if (!media->window) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_show error: failed to create SDL_Window.\n");
		goto err;
	}

	media->renderer = SDL_CreateRenderer(media->window, -1, 0);
	if (!media->renderer) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_show error: failed to create SDL_Renderer.\n");
		goto err;
	}

	event_loop(media);

err:
	ex_av_media_stop(media);
}

static void ex_av_media_free(exAVMedia **media) {
	ex_av_media_caches_free(*media);
	free(*media);
	*media = NULL;
}

static void ex_av_media_init_video_info(exAVMedia *media) {
	AVStream *stream = media->avfmt_ctx->streams[media->video_idx];
	AVCodecParameters *param = stream->codecpar;
	media->width = param->width;
	media->height = param->height;
	media->pix_fmt = param->format;
	media->sample_aspect_ratio = param->sample_aspect_ratio;
	media->video_time_base = stream->time_base;
	media->video_start_time = stream->start_time;
}

static void ex_av_media_init_audio_info(exAVMedia *media) {
	AVStream *stream = media->avfmt_ctx->streams[media->audio_idx];
	AVCodecParameters *param = stream->codecpar;
	media->sample_rate = param->sample_rate;
	media->sample_fmt = param->format;
	media->channel_layout = param->channel_layout;
	media->channels = param->channels;
	media->audio_time_base = stream->time_base;
	media->audio_start_time = stream->start_time;
}

static void ex_av_media_init_subtile_info(exAVMedia *media) {
	/* TODO */
}

static void ex_av_media_init_common(exAVMedia *media) {
	INIT_LIST_HEAD(&media->list);
	media->play = ex_av_media_play;
	media->default_width = 640;
	media->default_height = 480;
	media->screen_width = 0;
	media->screen_height = 0;
	media->screen_left = SDL_WINDOWPOS_CENTERED;
	media->screen_top = SDL_WINDOWPOS_CENTERED;
	media->flip = 0;
}

static void ex_av_media_init_stream_info(exAVMedia *media) {
	if (media->flags & MEDIA_FLAG_NO_VIDEO) {
check_audio:
		if (media->flags & MEDIA_FLAG_NO_AUDIO) {
check_subtile:
			if (media->flags & MEDIA_FLAG_NO_SUBTITLE) {
				return;
			}
			ex_av_media_init_subtile_info(media);
			return;
		}
		ex_av_media_init_audio_info(media);
		goto check_subtile;
	}
	ex_av_media_init_video_info(media);
	goto check_audio;
}

static void ex_av_media_init(exAVMedia *media) {
	ex_av_media_init_common(media);
	ex_av_media_init_stream_info(media);
}

exAVMedia *ex_av_url_open(const char *url, int flags) {
	int ret = -1, video_idx = -1, audio_idx = -1, subtitle_idx = -1;
	AVFormatContext *ic = NULL;
	exAVMedia *media = calloc(1, sizeof(exAVMedia));
	if (media == NULL) {
		av_log(NULL, AV_LOG_ERROR, "unable to create media: no memory\n");
		goto err0;
	}
	if ((ret = avformat_open_input(&ic, url, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "failed to open media file: %s: %s\n", av_err2str(ret), url);
		goto err1;
	}
	if ((ret = avformat_find_stream_info(ic, NULL)) < 0) {
		av_log(NULL, AV_LOG_INFO, "unable to find stream information from media file: %s: %s\n", av_err2str(ret), url);
		goto err1;
	}
	if (flags & MEDIA_FLAG_NO_VIDEO)
		media->flags |= MEDIA_FLAG_NO_VIDEO;
	else if ((video_idx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)) < 0) {
		av_log(NULL, AV_LOG_INFO, "no video stream found in media file: %s\n", url);
		media->flags |= MEDIA_FLAG_NO_VIDEO;
	}
	if (flags & MEDIA_FLAG_NO_AUDIO)
		media->flags |= MEDIA_FLAG_NO_AUDIO;
	else if ((audio_idx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0)) < 0) {
		av_log(NULL, AV_LOG_INFO, "no audio stream found in media file: %s\n", url);
		media->flags |= MEDIA_FLAG_NO_AUDIO;
	}
	if (flags & MEDIA_FLAG_NO_SUBTITLE)
		media->flags |= MEDIA_FLAG_NO_SUBTITLE;
	else if ((subtitle_idx = av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0)) < 0) {
		av_log(NULL, AV_LOG_INFO, "no subtitle stream found in media file: %s\n", url);
		media->flags |= MEDIA_FLAG_NO_SUBTITLE;
	}
	if (ex_av_media_caches_init(media))
		goto err1;
	media->avfmt_ctx = ic;
	media->video_idx = video_idx;
	media->audio_idx = audio_idx;
	media->subtitle_idx = subtitle_idx;
	ex_av_media_init(media);
	return media;
err1:
	free(media);
err0:
	avformat_close_input(&ic);
	return NULL;
}

exAVMedia *ex_av_media_open(const char *url, int flags) {
	int ret = -1;
	exAVMedia *media = NULL;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr))
		goto err0;

	media = ex_av_url_open(url, flags);
	if(media == NULL)
		goto err0;

	if ((ret = pthread_create(&media->packet_grabber, &attr, packet_grabber, media))) {
		av_log(NULL, AV_LOG_ERROR, "failed to create packet-grabber thread for media file: %s: %s\n", strerror(ret), url);
		media->flags |= MEDIA_FLAG_GRABBER_FINISHED;
	}

	if (media->flags & MEDIA_FLAG_NO_VIDEO){
		media->flags |= MEDIA_FLAG_VIDEO_DECODER_FINISHED;
	}
	else if ((ret = pthread_create(&media->video_decoder, &attr, video_decoder, media))) {
		av_log(NULL, AV_LOG_ERROR, "failed to create video-decoder thread for media file: %s: %s\n", strerror(ret), url);
		media->flags |= MEDIA_FLAG_VIDEO_DECODER_FINISHED;
	}

	if (media->flags & MEDIA_FLAG_NO_AUDIO) {
		media->flags |= MEDIA_FLAG_AUDIO_DECODER_FINISHED;
	}
	else if ((ret = pthread_create(&media->audio_decoder, &attr, audio_decoder, media))) {
		av_log(NULL, AV_LOG_ERROR, "failed to create audio-decoder thread for media file: %s: %s\n", strerror(ret), url);
		media->flags |= MEDIA_FLAG_AUDIO_DECODER_FINISHED;
	}

	if (media->flags & MEDIA_FLAG_NO_SUBTITLE) {
		media->flags |= MEDIA_FLAG_SUBTITLE_DECODER_FINISHED;
	}
	else if ((ret = pthread_create(&media->subtitle_decoder, &attr, subtitle_decoder, media))) {
		av_log(NULL, AV_LOG_ERROR, "failed to create subtitle-decoder thread for media file: %s: %s\n", strerror(ret), url);
		media->flags |= MEDIA_FLAG_SUBTITLE_DECODER_FINISHED;
	}

	pthread_attr_destroy(&attr);
err0:
	return media;
}

void ex_av_media_close(exAVMedia **media) {
	if (*media == NULL)
		return;
	if (!((*media)->flags & MEDIA_FLAG_GRABBER_FINISHED)) {
		pthread_cancel((*media)->packet_grabber);
		(*media)->flags |= MEDIA_FLAG_GRABBER_FINISHED;
	}
	if (!((*media)->flags & MEDIA_FLAG_VIDEO_DECODER_FINISHED)) {
		pthread_cancel((*media)->video_decoder);
		(*media)->flags |= MEDIA_FLAG_VIDEO_DECODER_FINISHED;
	}
	if (!((*media)->flags & MEDIA_FLAG_AUDIO_DECODER_FINISHED)) {
		pthread_cancel((*media)->audio_decoder);
		(*media)->flags |= MEDIA_FLAG_AUDIO_DECODER_FINISHED;
	}
	if (!((*media)->flags & MEDIA_FLAG_SUBTITLE_DECODER_FINISHED)) {
		pthread_cancel((*media)->subtitle_decoder);
		(*media)->flags |= MEDIA_FLAG_SUBTITLE_DECODER_FINISHED;
	}
	pthread_join((*media)->packet_grabber, NULL);
	pthread_join((*media)->video_decoder, NULL);
	pthread_join((*media)->audio_decoder, NULL);
	pthread_join((*media)->subtitle_decoder, NULL);

	ex_av_media_free(media);
}

