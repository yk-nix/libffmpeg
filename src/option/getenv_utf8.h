/*
 * getnev_utf8.h
 *
 *  Created on: 2022-12-06 15:21:39
 *      Author: yui
 */

#ifndef EXFFMEPG_OPTION_GETENV_UTF8_H_
#define EXFFMEPG_OPTION_GETENV_UTF8_H_

#include <ffmpeg_config.h>
#include <stdlib.h>

#include "mem.h"

#if defined(_WIN32)

#include "wchar_filename.h"

static inline char *getenv_utf8(const char *varname) {
	wchar_t *varname_w, *var_w;
	char *var;

	if(utf8towchar(varname, &varname_w))
			return NULL;
	if(!varname_w)
		return NULL;

	var_w = _wgetenv(varname_w);
	av_free(varname_w);

	if(!var_w)
		return NULL;
	if(wchartoutf8(var_w, &var))
		return NULL;

	return var;
}

static inline void freeenv_utf8(char *var) {
  av_free(var);
}

static inline char *getenv_dup(const char *varname) {
	return getenv_utf8(varname);
}

#else

static inline char *getenv_utf8(const char *varname) {
	return getenv(varname);
}

static inline void freeenv_utf8(char *var) {
}

static inline char *getenv_dup(const char *varname) {
	char *var = getenv(varname);
	if(!var)
		return NULL;
	return av_strdup(var);
}

#endif

#endif /* EXFFMEPG_OPTION_GETENV_UTF8_H_ */
