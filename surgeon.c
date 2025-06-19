


#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <sysexits.h>
#include <err.h>
#include <stdint.h>

#include "surgeon.h"


// OMF assumptions:
// little endian
// 32-bit numlen
// variable-length lablen
// version 2+
// labels < 64 bytes


FILE *infile;
FILE *outfile;

seg_list *star_segment;
seg_list *segments;
unsigned flag_v = 0;
unsigned errs = 0;

static uint8_t buffer[512];

uint8_t header[0x30 +10 + 64];
unsigned header_size;


// mallocs, exits on error
void *xmalloc(unsigned size) {

	void *m = malloc(size);
	if (!m) err(1, "malloc");

	return m;
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


#ifdef __ORCAC__
#define read16(base, offset) *(unsigned *)(base + offset)
#else
#define read16(base, offset) (base[offset] | (base[offset+1] << 8))
#endif


#ifdef __ORCAC__
#define read32(base, offset) *(unsigned long *)(base + offset)
#else
#define read32(base, offset) (base[offset] | (base[offset+1] << 8) | (base[offset+2] << 16) | (base[offset+3] << 24))
#endif



#ifdef __ORCAC__
#define write16(base, offset, value) *(unsigned *)(base + offset) = value
#else
#define write16(base, offset, value) do { \
	base[offset] = value & 0xff; base[offset+1] = (value >> 8) & 0xff; \
} while(0)
#endif


#ifdef __ORCAC__
#define write32(base, offset, value) *(unsigned long *)(base + offset) = value
#else
#define write32(base, offset, value) do { \
	base[offset] = value & 0xff; \
	base[offset+1] = (value >> 8) & 0xff; \
	base[offset+2] = (value >> 16) & 0xff; \
	base[offset+3] = (value >> 24) & 0xff; \
} while(0)
#endif



unsigned readstr(char *dest, const uint8_t *src) {
	unsigned n = src[0];
	if (dest) {
		if (n > 63) errx(EX_DATAERR, "string too long");
		memcpy(dest, src + 1, n);
		dest[n] = 0;
	}
	return n + 1;
}

#define HASH_TABLE_SIZE 67
static struct hash_entry *hash_table[HASH_TABLE_SIZE];

// djb hash
unsigned hash(const char *str) {

	unsigned n = 5381;
	for (unsigned i = 0; ; ++i) {
		unsigned c = str[i];
		if (!c) break;
		n = (n << 5) + n + c; /* n = n * 33 + c */
	}
	return n;
}


struct hash_entry *lookup(const char *name, unsigned insert) {

	unsigned hash_value = hash(name);
	unsigned ix = hash_value % HASH_TABLE_SIZE;
	struct hash_entry *e = hash_table[ix];
	while (e) {

		if (e->hash_value == hash_value && !strcmp(name, e->name))
			return e;

		e = e->next;
	}

	if (insert) {
		unsigned n = strlen(name);
		e = xmalloc(sizeof(hash_entry) + n + 2);
		// memset(e, 0, sizeof(hash_entry));
		e->next = hash_table[ix];
		e->hash_value = hash_value;
		e->bits = 0;
		memcpy(e->name, name, n+1);
		hash_table[ix] = e;
	}

	return e;
}



/* between segments, clear the bits */
void clear_ht(void) {
	hash_entry *e;
	unsigned i;
	for (i = 0; i < HASH_TABLE_SIZE; ++i) {
		e = hash_table[i];
		while (e) { e->bits = 0; e = e->next; }
	}
}


// todo -- need to re-think * handling.

// if this is -not- a star, zero out previous bits.
void prep_ht(struct seg_list *seg, unsigned star) {

	name_list *ptr;
	hash_entry *e;

	star = star ? IS_STAR : 0;

	// strong > weak so do weak first, then overwrite w/ strong.
	ptr = seg->weak;
	while (ptr) {
		e = lookup(ptr->name, 1);
		unsigned bits = e->bits;
		bits |= star;
		if ((bits & IS_STAR) != star) bits = 0;

		bits |= MAKE_WEAK;
		e->bits = bits;
		ptr = ptr->next;
	}

	ptr = seg->strong;
	while (ptr) {
		e = lookup(ptr->name, 1);
		unsigned bits = e->bits;
		bits |= star;
		if ((bits & IS_STAR) != star) bits = 0;

		bits &= ~MAKE_WEAK;
		bits |= MAKE_STRONG;
		e->bits = bits;
		ptr = ptr->next;
	}



	// add entries as well, in case they're also strong/weak?

	if (!star) {
		e = lookup(seg->name, 1);
		e->bits |= IS_DEFINED;
	}
}

static char name[64];

