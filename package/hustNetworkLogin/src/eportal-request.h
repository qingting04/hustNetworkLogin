#ifndef EPORTAL_REQUEST_H
#define EPORTAL_REQUEST_H

#include <stddef.h>

/* Inputs are NUL-terminated. Returns -1 if the body does not fit, including
 * its terminator. On failure, the caller must not send the partial output. */
int eportal_build_body(char *body, size_t body_size, const char *username,
		       const char *password, const char *mac, const char *query_string);

#endif
