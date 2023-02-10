/*
 * media.c
 *
 *  Created on: 2023-02-01 15:20:08
 *      Author: yui
 */

#include <pthread.h>

#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libavutil/error.h>
#include <libavutil/avstring.h>

#include <media.h>


#if HAVE_SDL2
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

static int audio_decode_frame(exAVMedia *m) {
	int data_size, resampled_data_size;
	int64_t dec_channel_layout;
	av_unused double audio_clock0;
	int wanted_nb_samples;
	exAVFrame *af = NULL;
	struct list_head *n = NULL;

	if (m->play_paused)
		return -1;
	do {
#if defined(_WIN32)
		while (m->aframes.list->count == 0) {
			if ((av_gettime_relative() - m->audio_callback_time) > 1000000LL * m->audio_hw_buf_size / m->audio_dst.bytes_per_sec / 2)
				return -1;
			av_usleep(1000);
		}
#endif
		if (!(n = m->aframes.list->pop_front(m->aframes.list)))
			return -1;
		af = list_entry(n, exAVFrame, list);
	} while (af->serial != m->aframes.serial);

	data_size = av_samples_get_buffer_size(NULL, af->avframe->channels, af->avframe->nb_samples, af->avframe->format, 1);

	dec_channel_layout =
		(af->avframe->channel_layout && af->avframe->channels == av_get_channel_layout_nb_channels(af->avframe->channel_layout)) ?
		af->avframe->channel_layout : av_get_default_channel_layout(af->avframe->channels);
	wanted_nb_samples = synchronize_audio(m, af->avframe->nb_samples);

	if (af->avframe->format        != m->audio_src.fmt            ||
			dec_channel_layout         != m->audio_src.channel_layout ||
			af->avframe->sample_rate   != m->audio_src.freq           ||
			(wanted_nb_samples != af->avframe->nb_samples && !m->swr_ctx)) {
		swr_free(&m->swr_ctx);
		m->swr_ctx = swr_alloc_set_opts(NULL,
																	  m->audio_dst.channel_layout, m->audio_dst.fmt,    m->audio_dst.freq,
																	  dec_channel_layout,          af->avframe->format, af->avframe->sample_rate,
																	  0, NULL);
		if (!m->swr_ctx || swr_init(m->swr_ctx) < 0) {
			av_log(NULL, AV_LOG_ERROR,
						 "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
							af->avframe->sample_rate, av_get_sample_fmt_name(af->avframe->format), af->avframe->channels,
							m->audio_dst.freq, av_get_sample_fmt_name(m->audio_dst.fmt), m->audio_dst.channels);
			swr_free(&m->swr_ctx);
			return -1;
		}
		m->audio_src.channel_layout = dec_channel_layout;
		m->audio_src.channels       = af->avframe->channels;
		m->audio_src.freq = af->avframe->sample_rate;
		m->audio_src.fmt = af->avframe->format;
	}

	if (m->swr_ctx) {
		const uint8_t **in = (const uint8_t **)af->avframe->extended_data;
		uint8_t **out = &m->audio_out_buf;
		int out_count = (int64_t)wanted_nb_samples * m->audio_dst.freq / af->avframe->sample_rate + 256;
		int out_size  = av_samples_get_buffer_size(NULL, m->audio_dst.channels, out_count, m->audio_dst.fmt, 0);
		int len2;
		if (out_size < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
			return -1;
		}
		if (wanted_nb_samples != af->avframe->nb_samples) {
			if (swr_set_compensation(m->swr_ctx, (wanted_nb_samples - af->avframe->nb_samples) * m->audio_dst.freq / af->avframe->sample_rate,
															 wanted_nb_samples * m->audio_dst.freq / af->avframe->sample_rate) < 0) {
				av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
				return -1;
			}
		}
		av_fast_malloc(&m->audio_out_buf, (unsigned int *)&m->audio_out_buf_size, out_size);
		if (!m->audio_out_buf)
			return AVERROR(ENOMEM);
		len2 = swr_convert(m->swr_ctx, out, out_count, in, af->avframe->nb_samples);
		if (len2 < 0) {
			av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
			return -1;
		}
		if (len2 == out_count) {
			av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
			if (swr_init(m->swr_ctx) < 0)
				swr_free(&m->swr_ctx);
		}
		m->audio_in_buf = m->audio_out_buf;
		resampled_data_size = len2 * m->audio_dst.channels * av_get_bytes_per_sample(m->audio_dst.fmt);
	}
	else {
		m->audio_in_buf = af->avframe->data[0];
		resampled_data_size = data_size;
	}

	audio_clock0 = m->audio_clock;
	/* update the audio clock with the pts */
	if (!isnan(af->pts))
		m->audio_clock = af->pts + (double) af->avframe->nb_samples / af->avframe->sample_rate;
	else
		m->audio_clock = NAN;
	m->audio_clock_serial = af->serial;
	af->put(af);
	return resampled_data_size;
}

