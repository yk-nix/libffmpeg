/*
 * sdl.c
 *
 *  Created on: 2023-02-01 16:45:29
 *      Author: yui
 */

#include <libavutil/pixfmt.h>
#include <libavutil/log.h>
#include <libavutil/error.h>

#include <sdl.h>

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


