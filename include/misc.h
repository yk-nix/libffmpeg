/*
 * misc.h
 *
 *  Created on: 2023-02-01 14:52:09
 *      Author: yui
 */

#ifndef INCLUDE_MISC_H_
#define INCLUDE_MISC_H_

/*
 * TCP connect to 'host', with its port is specified via 'service'
 * (service can also specified as a string of a number).
 * Return the socket opened if success, otherwise, return a negative error number.
 */
extern int av_tcp_connect(const char *host, const char *service);
/*
 * Close an opened TCP socket.
 */
extern void av_tcp_close(int sock);

/*
 * Convert an array into hexadecimal string.
 * If **out is NULL, create the string, and in that case caller must free it via 'av_freep',
 * when it is not used any more.
 */
char *av_hex_str(const uint8_t *array, size_t size, char **out);

/*
 * Convert an array into hexadecimal string, and print it on standard out.
 * 'eol' indicate whether to print the '\n'.
 */
static inline void av_hex_print(const uint8_t *array, size_t size, int eol) {
	for (int i = 0; i < size; i++)
		printf("%02x", array[i]);
	if (eol)
		printf("\n");
}

/*
 * Write 'content' into the 'name' file. Create the file if it dosen't exists;
 * Truncate the file if it exists already.
 * Return how many bytes written if success, otherwise return a negative error number.
 */
extern int av_file_write(const char *name, const char *content, size_t len);

/*
 * Append 'content' at the tail of the 'name' file. Create the file if it dosen't exists.
 * Return how many bytes appended if success, otherwise, return a negative error number.
 */
extern int av_file_append(const char *name, const char *content, size_t len);

#endif /* INCLUDE_MISC_H_ */
