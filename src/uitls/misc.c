/*
 * msic.c
 *
 *  Created on: 2023-02-01 14:55:49
 *      Author: yui
 */

#include <sys/stat.h>
#include <fcntl.h>
#if WIN32
#	include <winsock2.h>
#	include <winsock.h>
#	include <ws2tcpip.h>
#else
#	include <sys/types.h>
#	include <sys/socket.h>
#	include <netdb.h>
#endif

#include <libavutil/log.h>

int av_tcp_connect(const char *host, const char *service) {
	int sock = -1;
	int ret = -1;
	struct addrinfo *rp, *result;
	struct addrinfo hint = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = 0,
		.ai_flags = 0
	};
	if ((ret = getaddrinfo(host, service, &hint, &result))) {
		av_log(NULL, AV_LOG_ERROR, "getaddrinfo error: %s\n", gai_strerror(ret));
		goto err0;
	}
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(sock < 0)
			continue;
		if(connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		av_log(NULL, AV_LOG_ERROR, "failed to connect %s: %s\n", host, strerror(errno));
		close(sock);
	}
	ret = AVERROR(sock);
	freeaddrinfo(result);
err0:
	return AVERROR(ret);
}

void av_tcp_close(int sock) {
	if (sock >= 0)
		close(sock);
}

char *av_hex_str(const uint8_t *array, size_t size, char **out) {
	char *str = NULL;
	if (*out == NULL)
		str = av_malloc(2 * size + 1);
	else
		str = *out;
	if (str) {
		for (int i = 0; i < size; i++)
			sprintf(str + 2 * i, "%02x", array[i]);
	}
	if (*out == NULL)
		*out = str;
	return str;
}


static int _av_file_write(const char *name, const char *content, size_t len, int append) {
	size_t off = 0;
	int bytes = 0;
#ifdef __WIN32__
	int fd = open(name, append ? O_WRONLY | O_APPEND | O_BINARY | O_CREAT | O_TRUNC : O_WRONLY | O_BINARY | O_CREAT | O_TRUNC);
#else
	int fd = open(name, append ? O_WRONLY | O_APPEND | O_CREAT | O_TRUNC : O_WRONLY | O_CREAT | O_TRUNC);
#endif
	if (fd < 0)
		return AVERROR(errno);
	while (off < len) {
		bytes = write(fd, content + off, len - off);
		if (bytes < 0) {
			close(fd);
			return AVERROR(errno);
		}
		off += bytes;
	}
	close(fd);
	return len;
}

int av_file_write(const char *name, const char *content, size_t len) {
	return _av_file_write(name, content, len, 0);
}

int av_file_append(const char *name, const char *content, size_t len) {
	return _av_file_write(name, content, len, 1);
}
