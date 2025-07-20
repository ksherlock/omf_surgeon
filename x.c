#include "x.h"

#include <stdio.h>
#include <stdlib.h>
#include <err.h>

#ifdef __ORCAC__
#pragma noroot
#endif

void *xrealloc(void *ptr, size_t size) {
	void * tmp = realloc(ptr, size);
	if (!tmp) err(1, "realloc");
	return tmp;
}

void *xmalloc(size_t size) {
	void *tmp = malloc(size);
	if (!tmp) err(1, "malloc");
	return tmp;
}


unsigned xwrite(FILE *f, const void *data, unsigned n) {
	unsigned nn = fwrite(data, 1, n, f);
	if (nn != n) errx(1, "fwrite");
	return n;
}

unsigned xread(FILE *f, void *data, unsigned n) {
	unsigned nn = fread(data, 1, n, f);
	if (nn != n) errx(1, "fread");
	return n;
}


unsigned xread_eof(FILE *f, void *data, unsigned n) {
	unsigned nn = fread(data, 1, n, f);
	if (nn == 0 || nn == n) return nn;
	errx(1, "fread");
	return 0;
}

void xseek(FILE *f, long offset, int whence) {
	int ok = fseek(f, offset, whence);
	if (ok < 0) err(1, "fseek");
}