// returns the size of the expression.
unsigned process_expr(uint8_t *expr) {

	unsigned op;
	unsigned sz = 0;
	struct hash_entry *e;

	for(;;) {
		unsigned osz = sz;
		unsigned str = 0;

		op = expr[sz++];
		if (op == 0x00) return sz; // end of expression.
		if (op < 0x81) continue;

		switch(op) {
		case 0x80: break; /* current location */
		case 0x81: /* absolute value */
		case 0x87: /* relative offset */
			sz += 4;
			break;

		case 0x82: /* weak! */
		case 0x83: /* strong! */
		case 0x84: /* length attr */
		case 0x85: /* type attr */
		case 0x86: /* count attr */
			sz += readstr(name, expr + sz);
			str = 1;
			break;
		default:
			errx(EX_DATAERR, "bad omf expression opcode: $%02x", op);
		}

		if (str && (e = lookup(name, 0))) {

			if (op == 0x84 || op == 0x85 || op == 0x86) {
				if (e->bits & MAKE_WEAK) {
					warnx("unable to weaken %s", e->name);
					e->bits &= ~MAKE_WEAK;
					errs++;
				}
				e->bits |= IS_STRONG;
				continue;
			}

			if (e->bits & MAKE_WEAK) {
				if (op == 0x83) {
					if (!(e->bits & IS_DEFINED))
						expr[osz] = op = 0x82;
				}
			}

			if (e->bits & MAKE_STRONG) {
				if (op == 0x82) {
					expr[osz] = op = 0x83;
				}
			}

			if (op == 0x82) e->bits |= IS_WEAK;
			if (op == 0x83) e->bits |= IS_STRONG;

		}

	}

}





void process_omf_segment(seg_list *seg) {

	static uint8_t scratch[64 + 8];
	struct hash_entry *e;

	uint32_t bytecount = read32(header, 0);
	unsigned newSize = 0;

	uint8_t *body;
	name_list *ptr;
	unsigned delta_header = 0;

	if (bytecount > 0xc000) errx(EX_DATAERR, "omf segment too big");
	bytecount -= header_size;

	if (bytecount <= 512) {
		xread(infile, buffer, bytecount);
		body = buffer;
	} else {
		body = xmalloc(bytecount);
		xread(infile, body, bytecount);
	}

	// update kind, loadname


	// unsigned kind = read16(header, o_kind);
	if (seg->bits & SEG_KIND)
		write16(header, o_kind, seg->kind);
	if (seg->loadname) {
		unsigned disp_name = read16(header, o_displacement_name);
		memcpy(header + disp_name, seg->loadname, 10);
	}

	// store file offset, copy omf header
	unsigned long hstart = ftell(outfile);
	xwrite(outfile, header, header_size);


	// add alias entries....
	ptr = seg->alias;
	while (ptr) {
		e = lookup(ptr->name, 1);
		// check for a self-alias? if (e->bits & IS_DEFINED)
		e->bits |= IS_STRONG;
		unsigned n = strlen(ptr->name);
		scratch[0] = 0xe6;
		scratch[1] = n;
		memcpy(scratch + 2, ptr->name, n);
		n += 2;
		scratch[n++] = 0;
		scratch[n++] = 0;
		scratch[n++] = 'N';
		scratch[n++] = 0;

		newSize += xwrite(outfile, scratch, n);

		ptr = ptr->next;
	}

	// copy over the omf body.
	// $e5 strong may be dropped and expressions may be modified.
	unsigned offset = 0;
	for (;;) {
		unsigned start = offset;
		unsigned skip = 0;

		unsigned op = body[offset++];
		if (op == 0) break;
		if (op < 0xe0) {
			newSize += xwrite(outfile, body + start, op + 1);
			offset += op;
			continue;
		}

		switch (op) {
		case 0xe0: // align
		case 0xe1: // org
		case 0xf1: // ds
			offset += 4;
			break;
		case 0xe2: // reloc
		case 0xe3: // interseg
		case 0xe8: // mem
		case 0xf4: // entry (rtl)
		case 0xf5: // cReloc
		case 0xf6: // cInterseg
		case 0xf7: // super
		default:
			errx(EX_DATAERR, "unexpected omf opcode $%02x", op);
			break;

		case 0xe4: // using
		case 0xe5: // strong

			offset += readstr(name, body + offset);
			e = lookup(name, 1);

			if (e->bits & MAKE_WEAK) {
				++skip; // don't copy
			} else {
				e->bits |= IS_STRONG;
			}
			break;


		case 0xe6: // global
		case 0xef: // local [???]
			offset += readstr(name, body + offset);
			offset += 4;
			e = lookup(name, 1);
			e->bits |= IS_DEFINED;
			break;

		case 0xe7: // gequ
		case 0xf0: // equ
			// warn if weak?
			offset += readstr(NULL, body + offset);
			offset += 4;
			offset += process_expr(body + offset);
			break;

		case 0xeb: // expr
		case 0xec: // zpexpr
		case 0xed: // bkexpr
		case 0xf3: // lexpr
			offset += 1;
			offset += process_expr(body + offset);
			break;

		case 0xee: //relexpr
			offset += 5;
			offset += process_expr(body + offset);
			break;

		case 0xf2: // lconst
			offset += read32(body, offset);
			offset += 4;
			break;
		}

		if (!skip) newSize += xwrite(outfile, body + start, offset - start);
	}

	if (body != buffer) free(body);


	// add a strong-references as needed.
	// now check for strong segments...
	ptr = seg->strong;
	while (ptr) {
		e = lookup(ptr->name, 1);
		if ((e->bits & (MAKE_STRONG | IS_DEFINED)) == MAKE_STRONG) {
			unsigned n = strlen(ptr->name);
			scratch[0] = 0xe5;
			scratch[1] = n;
			memcpy(scratch + 2, ptr->name, n);
			newSize += xwrite(outfile, scratch, 2 + n);
		}
		ptr = ptr->next;
	}

	// omf end
	scratch[0] = 0;
	newSize += xwrite(outfile, scratch, 1);

	// flush it. update the header

	if (newSize != bytecount) {
		newSize += header_size;
		write32(header, 0, newSize);
		delta_header = 1;
	}

	if (delta_header) {

		xseek(outfile, hstart, SEEK_SET);
		xwrite(outfile, header, header_size);
		xseek(outfile, 0, SEEK_END);
	}


	// now, check for entry errors
	ptr = seg->alias;
	while (ptr) {
		e = lookup(ptr->name, 1);
		if (e->bits & IS_DEFINED) {
			warnx("%s already defined", e->name);
			errs++;
		}
		ptr = ptr->next;
	}

	// check for weak errors
	ptr = seg->weak;
	while (ptr) {
		e = lookup(ptr->name, 1);
		if (e->bits & IS_DEFINED) {
			warnx("unable to weaken %s", ptr->name);
			errs++;
		}
		ptr = ptr->next;
	}


}

