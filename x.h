#include <stdio.h>

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
unsigned xwrite(FILE *f, const void *data, unsigned n);
unsigned xread(FILE *f, void *data, unsigned n);
unsigned xread_eof(FILE *f, void *data, unsigned n);
void xseek(FILE *f, long offset, int whence);
