#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <sysexits.h>
#include <err.h>
#include <stdint.h>
#include <ctype.h>

#include "omf.h"
#include "x.h"
#include "read.h"


unsigned current_seg = 0;
unsigned current_pc = 0;


void usage(int ex) {
	fputs(
		"omf-relinker [-viC] input patch output\n"
		"flags:\n"
		"-v  be verbose\n"
		"-i  case insensitive\n"
		"-C  don't compress relocations\n"
		, stdout);
	exit(ex);
}


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


/*

1. load the base omf file, generate a symbol table
2. load the patch object file
3. process the patch file.
4. if expressload, generate an updated expressload segment.

*/

typedef struct entry {
	struct entry *next;
	unsigned hash;
	int level; // 0 = global, other number is local to segment.
	unsigned bits;
	unsigned seg;
	uint32_t offset;
	int shift;
	char name[];
} entry;


#define HASH_TABLE_SIZE 67
static entry *hash_table[HASH_TABLE_SIZE];


typedef struct expr_list {
	struct expr_list *next;
	unsigned type; // expr, bkexpr, etc
	unsigned size;
	unsigned offset;
	unsigned org; // for rel_expr
	unsigned level;
	uint8_t expr[]; // packed rpn expression.
} expr_list;



// (seg + offset)|-16
// seg 0 indicates constant value.
typedef struct reloc {
	unsigned seg;
	int offset;
	int shift;
} reloc;


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


/*
 local/global scope is handled by level;
*/
entry *find_entry(const char *name, int level, int insert) {

	unsigned h = hash(name);
	unsigned ix = h % HASH_TABLE_SIZE;

	entry *e = hash_table[ix];
	entry *local = 0;
	entry *global = 0;

	while (e) {
		if (e->hash == h && !strcmp(name, e->name)) {

			if (e->level == 0) global = e;
			else if (e->level == level) local = e;
		}
		e = e->next;
	}

	if (insert) {
		int gen = 0;
		 if (level == 0 && !global) gen = 1;
		 if (level && !local) gen = 1;

		if (gen) {
			int n = strlen(name) + 1;
			e = malloc(n + sizeof(entry));
			memset(e, 0, sizeof(entry));
			memcpy(e->name, name, n);
			e->hash = h;
			e->level = level;
			e->next = hash_table[ix];
			hash_table[ix] = e;
			return e;
		}
	}

	return local ? local : global;
}



static uint8_t header[0x30 + 10 + 64];

static char name[64];


int omf_version = 0;
int omf_lablen = 0;
int expressload = 0;
int compress = 0;
int super = 0;
int verbose = 0;
int insensitive = 0;


unsigned readstr(char *dest, const uint8_t *src) {

	unsigned rv, n;
	if (omf_lablen) {
		rv = n = omf_lablen;
	} else {
		n = *src++;
		rv = n + 1;
	}

	if (dest) {
		if (n > 63) errx(EX_DATAERR, "string too long");

		memcpy(dest, src, n);
		while (n && dest[n] < 0x21) --n; // strip ws
		dest[n] = 0;
	}
	return rv;
}