static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
  exAVMedia *m = opaque;
  int audio_size, len1;

  m->audio_callback_time = av_gettime_relative();

  while (len > 0) {
		if (m->audio_in_buf_index >= m->audio_in_buf_size) {
			 audio_size = audio_decode_frame(m);
			 if (audio_size < 0) {
					/* if error, just output silence */
				 m->audio_in_buf = NULL;
				 m->audio_in_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / m->audio_dst.frame_size * m->audio_dst.frame_size;
			 }
			 else {
				 if (m->show_mode != SHOW_MODE_VIDEO)
					 update_sample_display(m, (int16_t *)m->audio_in_buf, audio_size);
				 m->audio_in_buf_size = audio_size;
			 }
			 m->audio_in_buf_index = 0;
		}
		len1 = m->audio_in_buf_size - m->audio_in_buf_index;
		if (len1 > len)
			len1 = len;
		if (!m->muted && m->audio_in_buf && m->volume == SDL_MIX_MAXVOLUME)
			memcpy(stream, (uint8_t *)m->audio_in_buf + m->audio_in_buf_index, len1);
		else {
			memset(stream, 0, len1);
			if (!m->muted && m->audio_in_buf)
					SDL_MixAudioFormat(stream, (uint8_t *)m->audio_in_buf + m->audio_in_buf_index, AUDIO_S16SYS, len1, m->volume);
		}
		len -= len1;
		stream += len1;
		m->audio_in_buf_index += len1;
  }
  m->audio_write_buf_size = m->audio_in_buf_size - m->audio_in_buf_index;
  /* Let's assume the audio driver that is used by SDL has two periods. */
  if (!isnan(m->audio_clock)) {
		ex_av_clock_set_at(&m->audclk,
								 m->audio_clock - (double)(2 * m->audio_hw_buf_size + m->audio_write_buf_size) / m->audio_dst.bytes_per_sec,
								 m->audio_clock_serial,
								 m->audio_callback_time / 1000000.0);
		ex_av_clock_sync_to_slave(&m->extclk, &m->audclk);
  }
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
	//m->audio_spec.samples = 2 << av_log2(m->audio_frame_size);
	m->audio_spec.userdata = m;
	m->audio_spec.callback = sdl_audio_callback;
	m->audio_dst.channel_layout = m->audio_channel_layout;
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
		m->audio_dst.channel_layout = av_get_default_channel_layout(m->audio_spec.channels);
	}
	if (spec.format != AUDIO_S16SYS) {
		av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
		goto err;
	}
	if (spec.channels != m->audio_spec.channels) {
		m->audio_dst.channel_layout = av_get_default_channel_layout(spec.channels);
		if (!m->audio_dst.channel_layout) {
			av_log(NULL, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
			goto err;
		}
	}
	m->audio_dst.fmt = AV_SAMPLE_FMT_S16;
	m->audio_dst.freq = spec.freq;
	m->audio_dst.channels =  spec.channels;
	m->audio_dst.frame_size = av_samples_get_buffer_size(NULL, m->audio_dst.channels, 1, m->audio_dst.fmt, 1);
	m->audio_dst.bytes_per_sec = av_samples_get_buffer_size(NULL, m->audio_dst.channels, m->audio_dst.freq, m->audio_dst.fmt, 1);
	if (m->audio_dst.bytes_per_sec <= 0 || m->audio_dst.frame_size <= 0) {
		av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
		goto err;
	}
	m->audio_hw_buf_size = spec.size;
	m->audio_src = m->audio_dst;
	return 0;
err:
	return ret;
}

