/*
 * frame.c
 *
 *  Created on: 2022-12-06 15:21:39
 *      Author: yui
 */

#include <ffmpeg_config.h>
#include <frame.h>

AVFrame	*av_frame_load_picture(const char *url) {
	int ret = 0, stream_idx = -1;
	AVFormatContext *ic = NULL;
	AVCodecContext *cc = NULL;
	AVPacket *pkt = NULL;
	AVFrame *frame = NULL;
	AVCodec *codec = NULL;
	pkt = av_packet_alloc();
	if (pkt == NULL)
		goto err0;
	if ((ret = avformat_open_input(&ic, url, NULL, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_load_picture: avformat_open_input error: %s\n", av_err2str(ret));
		goto err1;
	}
	if ((ret = avformat_find_stream_info(ic, NULL)) < 0) {
		av_log(NULL, AV_LOG_INFO, "av_frame_load_picture: avformat_find_stream_info error: %s\n", av_err2str(ret));
	}
	if ((stream_idx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_load_picture: av_find_best_stream error: failed find video-stream.(the file has no pictures)\n");
		goto err2;
	}
	cc = avcodec_alloc_context3(codec);
	if(cc == NULL) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_load_picture: avcodec_alloc_context3 error: no memory.\n");
		goto err2;
	}
	if ((ret = avcodec_parameters_to_context(cc, ic->streams[stream_idx]->codecpar)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_load_picture: avcodec_parameters_to_context error: %s\n", av_err2str(ret));
		goto err3;
	}
	if ((ret = avcodec_open2(cc, codec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_load_picture: avcodec_open2 error: %s\n", av_err2str(ret));
		goto err3;
	}
	while (1) {
		ret = av_read_frame(ic, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			goto err3;
		if (ret < 0)
			break;
		ret = avcodec_send_packet(cc, pkt);
		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_frame_load_picutre: avcodec_send_packet error: %s\n", av_err2str(ret));
			goto err4;
		}
		av_frame_free(&frame);
		frame = av_frame_alloc();
		if (frame == NULL)
			goto err3;
		while (1) {
			ret = avcodec_receive_frame(cc, frame);
			if (ret == 0) {
				goto err3;
			}
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;
			}
			av_log(NULL, AV_LOG_ERROR, "av_frame_load_picutre: avcodec_receive_frame error: %s\n", av_err2str(ret));
			goto err4;
		}
		av_packet_unref(pkt);
	}
	av_log(NULL, AV_LOG_ERROR, "av_frame_load_picutre: av_read_frame error: %s\n", av_err2str(ret));
err4:
	av_frame_free(&frame);
err3:
	avcodec_free_context(&cc);
err2:
	avformat_close_input(&ic);
err1:
	av_packet_free(&pkt);
err0:
	return frame;
}

#if HAVE_SDL2
# include <SDL2/SDL.h>
# ifdef _WIN32
#  undef main
# endif
static const struct TextureFormatEntry {
	enum AVPixelFormat format;
	int texture_fmt;
} sdl_texture_format_map[] = {
	{ AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
	{ AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
	{ AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
	{ AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
	{ AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
	{ AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
	{ AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
	{ AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
	{ AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
	{ AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
	{ AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
	{ AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
	{ AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
	{ AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
	{ AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
	{ AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
	{ AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
	{ AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
	{ AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
	{ AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

static void get_sdl_pixelfmt_and_blendmode(int format, Uint32 *sdl_pixelfmt, SDL_BlendMode *sdl_blendmode) {
	int i;
	*sdl_blendmode = SDL_BLENDMODE_NONE;
	*sdl_pixelfmt = SDL_PIXELFORMAT_UNKNOWN;
	if(format == AV_PIX_FMT_RGB32   || format == AV_PIX_FMT_RGB32_1 ||
			format == AV_PIX_FMT_BGR32   || format == AV_PIX_FMT_BGR32_1)
		*sdl_blendmode = SDL_BLENDMODE_BLEND;
	for(i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
		if(format == sdl_texture_format_map[i].format) {
			*sdl_pixelfmt = sdl_texture_format_map[i].texture_fmt;
			return;
		}
	}
}

static int realloc_texture(SDL_Renderer *renderer, SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture) {
	Uint32 format;
	int access, w, h;
	if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
		void *pixels;
		int pitch;
		if (*texture)
			SDL_DestroyTexture(*texture);
		if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
			return -1;
		if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
			return -1;
		if (init_texture) {
			if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
				return -1;
			memset(pixels, 0, pitch * new_height);
			SDL_UnlockTexture(*texture);
		}
		av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
	}
	return 0;
}

int sdl_update_texture(SDL_Renderer *render, SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
	int ret = 0;
	Uint32 sdl_pixelfmt;
	SDL_BlendMode sdl_blendmode;
	get_sdl_pixelfmt_and_blendmode(frame->format, &sdl_pixelfmt, &sdl_blendmode);
	if (realloc_texture(render, tex, sdl_pixelfmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pixelfmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
		return -1;
	switch (sdl_pixelfmt) {
		case SDL_PIXELFORMAT_UNKNOWN:
			*img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
																							frame->width, frame->height, frame->format,
																							frame->width, frame->height, AV_PIX_FMT_BGRA,
																							SWS_BICUBIC, NULL, NULL, NULL);
			if (*img_convert_ctx != NULL) {
				uint8_t *pixels[4];
				int pitch[4];
				if(!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
					sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, pixels, pitch);
					SDL_UnlockTexture(*tex);
				}
			}
			else {
				av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
				ret = -1;
			}
			break;
		case SDL_PIXELFORMAT_IYUV:
			if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
				ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
																							 frame->data[1], frame->linesize[1],
																							 frame->data[2], frame->linesize[2]);
			}
			else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
				ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
																							 frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
																							 frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
			}
			else {
				av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
				return -1;
			}
			break;
		default:
			if (frame->linesize[0] < 0) {
				ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
			}
			else {
				ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
			}
			break;
	}
	return ret;
}

void av_frame_show(AVFrame *frame) {
	struct SwsContext *img_convert_ctx = NULL;
	SDL_Window *window = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *texture = NULL;
	SDL_Event event;

	SDL_Init(SDL_INIT_VIDEO);

	window = SDL_CreateWindow("frame-displayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, frame->width, frame->height, SDL_WINDOW_SHOWN);
	if (!window) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_show error: failed to create SDL_Window.\n");
		goto err0;
	}

	renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_show error: failed to create SDL_Renderer.\n");
		goto err1;
	}
	sdl_update_texture(renderer, &texture, frame, &img_convert_ctx);
	SDL_SetRenderTarget(renderer, texture);
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	while (1) {
		SDL_PollEvent(&event);
		if (event.type == SDL_QUIT)
			break;
		SDL_RenderCopy(renderer, texture, NULL, NULL);
		SDL_RenderPresent(renderer);
	}

err1:
	if (window) {
		SDL_DestroyWindow(window);
	}
	if (renderer) {
		SDL_DestroyRenderer(renderer);
	}
	if (texture) {
		SDL_DestroyTexture(texture);
	}
	if (img_convert_ctx) {
		sws_freeContext(img_convert_ctx);
	}
err0:
	SDL_Quit();
}
#else
void av_frame_show(AVFrame *frame) { }
#endif

static void ex_av_frame_show(exAVFrame *self) {
	av_frame_show(self->avframe);
}

int av_frame_save(AVFrame *frame, const char *url) {
	int ret = -1;
	AVFormatContext *fmt_ctx = NULL;
	AVCodec *codec = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVStream *stream = NULL;
	AVPacket *pkt = NULL;
	pkt = av_packet_alloc();
	if (pkt == NULL) {
		ret = AVERROR(ENOMEM);
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: av_packet_alloc error: %s\n", av_err2str(ret));
		goto err0;
	}
	codec = avcodec_find_encoder_by_name("mjpeg");
	if (codec == NULL) {
		ret = AVERROR(ENOENT);
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avcodec_find_encoder error: %s: AV_CODEC_ID_MJPEG\n", av_err2str(ret));
		goto err1;
	}
	codec_ctx = avcodec_alloc_context3(codec);
	if (codec_ctx == NULL) {
		ret = AVERROR(ENOMEM);
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avcodec_alloc_context3 error: %s\n", av_err2str(ret));
		goto err1;
	}
	codec_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
	codec_ctx->time_base = (AVRational) {1, 1};
	codec_ctx->width  = frame->width;
	codec_ctx->height = frame->height;
	if (!avcodec_is_supported_pix_format(codec, frame->format)) {
		if (av_frame_convert_pix_format(frame, AV_PIX_FMT_YUV420P))
			goto err2;
		codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	}
	else {
		codec_ctx->pix_fmt = (enum AVPixelFormat)frame->format;
	}
	if ((ret = avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, url)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avformat_alloc_output_context2 error: %s\n", av_err2str(ret));
		goto err2;
	}
	if(!(stream = avformat_new_stream(fmt_ctx, codec))) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avformat_new_stream: unable to create a new av-stream\n");
		goto err3;
	}
	if ((ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avcodec_parameters_from_context error: %s\n", av_err2str(ret));
		goto err3;
	}
	if ((ret = avio_open(&fmt_ctx->pb, url, AVIO_FLAG_WRITE)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avio_open error: %s: %s\n", av_err2str(ret), url);
		goto err3;
	}
	if ((ret = avformat_write_header(fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avformat_write_header error: %s\n", av_err2str(ret));
		goto err4;
	}
	if ((ret = avcodec_open2(codec_ctx, codec, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avcodec_open2 error: %s\n", av_err2str(ret));
		goto err4;
	}
	if ((ret = avcodec_send_frame(codec_ctx, frame)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_save: avcodec_send_frame error: %s\n", av_err2str(ret));
		goto err5;
	}
	while (1) {
		ret = avcodec_receive_packet(codec_ctx, pkt);
		if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
			break;
		else if (ret < 0)
			goto err6;
		if ((ret = av_write_frame(fmt_ctx, pkt)) < 0) {
			av_log(NULL, AV_LOG_ERROR, "av_frame_save: av_write_frame error: %s\n", av_err2str(ret));
			goto err6;
		}
		av_packet_unref(pkt);
	}
	ret = 0;
err6:
	av_packet_unref(pkt);
err5:
	avcodec_close(codec_ctx);
err4:
	avio_close(fmt_ctx->pb);
	if(ret < 0)
		avpriv_io_delete(url);
err3:
	avformat_free_context(fmt_ctx);
err2:
	avcodec_free_context(&codec_ctx);
err1:
	av_packet_free(&pkt);
err0:
	return ret;
}

static int ex_av_frame_save(exAVFrame *self, const char *url) {
	return av_frame_save(self->avframe, url);
}

int av_frame_convert(AVFrame *frame, int width, int height, int format) {
	int ret = -1;
	struct SwsContext *sws_ctx = NULL;
	AVFrame *dst = NULL;
	if (width == frame->width && height == frame->height && format == frame->format)
		goto err0;
	dst = av_frame_alloc();
	if (dst == NULL) {
		ret = AVERROR(ENOMEM);
		av_log(NULL, AV_LOG_ERROR, "av_frame_convert: av_frame_alloc error: %s\n", av_err2str(ret));
		goto err0;
	}
	av_frame_copy_props(dst, frame);
	dst->width = width;
	dst->height = height;
	dst->format = format;
	if ((ret = av_frame_get_buffer(dst, 0)) < 0) {
		ret = AVERROR(ENOMEM);
		av_log(NULL, AV_LOG_ERROR, "av_frame_convert: av_frame_get_buffer error: %s\n", av_err2str(ret));
		goto err2;
	}
	sws_ctx = sws_getCachedContext(sws_ctx, frame->width, frame->height, frame->format, width, height, format, SWS_BICUBIC, NULL, NULL, NULL);
	if (sws_ctx == NULL) {
		ret = AVERROR(ENOMEM);
		av_log(NULL, AV_LOG_ERROR, "av_frame_convert: sws_getCachedContext error: %s\n", av_err2str(ret));
		goto err1;
	}
	if ((ret = sws_scale(sws_ctx, (const uint8_t * const*)frame->data, frame->linesize, 0, frame->height, dst->data, dst->linesize)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "av_frame_convert: sws_scale error: %s\n", av_err2str(ret));
		goto err1;
	}
	av_frame_unref(frame);
	av_frame_move_ref(frame, dst);
	av_frame_free(&dst);
	ret = 0;
err2:
	av_frame_unref(dst);
err1:
	av_frame_free(&dst);
	sws_freeContext(sws_ctx);
err0:
	return ret;
}

int av_frame_resize(AVFrame *frame, int width, int height) {
	return av_frame_convert(frame, width, height, frame->format);
}

int av_frame_convert_pix_format(AVFrame *frame, int format) {
	return av_frame_convert(frame, frame->width, frame->height, format);
}

static int ex_av_frame_resize(exAVFrame *self, int width, int height) {
	return av_frame_resize(self->avframe, width, height);
}

static int ex_av_frame_convert(exAVFrame *self, int format) {
	return av_frame_convert(self->avframe, self->avframe->width, self->avframe->height, format);
}

static exAVFrame *get(exAVFrame *self) {
	if(atomic_inc_and_test(&self->refcount)) {
		/* the frame is being released */
		atomic_dec(&self->refcount);
		return NULL;
	}
	return self;
}

static void put(exAVFrame *self) {
	if(atomic_dec_and_test(&self->refcount)) {
		if(pthread_rwlock_trywrlock(&self->rwlock))
			return; /* there is someone else has 'get' this frame, so let that caller free the frame */
		atomic_dec(&self->refcount);
		list_del(&self->list);
		if(self->avframe && av_frame_is_writable(self->avframe))
			av_frame_unref(self->avframe);
		av_frame_free(&self->avframe);
		pthread_rwlock_unlock(&self->rwlock);
		pthread_rwlock_destroy(&self->rwlock);
		free(self);
	}
}

static void ex_av_frame_ops_init(exAVFrame *f) {
	f->get = get;
	f->put = put;
	f->resize  = ex_av_frame_resize;
	f->save    = ex_av_frame_save;
	f->convert = ex_av_frame_convert;
	f->show    = ex_av_frame_show;
}

static int ex_av_frame_init(exAVFrame *f) {
	int ret = -1;
	INIT_LIST_HEAD(&f->list);
	atomic_set(&f->refcount, 1);
	ex_av_frame_ops_init(f);
	pthread_rwlockattr_t rwlockattr;
	if (pthread_rwlockattr_init(&rwlockattr))
		return ret;
	ret = pthread_rwlock_init(&f->rwlock, &rwlockattr);
	pthread_rwlockattr_destroy(&rwlockattr);
	return ret;
}

exAVFrame *ex_av_frame_alloc(size_t size) {
	exAVFrame *f = NULL;
	if (size < sizeof(exAVFrame))
		size = sizeof(exAVFrame);
	f = calloc(1, size);
	if (f == NULL)
		goto err0;
	f->avframe = av_frame_alloc();
	if (f->avframe == NULL)
		goto err1;
	ex_av_frame_init(f);
	return f;
err1:
	free(f);
err0:
	return NULL;
}

exAVFrame *ex_av_frame_load_picture(const char *url) {
	exAVFrame *f = NULL;
	f = calloc(1, sizeof(exAVFrame));
	if (f == NULL)
		goto err0;
	f->avframe = av_frame_load_picture(url);
	if (f->avframe == NULL)
		goto err1;
	ex_av_frame_init(f);
	return f;
err1:
	free(f);
err0:
	return NULL;
}

void ex_av_frame_free_list_entry(struct list_head *n) {
	exAVFrame *self = list_entry(n, exAVFrame, list);
	put(self);
}