// returns -1 on error, expr length on success.
int eval_expr(const uint8_t *expr, struct reloc *r, int level) {

	// realistically a 2-item stack should be enough in most cases...
	#define R_STACK_SIZE 8
	static struct reloc r_stack[R_STACK_SIZE];

	unsigned op;
	unsigned offset = 0;
	unsigned s = 0;
	entry *e;

	reloc *a;
	reloc *b;
	for(;;) {
		op = expr[offset++];
		if (op == 0) break;
		switch(op) {
			case EXPR_ADD:
				if (s < 2) return -1;
				a = &r_stack[s-1];
				b = &r_stack[s-2];

				if (a->shift || b->shift) return -1;
				if (a->seg && b->seg) return -1;
				if (a->seg) b->seg = a->seg;
				b->offset += a->offset;
				--s;
				break;
			case EXPR_SUB:
				if (s < 2) return -1;
				// todo -- check order....
				a = &r_stack[s-1];
				b = &r_stack[s-2];
				if (a->shift || b->shift) return -1;
				if (a->seg && b->seg) {
					if (a->seg != b->seg) return -1;
					b->seg = 0;
					b->offset -= a->offset;
				} else {
					if (a->seg) b->seg = a->seg;
					b->offset -= a->offset;
				}
				--s;
				break;

			case EXPR_SHIFT:
				if (s < 2) return -1;
				a = &r_stack[s-1];
				b = &r_stack[s-2];

				if (a->seg || a->shift) return -1;
				if (b->seg) b->shift += a->offset;
				else {
					int n = a->offset;
					if (n < 0)
						b->offset >>= -n;
					else
						b->offset <<= n;
				}
				--s;
				break;

			case EXPR_MUL:
				if (s < 2) return -1;
				a = &r_stack[s-1];
				b = &r_stack[s-2];
				if (a->seg || b->seg) return -1;
				b->offset *= a->offset;
				--s;
				break;

			case EXPR_ABS:
				a = &r_stack[s++];
				a->seg = 0;
				a->shift = 0;
				a->offset = read32(expr, offset);
				offset += 4;
				break;
			case EXPR_PC:
				// not used by orca/m?
				a = &r_stack[s++];
				a->seg = current_seg;
				a->shift = 0;
				a->offset = current_pc;
				break;
			case EXPR_REL:
				a = &r_stack[s++];
				a->seg = current_seg;
				a->shift = 0;
				a->offset = read32(expr, offset); 
				// todo - this needs to be adjusted by the current seg offset.
				offset += 4;
				break;

			case EXPR_WEAK:
			case EXPR_LABEL:
				a = &r_stack[s++];
				offset += readstr(name, expr + offset);
				e = find_entry(name, level, 0);
				if (!e) return -1;
				if (!(e->bits & 0x01)) return -1;
				a->seg = e->seg;
				a->offset = e->offset;
				a->shift = 0;
				break;
			detault:
				errx(1, "unsupported omf expr op $%02x", op);
				return -1;
		}

		if (s == R_STACK_SIZE-1) {
			errx(1, "expression too complicated");
			return -1;
		}

	}
	if (s != 1) return -1;
	*r = r_stack[0];
	return offset;
}

int expr_size(const uint8_t *expr) {

	unsigned op;
	unsigned offset = 0;

	for(;;) {
		op = expr[offset++];
		if (op == EXPR_END) break;
		if (op < 0x16) continue;
		switch(op) {
			default:
				return -1;
			case EXPR_PC:
				break;
			case EXPR_REL:
				// todo - this needs to be adjusted by the current seg offset.

			case EXPR_ABS:
				offset += 4;
				break;
			case EXPR_WEAK:
			case EXPR_LABEL:
			case EXPR_LEN:
			case EXPR_TYPE:
			case EXPR_COUNT:
				offset += expr[offset] + 1;
				break;
		}
	}
	return offset;
}


void upcase_name(void) {
	for (unsigned i = 0; ; ++i) {
		unsigned c = name[i];
		if (!c) break;
		if (islower(c)) name[i] = toupper(c);
	}	
}