static void ex_av_media_prepare_play_audio(exAVMedia *m) {
	m->audio_buffer = av_buffer_allocz(AUDIO_BUFFER_SIZE);
	if (m->audio_buffer == NULL)
		return;
	audio_spec_init(m);
}

static void ex_av_media_start_play_audio(exAVMedia *m) {
	audio_spec_init(m);
	if (audio_open(m))
		return;
	/*
	 * If the video stream exists, transfer the control to video-player
	 */
	if (m->video_idx >= 0)
		return;
	SDL_PauseAudioDevice(m->audio_dev, 0);
	while (!ex_av_media_audio_decoder_stopped(m)) {
		SDL_Delay(1);
	}
	SDL_Delay(1000);
}

static void ex_av_media_prepare_stop_audio(exAVMedia *m) {
	if (m->audio_buffer)
		av_buffer_unref(&m->audio_buffer);
}

static void ex_av_media_stop_play_audio(exAVMedia *m) {

}

static void ex_av_media_prepare_play_video(exAVMedia *m) {

}

static void ex_av_media_start_play_video(exAVMedia *m) {

}

static void ex_av_media_prepare_stop_video(exAVMedia *m) {

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

/*
 * Grab a packet and insert it into the list if success.
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
	ret = av_read_frame(m->ic, pkt->avpkt);
	if (ret == 0) {
		if (pkt->avpkt->stream_index == m->video_idx)
			pkt_list = m->vpackets.list;
		else if (pkt->avpkt->stream_index == m->audio_idx)
			pkt_list = m->apackets.list;
		else if (pkt->avpkt->stream_index == m->subtitle_idx)
			pkt_list = m->spackets.list;
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
		if (list->insert_tail(list, &frame->list)) {
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
		pkt_list = m->vpackets.list; frame_list = m->vframes.list; break;
	case AVMEDIA_TYPE_AUDIO:
		pkt_list = m->apackets.list; frame_list = m->aframes.list; break;
	case AVMEDIA_TYPE_SUBTITLE:
		pkt_list = m->spackets.list; frame_list = m->sframes.list; break;
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
	m->start_decode = 0;
}

static void ex_av_media_start_decode(exAVMedia *m) {
	/* The media is not yet opened */
	if (m->ic == NULL)
		return;
	/* Start up packet-grabber and decoders */
	pthread_create(&m->packet_grabber, NULL, packet_grabber, m);
	if (m->video_idx >= 0)
		pthread_create(&m->video_decoder, NULL, video_decoder, m);
	if (m->audio_idx >= 0)
		pthread_create(&m->audio_decoder, NULL, audio_decoder, m);
	if (m->subtitle_idx >= 0)
		pthread_create(&m->subtitle_decoder, NULL, subtitle_decoder, m);
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
	m->vpackets.list = list_create(MUTEX, PACKET_QUEUE_SIZE);
	m->apackets.list = list_create(MUTEX, PACKET_QUEUE_SIZE);
	m->spackets.list = list_create(MUTEX, PACKET_QUEUE_SIZE);
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
		if (m->url && !strcmp(m->url, url))
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

static void ex_av_media_close(exAVMedia *m) {
	ex_av_media_stop_play(m);
	ex_av_media_stop_decode(m);
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
		if (!m->play_started)
			m->start_play(m);
	}
}

static void ex_av_media_stop(exAVMedia *m) {
	if (m->ic) {
		if (m->play_started)
			m->stop_play(m);
		if (m->decode_started)
			m->stop_decode(m);
	}
}

static void ex_av_media_init_ops(exAVMedia *m) {
	m->get = ex_av_media_get;
	m->put = ex_av_media_put;
	m->open  = _ex_av_media_open;
	m->close = ex_av_media_close;
	m->start_decode = ex_av_media_start_decode;
	m->stop_decode  = ex_av_media_stop_decode;
	m->start_play  = ex_av_media_start_play;
	m->stop_play = ex_av_media_stop_play;
	m->play = ex_av_media_play;
	m->stop = ex_av_media_stop;
}

