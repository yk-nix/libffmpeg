/*
 * sdl.h
 *
 *  Created on: 2023-02-01 16:45:46
 *      Author: yui
 */

#ifndef INCLUDE_SDL_H_
#define INCLUDE_SDL_H_

#include <SDL2/SDL.h>

#include <libavutil/frame.h>
#include <libswscale/swscale.h>

/*
 * Update texture.
 */
extern int sdl_update_texture(SDL_Renderer *render, SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx);


#endif /* INCLUDE_SDL_H_ */