void copy_segments(FILE *infile, FILE *outfile) {


	for(;;) {
		unsigned n = xread_eof(infile, header, 0x2c);
		if (!n) break;

		if (header[o_version] < 1 || header[o_version] > 2
			|| header[o_number_sex] != 0
			|| header[o_number_length] != 4) {
			errx(EX_DATAERR, "bad/unsupported OMF file");
		}


		unsigned disp_name = read16(header, o_displacement_name);
		unsigned disp_data = read16(header, o_displacement_data);
		// unsigned kind = read16(header, o_kind);

		// header_size = disp_data;
		xread(infile, header + 0x2c, disp_data - 0x2c);

		readstr(name, header + disp_name + 10); // disp_name is to the 10-char loadname.

		unsigned segnum = read16(header, o_segment_number);

		// grr... omf v1 uses block count, but the last segment may be a partial block.

		uint32_t bytecount = read32(header, o_byte_count);
		if (omf_version == 1) bytecount *= 512;

		omf_lablen = header[o_label_length];
		if (omf_lablen > 63) errx(EX_DATAERR, "string too long");


		if (segnum == 1) {
			omf_version = header[o_version];

			if (!strcasecmp(name, "ExpressLoad")) expressload = 1;
			if (!strcasecmp(name, "~ExpressLoad")) expressload = 1;


			// if this is an expressload segment, add space for 1 extra segment, ....
			if (expressload) {

				// each segment is 8 bytes (header entry table)
				// + 2 bytes (segment number conversion)
				// + 0x2e + name bytes
				xseek(infile, bytecount - disp_data, SEEK_CUR);
				continue;
			}
		}


		// if name is printable, add it as a hash entry...
		entry *e;
		if (name[0]) {
			if (insensitive) upcase_name();
			e = find_entry(name, 0, 1);
			e->seg = segnum;
			e->bits = 1; // defined.
		}

		if (insensitive)
			snprintf(name, 64, "SEG_%u", segnum);
		else
			snprintf(name, 64, "seg_%u", segnum);

		e = find_entry(name, 0, 1);
		e->seg = segnum;
		e->bits = 1; // defined.

		if (insensitive)
			snprintf(name, sizeof(name), "SEG_%u_SIZE", segnum);
		else
			snprintf(name, sizeof(name), "seg_%u_size", segnum);

		e = find_entry(name, 0, 1);
		e->offset = read32(header, o_length);
		e->bits = 1; // defined.

		// copy to output file...
	}
}


#if 0
int sort_reloc_by_dest_offset(const reloc *a, const void *b) {
	const reloc *aa = (const reloc *)a;
	const reloc *bb = (const reloc *)b;

	return aa->dest_offset - bb->dest_offset;
}

int sort_reloc_by_seg(const reloc *a, const void *b) {
	const reloc *aa = (const reloc *)a;
	const reloc *bb = (const reloc *)b;

	int n = aa->src_seg - bb->src_seg;
	if (n == 0) n = aa->src_offset - bb->src_offset;
	if (n == 0) n = aa->dest_offset - bb->dest_offset;
	return n;
}
#endif



#define SPACE_FOR(x) if ((seg_size + (x)) <= seg_capacity) \
{ seg_capacity += 1024; seg = xrealloc(seg, seg_capacity); }