static void ex_av_media_init_common(exAVMedia *m) {
	INIT_LIST_HEAD(&m->list);
	atomic_set(&m->refcount, 1);
	m->video_idx = -1;
	m->audio_idx = -1;
	m->subtitle_idx = -1;
#if HAVE_SDL2
	m->volume = 100;
	m->av_sync_type = AV_SYNC_AUDIO_MASTER;
	m->flip = 0;
	m->refresh_rate = 0.01;
	m->cursor_hidden = 0;
	m->window_default_width = 640;
	m->window_default_height = 480;
	m->screen_left = SDL_WINDOWPOS_CENTERED;
	m->screen_top = SDL_WINDOWPOS_CENTERED;
#endif
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
	m->url = av_strdup(url);
	return m;
}




#if 0
extern int sdl_update_texture(SDL_Renderer *render, SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx);
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
	exAVMedia *m = (exAVMedia *)opaque;
	exAVFrame *frame = NULL;
	struct list_head *n = NULL;
	int samples = 0;
	Uint8 *write_ptr = m->params->audio_buffer->data;
	Uint8 *read_ptr = m->params->audio_buffer->data;
	const uint8_t **data = NULL;
	int data_len = 0, count = 0;

	if(m->params->audio_buffer == NULL)
		return;
	SDL_memset(stream, 0, len);

	while(count < len) {
		if (m->swr_ctx == NULL || !swr_is_initialized(m->swr_ctx))
			break;
		n = m->aframes.list->pop_front(m->aframes.list);
		if (n == NULL) {
			if (ex_av_media_audio_decoder_stopped(m))
				break;
		}
		frame = list_entry(n, exAVFrame, list);
retry:
		if (frame) {
			data = (const uint8_t **)frame->avframe->data;
			data_len = frame->avframe->nb_samples;
		}
		write_ptr += count;
		samples = swr_convert(m->swr_ctx, &write_ptr, len - count, data, data_len);
		if (samples > 0)
			count += samples * m->params->audio_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
		frame->put(frame);
		if (swr_get_out_samples(m->swr_ctx, 0) > 0) {
			data = NULL;
			data_len = 0;
			goto retry;
		}
		break;
	}
	if (count > 0) {
		SDL_MixAudioFormat(stream, read_ptr, AUDIO_S16SYS, count, m->params->volume);
		memset(m->params->audio_buffer->data, 0, m->params->audio_buffer->size);
	}
}

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
	int max_width  = media->params->screen_width  ? media->params->screen_width  : INT_MAX;
	int max_height = media->params->screen_height ? media->params->screen_height : INT_MAX;
	if (max_width == INT_MAX && max_height == INT_MAX)
		max_height = media->params->video_height;
	calculate_display_rect(&rect, 0, 0, max_width, max_height, media->params->video_width, media->params->video_height, media->params->video_sar);
	media->params->screen_width  = media->params->window_default_width  = rect.w;
	media->params->screen_height = media->params->window_default_height = rect.h;
}

static int get_master_sync_type(exAVMedia *m) {
	if (m->params->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		if (m->video_idx >= 0)
			return AV_SYNC_VIDEO_MASTER;
		else
			return AV_SYNC_AUDIO_MASTER;
	}
	else if (m->params->av_sync_type == AV_SYNC_AUDIO_MASTER) {
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
		val = ex_av_clock_get(&m->video_clock);
		break;
	case AV_SYNC_AUDIO_MASTER:
		val = ex_av_clock_get(&m->audio_clock);
		break;
	default:
		val = ex_av_clock_get(&m->external_clock);
		break;
	}
	return val;
}

static void video_image_display(exAVMedia *m, exAVFrame *frame, double *remaining_time) {
	SDL_Rect rect;
	calculate_display_rect(&rect,
													0, 0,
													m->params->screen_width, m->params->screen_height,
													m->params->video_width, m->params->video_height,
													m->params->video_sar);
	if (frame)
		sdl_update_texture(m->renderer, &m->texture, frame->avframe, &m->sws_ctx);
	SDL_ShowWindow(m->window);
	SDL_SetRenderDrawColor(m->renderer, 0, 0, 0, 255);
	SDL_RenderClear(m->renderer);
	SDL_RenderCopyEx(m->renderer, m->texture, NULL, &rect, 0, NULL, m->params->flip ? SDL_FLIP_VERTICAL : 0);
	SDL_RenderPresent(m->renderer);
}