/*
 * just copy the omf segment.
 */
void just_copy(void) {

	xwrite(outfile, header, header_size);

	uint32_t bytecount = read32(header, 0) - header_size;
	while (bytecount) {
		unsigned n = 512;
		if (bytecount < n) n = bytecount;
		xread(infile, buffer, n);
		xwrite(outfile, buffer, n);
		bytecount -= n;
	}
}


void process_omf_file(void) {

	seg_list *seg;

	for(;;) {

		unsigned n = xread_eof(infile, header, 0x2c);
		if (n == 0) return; // eof.

		if (header[o_version] != 2 || header[o_number_sex != 0]
			|| header[o_number_length] != 4 || header[o_label_length] != 0) {
			errx(EX_DATAERR, "bad/unsupported OMF file");
		}

		unsigned disp_name = read16(header, o_displacement_name);
		unsigned disp_data = read16(header, o_displacement_data);
		// unsigned kind = read16(header, o_kind);

		header_size = disp_data;
		xread(infile, header + 0x2c, disp_data - 0x2c);

		readstr(name, header + disp_name + 10); // disp_name is to the 10-char loadname.

		if (flag_v) printf("%s\n", name);

		seg = segments;
		while (seg) {
			if (!strcmp(seg->name, name)) break;
			seg = seg->next;
		}
		if (!seg) seg = star_segment;
		if (!seg) {
			if (flag_v) fputs("  copying\n", stdout);
			just_copy();
			continue;
		}

		if (seg->bits & SEG_DELETE) {
			uint32_t bytecount = read32(header, 0) - header_size;

			if (flag_v) fputs("  deleting\n", stdout);

			xseek(infile, bytecount, SEEK_CUR);
			continue;
		}


		if (flag_v) fputs("  updating\n", stdout);
		clear_ht();
		prep_ht(seg, 0);
		process_omf_segment(seg);

		#if 0
		if (star_segment == 0 && seg == 0) {
			just_copy();
			continue;
		}
		clear_ht();
		if (star_segment) prep_ht(star_segment, 1);
		if (seg) prep_ht(seg, 0);

		// pass in star seg as well...
		process_one_segment(seg);
		#endif
	}

}




void usage(unsigned ex) {

	fputs("omfsurgeon [-hv] scriptfile infile outfile\n", stdout);
	exit(ex);
}

int main(int argc, char **argv) {

	int c;
	char *cp;
	seg_list *seg;


	while ((c = getopt(argc, argv, "hv")) != -1) {

		switch(c) {
		case 'h': usage(0); break;
		// case 'o': out_file = optarg; break;
		// case 's': script_file = optarg; break;
		case 'v': ++flag_v; break;
		default: usage(EX_USAGE); break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 3) usage(EX_USAGE);

	cp = argv[0];
	FILE *f = fopen(cp, "r");
	if (!f) err(EX_NOINPUT, "fopen %s", cp);
	seg = parse_file(f);
	fclose(f);

	if (!seg) {
		errx(EX_DATAERR, "empty script");
	}

	if (!seg->name[0]) {
		star_segment = seg;
		seg = seg->next;
	}
	segments = seg;


	// iigs - check file type.

	cp = argv[1];
	infile = fopen(argv[1], "rb");
	if (!infile) err(EX_NOINPUT, "fopen %s", cp);

	cp = argv[2];
	outfile = fopen(cp, "wb");
	if (!outfile) err(EX_NOINPUT, "fopen %s", cp);

	process_omf_file();

	fclose(infile);
	fclose(outfile);

	return errs ? 1 : 0;
}