void process_patch(FILE *infile, FILE *outfile) {

	expr_list *head = 0;
	expr_list *el;

	unsigned reloc_size = 0;
	unsigned reloc_capacity = 32;
	reloc *relocs = xmalloc(sizeof(reloc) * reloc_capacity);

	unsigned seg_size = 5;
	unsigned seg_capacity = 1024;
	uint8_t *seg = xmalloc(seg_capacity);
	seg[0] = OMF_LCONST;
	seg[1] = 0;
	seg[2] = 0;
	seg[3] = 0;
	seg[4] = 0;


	omf_lablen = 0;


	// insert seg name into symbol table...

	// seg = new segnum....
	for (unsigned level = 1;;++level) {

		unsigned n = xread_eof(infile, header, 0x2c);
		if (n == 0) return; // eof.

		if (header[o_version] != 2 || header[o_number_sex] != 0
			|| header[o_number_length] != 4 || header[o_label_length] != 0) {
			errx(EX_DATAERR, "bad/unsupported OMF file");
		}

		unsigned disp_name = read16(header, o_displacement_name);
		unsigned disp_data = read16(header, o_displacement_data);
		// unsigned kind = read16(header, o_kind);

		xread(infile, header + 0x2c, disp_data - 0x2c);

		readstr(name, header + disp_name + 10); // disp_name is to the 10-char loadname.

		uint32_t bytecount = read32(header, o_byte_count);
		if (bytecount > 0xffff) errx(EX_DATAERR, "omf segment too big");

		unsigned seg_offset = seg_size - 5; // for seg start+offset math.
		if (name[0]) {
			entry *e = find_entry(name, 0, 1);
			// what if there's a name clash???
			e->bits = 1;
			e->seg = segnum;
			e->offset = seg_offset;
		}


		bytecount -= disp_data;

		uint8_t *body;
		body = xmalloc(bytecount);
		xread(infile, body, bytecount);

		unsigned offset = 0;

		for (;;) {
			uint32_t x;
			unsigned sz;
			uint32_t org;
			reloc r;
			entry *e;

			unsigned op = body[offset++];
			if (op == OMF_END) break;
			if (op < 0xe0) {
				SPACE_FOR(op)
				memcpy(seg + seg_size, body + offset, op);
				seg_size += op;
				continue;
			}

			switch(op) {
				default:
					errx(1, "unsupported omf op $%02x", op);
					break;

				case OMF_DS:
					x = read32(body, offset);
					SPACE_FOR(x)
					memset(seg + seg_size, 0, x);
					seg_size += x; 
					offset += 4;
					break;

				case OMF_EXPR:
				case OMF_ZPEXPR:
				case OMF_BKEXPR:
				case OMF_LEXPR:
				case OMF_RELEXPR:
					sz = body[offset++]; // num bytes..
					// parse the expression, copy into a list to eval later?

					org = 0;
					if (op == OMF_RELEXPR) {
						org = read32(body, offset);
						offset += 4;
					}
					x = expr_size(body + offset);

					el = xmalloc(x + sizeof(expr_list));

					memset(el, 0, sizeof(expr_list));
					memcpy(el->expr, body + offset, x);

					el->next = head;
					head = el;

					el->type = op;
					el->size = sz;
					el->offset = seg_size - 5;
					el->org = org;
					el->level = level;

					SPACE_FOR(sz);
					for (unsigned i = 0; i < sz; ++i) seg[seg_size++] = 0;

					offset += x;
					break;

				case OMF_EQU:
				case OMF_GEQU:
					x = readstr(name, body + offset);
					offset += x + 4; // length, type, private

					// if we can eval the expression, insert it now.
					// otherwise, copy and 
					x = eval_expr(body + offset, &r, level);
					if (x > 0) {
						offset += x;
						e = find_entry(name, op == OMF_EQU ? level : 0, 1);
						if (e->bits) warnx("duplicate label %s", name);
						e->bits |= 1;
						e->seg = r.seg;
						e->offset = r.offset;
						e->shift = r.shift;
					} else {
						// todo -- name might have been overwritten at this point...
						errx(1, "Unable to evaluate expression %s", name);
					}
					break;

				case OMF_STRONG:
				case OMF_USING:
					x = body[offset++];
					offset += x;
					break;

			}
		} // omf opcode

		free(body);
	} // omf segment

	unsigned reloc_offset = seg_size; // expressload needs this.

	// now evaluate all the expressions...
	el = head;
	while (el) {

		// presumably, there won't be a lot of relocs
		// so we'll skip SUPER support (which is a lot more work)
		// and just generate reloc records here and now.

		// actually... just generate the 
		reloc r;
		#if 0
		if (reloc_size == reloc_capacity) {
			reloc_capacity += 32;
			relocs = xrealloc(reloc_capacity * sizeof(reloc));
		}
		reloc *r = relocs + reloc_size;
		#endif
		int ok = eval_expr(el->expr, &r, el->level);
		if (ok < 0) errx(1, "Unable to evalute expression");

		if (r.seg == 0) {
			// patch in place...
			// todo -- bk, rel, etc.
			unsigned loc = 5 + e->offset;
			if (e->type == OMF_RELEXPR) {
				// todo ....
			}

			for (unsigned i = 0; i < el->size; ++i, r.offset >>= 8) {
				body[loc] = r.offset & 0xff;
			}
		} else if (r.seg == segnum) {
			// reloc / creloc
			SPACE_FOR(11)  // OMF_RELOC

			if (compress && r.offset < 0x10000L) {
				seg[seg_size++] = OMF_cRELOC;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write16(seg, seg_size, el->offset); seg_size += 2;
				write16(seg, seg_size, r.offset); seg_size += 2;
			} else {
				seg[seg_size++] = OMF_RELOC;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write32(seg, seg_size, el->offset); seg_size += 4;
				write32(seg, seg_size, r.offset); seg_size += 4;
			}

		} else {
			// interseg / cinterseg
			SPACE_FOR(15)

			if (compress && r.offset < 0x10000L && r.seg < 256) {
				seg[seg_size++] = OMF_cINTERSEG;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write16(seg, seg_size, el->offset); seg_size += 2;
				seg[seg_size++] = r.seg;
				write16(seg, seg_size, r.offset); seg_size += 2;
			} else {
				seg[seg_size++] = OMF_INTERSEG;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write32(seg, seg_size, el->offset); seg_size += 4;
				write16(seg, seg_size, 1); seg_size += 2; // file num
				write16(seg, seg_size, r.seg); seg_size += 2;
				write32(seg, seg_size, r.offset); seg_size += 4;
			}
		}

		expr_list *tmp = el->next;
		free(el);
		el = tmp;
	}
	head = 0;

	// symbol table not needed anymore.


	// now sort the relocs
	// not actually needed since they'll be in order 
	#if 0
	if (reloc_size)
		qsort(relocs, sizeof(reloc), reloc_size, sort_reloc);
	#endif

	SPACE_FOR(1)
	seg[seg_size++] = OMF_END;

	memset(header, 0, sizeof(header));
	if (omf_version == 1) {
		write32(header, o_byte_count, (bytecount + 511) >> 9);
		header[0x0c] = 0x10; // init segment.
	} else {
		write32(header, o_byte_count, bytecount);
		write16(header, o_kind, 0x10); // init segment.
	}
	write32(header, o_length, ...);
	header[o_label_length] = 0; // todo .. when in rome...
	header[o_number_length] = 4;
	header[o_version] = omf_version;
	write32(header, o_bank_size, 0x010000L);
	header[o_number_sex] = 0; // little-endian
	write16(header, o_segment_number, segnum);
	write16(header, o_displacement_name, 0x2c);
	write16(header, o_displacement_data, 0x2c + 10 + );
	// loadname, segname...

	// write the output file
	// if expressload, regenerate it...	
	// if version 1, pad out previous seg, 

}




int main(int argc, char **argv) {

	int c;

	verbose = 0;
	compress = 1;
	super = 0;
	insensitive = 0;

	// flag to inhibit super / compressed?
	// omf version deduced from input file...
	while ((c = getopt(argc, argv, "hiC")) != -1) {
		switch(c) {
			case 'h': usage(0); break;
			case 'i': insensitive = 1; break;
			case 'v': verbose = 1; break;
			case 'C': compress = 0; break;
			default: usage(1); break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 3) usage(1);


	FILE *infile;
	FILE *outfile;
	FILE *objfile;

	char *cp;
	cp = argv[0];
	infile = fopen(cp, "rb");
	if (!infile) errx(1, "open %s", cp);

	cp = argv[1];
	objfile = fopen(cp, "rb");
	if (!objfile) errx(1, "open %s", cp);

	cp = argv[2];
	outfile = fopen(cp, "wb");
	if (!infile) errx(1, "open %s", cp);

	copy_segments(infile, outfile);
	process_patch(objfile, outfile);
	if (expressload) process_express(infile, outfile);

	fclose(infile);
	fclose(objfile);
	fclose(outfile);
	return 0;
}