static void video_refresh(exAVMedia *m, double *remaining_time) {
	if (m->frame == NULL) {
		struct list_head *n = media->vframes->ops->pop_front(media->vframes);
		if (n == NULL) {
			if (ex_av_media_decoder_is_finished(media)) {
				media->finsished = 1;
				if (media->refresh_rate == 0)
					media->refresh_rate = 0.1;
				goto refresh;
			}
			return;
		}
		media->frame = list_entry(n, exAVFrame, list);
	}
	if (media->frame) {
		double pts = (media->frame->avframe->pts - media->video_start_time) * av_q2d(media->video_time_base);
		if (pts > 0) {
			double ts_diff = (av_gettime_relative() - media->last_frame_ts) / 1000000.0 - 0.005;  /* 0.005 is the time consumed to display a frame */
			double pts_diff = pts - media->last_frame_pts;
			if (pts_diff > ts_diff) {
				if (pts_diff - ts_diff < media->refresh_rate)
					*remaining_time = pts_diff - ts_diff;
				return;
			}
		}
		media->last_frame_pts = pts;
	}
refresh:
  video_image_display(media, media->frame, remaining_time);
  if (media->frame) {
  	media->last_frame_ts = av_gettime_relative();
  	media->frame->put(media->frame);
  	media->frame = NULL;
  }
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

static void toggle_full_screen(exAVMedia *media) {
    media->is_full_screen = !media->is_full_screen;
    SDL_SetWindowFullscreen(media->window, media->is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static int key_event_handler(exAVMedia *media, SDL_Event *event) {
	return 0;
}

static int mouse_event_handler(exAVMedia *media, SDL_Event *event) {
	if (event->type == SDL_MOUSEBUTTONDOWN) {
		if (event->button.button == SDL_BUTTON_LEFT) {
				static int64_t last_mouse_left_click = 0;
				if (av_gettime_relative() - last_mouse_left_click <= 500000) {
					toggle_full_screen(media);
					media->force_refresh = 1;
					last_mouse_left_click = 0;
				}
				else {
					last_mouse_left_click = av_gettime_relative();
				}
			}
	}
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
		if (media->texture && !media->finsished) {
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

static void ex_av_media_play_stop(exAVMedia *m) {
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
	if (m->audio_idx >= 0) {
		SDL_CloseAudio();
	}
	if (m->swr_ctx && swr_is_initialized(m->swr_ctx))
		swr_close(m->swr_ctx);
	swr_free(&m->swr_ctx);
	SDL_Quit();
}

static void ex_av_media_play_start(exAVMedia *media) {
	int ret = 0;
	int flags = SDL_INIT_VIDEO;
	SDL_AudioSpec desired = {
		.freq = 48000,
		.format = AUDIO_S16SYS,
		.samples = 512,
		.channels = 2,
		.silence = 0,
		.callback = sdl_audio_callback,
		.userdata = media,
	};
	if (media->audio_idx >= 0) {
		media->audio_swr_ctx = swr_alloc_set_opts(NULL,	AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
																							media->channel_layout, media->sample_fmt, media->sample_rate,
																							AV_LOG_TRACE,	NULL);
		if ((ret = swr_init(media->audio_swr_ctx)) < 0) {
			av_log(NULL, AV_LOG_ERROR, "ex_av_media_play: swr_init error: %s\n", av_err2str(ret));
			goto err;
		}
		media->audio_buffer = av_buffer_allocz(192000);
		if(media->audio_swr_ctx == NULL || media->audio_buffer == NULL)
			goto err;

		flags |= SDL_INIT_AUDIO | SDL_INIT_TIMER;
	}

	SDL_Init(flags);

	if (media->audio_idx >= 0) {
		if(SDL_OpenAudio(&desired, NULL)) {
			av_log(NULL, AV_LOG_ERROR, "av_media_play: SDL_OpenAudio error: %s\n", SDL_GetError());
			goto err;
		}
		SDL_PauseAudio(0);
	}

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
	ex_av_media_play_stop(media);
}
#endif
