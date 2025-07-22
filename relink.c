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


/* used by eval_expr / expr_size */
unsigned current_pc;
unsigned current_seg_offset;


#define MIN_HEADER_SIZE (0x30 + 10 + 1)
#define MAX_HEADER_SIZE (0x30 + 10 + 64)


unsigned header_size;
uint8_t header[0x30 + 10 + 64];


unsigned patch_seg;

uint32_t patch_lconst_mark;
unsigned patch_lconst_size;
uint32_t patch_reloc_mark;
unsigned patch_reloc_size;

unsigned patch_header_size;
static uint8_t patch_header[0x30 + 10 + 64];


unsigned expr_header_size;
unsigned expr_body_size;
// uint8_t expr_header[0x30 + 10 + 64];



static char name[64];


int omf_version = 0;
int omf_lablen = 0;
int expressload = 0;

int compress = 0;
int super = 0;
int verbose = 0;
int insensitive = 0;
char *segname = 0;




void usage(int ex) {
	fputs(
		"omf-relinker [-viC] [-s name] input patch output\n"
		"flags:\n"
		"-v        be verbose\n"
		"-s name   segment name\n"
		"-I        case sensitive\n"
		"-C        don't compress relocations\n"
		, stdout);
	exit(ex);
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
	unsigned disp; // displacement for rel_expr
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
		while (n && dest[n-1] < 0x21) --n; // strip ws
		dest[n] = 0;
	}
	return rv;
}




// returns -1 on error, expr length on success.
int eval_expr(const uint8_t *expr, struct reloc *r, int level) {

	static char name[64];

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
				a->seg = patch_seg;
				a->shift = 0;
				a->offset = current_pc;
				break;
			case EXPR_REL:
				a = &r_stack[s++];
				a->seg = patch_seg;
				a->shift = 0;
				a->offset = read32(expr, offset) + current_seg_offset; 
				offset += 4;
				break;

			case EXPR_WEAK:
			case EXPR_LABEL:
				a = &r_stack[s++];
				offset += readstr(name, expr + offset);
				e = find_entry(name, level, 0);
				if (!e) {
					errx(1, "Missing symbol: %s", name);
					return -1;
				}
				if (!(e->bits & 0x01)) return -1;
				a->seg = e->seg;
				a->offset = e->offset;
				a->shift = 0;
				break;
			default:
				errx(1, "Unsupported omf expr op $%02x", op);
				return -1;
		}

		if (s == R_STACK_SIZE-1) {
			errx(1, "Expression stack overflow");
			return -1;
		}

	}
	if (s != 1) {
		errx(1, "Empty expression");
		return -1;
	}
	*r = r_stack[0];
	return offset;
}

/* returns the omf expression size.  Also updates the EXPR_REL offset. */
int expr_size(uint8_t *expr) {

	unsigned op;
	unsigned offset = 0;

	for(;;) {
		uint32_t x;
		op = expr[offset++];
		if (op == EXPR_END) break;
		if (op < 0x16) continue;
		switch(op) {
			default:
				return -1;
			case EXPR_PC:
				break;
			case EXPR_REL:
				// this needs to be adjusted by the current seg offset.
				x = read32(expr, offset);
				write32(expr, offset, x + current_seg_offset);
				offset += 4;
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


/* reads the header, returns size (disp_data) */
/* returns 0 on eof */
unsigned read_header(FILE *infile, uint8_t *header) {

	unsigned n = xread_eof(infile, header, 0x2c);
	if (!n) return 0;

	if (header[o_version] < 1 || header[o_version] > 2
		|| header[o_number_sex] != 0
		|| header[o_number_length] != 4) {
		errx(EX_DATAERR, "Invalid OMF segment header");
	}

	unsigned disp_data = read16(header, o_displacement_data);
	if (disp_data < MIN_HEADER_SIZE || disp_data > MAX_HEADER_SIZE)
		errx(EX_DATAERR, "Invalid OMF segment header");

	xread(infile, header + 0x2c, disp_data - 0x2c);
	return disp_data;
}

static char buffer[512];
void copy_v2(FILE *infile, FILE *outfile, uint32_t size) {

	while (size) {
		unsigned n = size > 512 ? 512 : size;
		xread(infile, buffer, n);
		xwrite(outfile, buffer, n);
		size -= n;
	}
}

void copy_v1(FILE *infile, FILE *outfile, uint32_t blocks) {

	// last block may be partial at eof.

	while (blocks) {
		int n = fread(buffer, 1, 512, infile);
		if (n < 512 && blocks > 1) {
			errx(1, "fwrite");
		}
		xwrite(outfile, buffer, n);
		--blocks;
	}
}

void process_load_file(FILE *infile, FILE *outfile) {


	unsigned segnum = 0;
	for(segnum = 1; ; ++segnum) {

		long pos = ftell(infile);

		unsigned disp_data = read_header(infile, header);
		if (!disp_data) break;

		unsigned disp_name = read16(header, o_displacement_name);
		// unsigned kind = read16(header, o_kind);


		readstr(name, header + disp_name + 10); // disp_name is to the 10-char loadname.

		if (segnum != read16(header, o_segment_number)) {
			errx(1, "bad segnum");
		}

		uint32_t bytecount = read32(header, o_byte_count);

		omf_lablen = header[o_label_length];
		if (omf_lablen > 63) errx(EX_DATAERR, "string too long");


		if (segnum == 1) {
			omf_version = header[o_version];

			if (!strcasecmp(name, "ExpressLoad")) expressload = 1;
			if (!strcasecmp(name, "~ExpressLoad")) expressload = 1;


			if (expressload) {
				// expressload requires omf v2 so don't worry about bytecount vs block_count

				// skip the segment for now but add extra space in the outfile
				// for the expressload segment to eventually go.

				// each segment is 8 bytes (header entry table)
				// + 2 bytes (segment number conversion)
				// + 0x2e + name bytes

				expr_header_size = disp_data;
				expr_body_size = bytecount - disp_data;
				xseek(infile, bytecount, SEEK_SET);

				// overhead from 1 more segment
				unsigned n = 10 + 16 + 42;
				n += omf_lablen ? omf_lablen : strlen(segname) + 1;

				xseek(outfile, bytecount + n, SEEK_SET);
				continue;
			}
		}

		// if (omf_version == 1) bytecount *= 512;


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


		// back up so copy_vx will include the header as well.
		xseek(infile, pos, SEEK_SET);

		// copy to output file...
		if (omf_version == 1) {
			// omf v1 uses block count and segments are
			// padded to a full block.
			// However, I have encountered some examples
			// where the final block is not padded.

			copy_v1(infile, outfile, bytecount);
		} else {
			copy_v2(infile, outfile, bytecount);
		}
	}
	if (segnum == 1) errx(1,"bad omf file");
	patch_seg = segnum;
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


void process_obj_file(FILE *infile, FILE *outfile) {

	expr_list *head = 0;
	expr_list *el;

	// unsigned reloc_capacity = 32;
	// reloc *relocs = xmalloc(sizeof(reloc) * reloc_capacity);

	unsigned seg_size = 5;
	unsigned seg_capacity = 1024;
	uint8_t *seg = xmalloc(seg_capacity);
	seg[0] = OMF_LCONST;
	seg[1] = 0;
	seg[2] = 0;
	seg[3] = 0;
	seg[4] = 0;

	unsigned local_omf_lablen = omf_lablen;

	omf_lablen = 0;


	// insert seg name into symbol table...

	current_seg_offset = 0;
	current_pc = 0;

	for (unsigned level = 1;;++level) {


		unsigned disp_data = read_header(infile, header);
		if (!disp_data) break;

		if (header[o_version] != 2 || header[o_label_length] != 0) {
			errx(EX_DATAERR, "bad/unsupported OMF file");
		}

		unsigned disp_name = read16(header, o_displacement_name);
		// unsigned kind = read16(header, o_kind);

		// disp_name is to the 10-char loadname.
		readstr(name, header + disp_name + 10);

		uint32_t bytecount = read32(header, o_byte_count);
		if (bytecount > 0xffff) errx(EX_DATAERR, "omf segment too big");

		current_seg_offset = seg_size - 5;
		if (name[0]) {
			entry *e = find_entry(name, 0, 1);
			// what if there's a name clash???
			e->bits = 1;
			e->seg = patch_seg;
			e->offset = current_seg_offset;
		}


		bytecount -= disp_data;

		uint8_t *body;
		body = xmalloc(bytecount);
		xread(infile, body, bytecount);

		unsigned offset = 0;

		for (;;) {
			uint32_t x;
			unsigned sz;
			unsigned disp;
			reloc r;
			entry *e;

			unsigned op = body[offset++];
			if (op == OMF_END) break;
			if (op < 0xe0) {
				SPACE_FOR(op)
				memcpy(seg + seg_size, body + offset, op);
				seg_size += op;
				offset += op;
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

				case OMF_ALIGN:
					x = read32(body, offset);
					offset += 4;
					if (x) {
						// verify for power of 2
						if ((x & (x - 1)) == 0) {
							unsigned pc = seg_size - 5;
							unsigned sz = pc & (x-1);
							if (sz) {
								x -= sz;
								SPACE_FOR(x)
								memset(seg + seg_size, 0, x);
								seg_size += x;
							}
						}
					}
					break;

				case OMF_EXPR:
				case OMF_ZPEXPR:
				case OMF_BKEXPR:
				case OMF_LEXPR:
				case OMF_RELEXPR:
					sz = body[offset++]; // num bytes..
					// parse the expression, copy into a list to eval later?

					disp = 0;
					if (op == OMF_RELEXPR) {
						disp = read32(body, offset);
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
					el->disp = disp;
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
					// otherwise, that's a problem.
					current_pc = seg_size - 5; 
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
					// skip...
					x = body[offset++];
					offset += x;
					break;

			}
		} // omf opcode

		free(body);
	} // omf segment

	unsigned reloc_offset = seg_size; // expressload needs this.

	// now evaluate all the expressions...
	// TODO -- these are backwards due to the linked list
	// insertion order.  need to have head and tail pointers.

	el = head;
	current_seg_offset = 0;

	patch_lconst_size = seg_size - 5;
	patch_reloc_mark = seg_size; // need to adjust for absolute location.
	while (el) {

		// presumably, there won't be a lot of relocs
		// so we'll skip SUPER support (which is a lot more work)
		// and just generate reloc records here and now.

		// actually... just generate and inserts reloc here and now
		// (no super) 
		reloc r;
		#if 0
		if (reloc_size == reloc_capacity) {
			reloc_capacity += 32;
			relocs = xrealloc(reloc_capacity * sizeof(reloc));
		}
		reloc *r = relocs + reloc_size;
		#endif
		current_pc = el->offset;
		int ok = eval_expr(el->expr, &r, el->level);
		if (ok < 0) errx(1, "Unable to evalute expression");

		if (el->type == OMF_RELEXPR) {
			// special case... 
			if (r.seg != patch_seg) {
				errx(1, "bad relative relocation");
			}
			if (r.shift) errx(1, "bad relative relocation");

			// todo -- the org....
			int32_t delta = r.offset - el->offset - el->disp;
			if (el->size == 1 && (delta > 127 || delta < -128))
				errx(1, "relative branch overflow");
			// if (size == 2 && (delta > 32767 || delta < -32768))
				// errx(1, "relative branch overflow");
			// 16-bit can't overflow because it will wrap within the bank...
			r.seg = 0;
			r.offset = delta;
		}

		if (r.seg == 0) {
			// constant - patch in place...
			// todo -- bk, etc.
			unsigned loc = 5 + el->offset;

			for (unsigned i = 0; i < el->size; ++i, r.offset >>= 8) {
				seg[loc++] = r.offset & 0xff;
			}
		} else if (r.seg == patch_seg) {
			// reloc / creloc
			SPACE_FOR(11)  // OMF_RELOC

			if (compress && r.offset < 0x10000L) {
				seg[seg_size++] = OMF_cRELOC;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write16(seg, seg_size, el->offset); seg_size += 2;
				write16(seg, seg_size, r.offset); seg_size += 2;

				patch_reloc_size += 7;
			} else {
				seg[seg_size++] = OMF_RELOC;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write32(seg, seg_size, el->offset); seg_size += 4;
				write32(seg, seg_size, r.offset); seg_size += 4;

				patch_reloc_size += 11;
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

				patch_reloc_size += 8;
			} else {
				seg[seg_size++] = OMF_INTERSEG;
				seg[seg_size++] = el->size;
				seg[seg_size++] = r.shift;
				write32(seg, seg_size, el->offset); seg_size += 4;
				write16(seg, seg_size, 1); seg_size += 2; // file num
				write16(seg, seg_size, r.seg); seg_size += 2;
				write32(seg, seg_size, r.offset); seg_size += 4;

				patch_reloc_size += 15;
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

	uint32_t bytecount = seg_size; // todo - plus header size...
	memset(patch_header, 0, sizeof(patch_header));
	if (omf_version == 1) {
		write32(patch_header, o_block_count, (bytecount + 511) >> 9);
		patch_header[0x0c] = 0x10; // init segment.
	} else {
		write32(patch_header, o_byte_count, bytecount);
		write16(patch_header, o_kind, 0x10); // init segment.
	}
	write32(patch_header, o_length, reloc_offset - 5);
	patch_header[o_label_length] = local_omf_lablen;
	patch_header[o_number_length] = 4;
	patch_header[o_version] = omf_version;
	write32(patch_header, o_bank_size, 0x010000L);
	patch_header[o_number_sex] = 0; // little-endian
	write16(patch_header, o_segment_number, patch_seg);
	write16(patch_header, o_displacement_name, 0x2c);


	// update the lconst record.
	write32(seg, 1, reloc_offset - 5);

	memset(patch_header + 0x2c, ' ', 10); // load name.
	unsigned n = 0;
	if (local_omf_lablen) {
		unsigned i;
		for (i = 0; i < local_omf_lablen; ++i) {
			unsigned c = segname[i];
			if (!c) break;
			patch_header[0x2c + 10 + i] = c;
		}
		while (i < local_omf_lablen) 
			patch_header[0x2c + 10 + i++] = 0;
		n = local_omf_lablen;
	} else {
		unsigned l = strlen(segname);
		patch_header[0x2c + 10] = l;
		memcpy(patch_header + 0x2c + 10 + 1, segname, l);
		n = l + 1;
	}
	patch_header_size = 0x2c + 10 + n; 
	write16(patch_header, o_displacement_data, patch_header_size);


	long pos = ftell(outfile);

	if (omf_version == 1) {
		// pad out the previous segment to a full block.
		// this should already be done but I think I've found
		// at least one that didn't.

		memset(buffer, 0, sizeof(buffer));
		unsigned n = pos & 0x1ff;

		if (n) {

			xwrite(outfile, buffer, 512 - n);
			pos = (pos + 511) & ~511;
		}
	}

	pos += patch_header_size;
	patch_reloc_mark += pos;
	patch_lconst_mark += pos + 5;

	xwrite(outfile, patch_header, patch_header_size);
	xwrite(outfile, seg, seg_size);

	if (omf_version == 1) {
		// pad out the segment to a full block.
		pos = ftell(outfile);
		unsigned n = pos & 0x1ff;

		if (n) {
			xwrite(outfile, buffer, 512 - n);
			pos = (pos + 511) & ~511;
		}
	}

	free(seg);

}

// first segment is an expressload segment. (implies omf v2)
// we need to load the header, bump the sizes,
// insert the new entry.
void process_express(FILE *infile, FILE *outfile) {


	fseek(infile, 0, SEEK_SET);
	fseek(outfile, 0, SEEK_SET);

	uint8_t *body;
	uint8_t *p;

	unsigned disp_data = read_header(infile, header);
	// unsigned disp_name = read16(header, o_displacement_name);

	// uint32_t bytecount = read32(header, o_byte_count);


	unsigned n = 10 + 16 + 42;
	n += omf_lablen ? omf_lablen : strlen(segname) + 1;

	body = p = xmalloc(expr_body_size + n);


	uint32_t length = read32(header, o_length);
	length += n;
	write32(header, o_length, length);

	xwrite(outfile, header, disp_data);

	xread(infile, body, expr_body_size);

	// fixup the lconst record
	write32(body, 1, length);

	xwrite(outfile, p, 5);
	p += 5;

	uint8_t *bp = p;
	unsigned nseg = read16(bp, 4) + 1; // # segs - 1
	write16(bp, 4, nseg);


	bp += 6;

	// need to bump relative offsets in the header entry table by 10...
	for (unsigned i = 0; i < nseg; ++i, bp += 8) {
		unsigned n = read16(bp, 0) + 10;
		write16(bp, 0, n);
	}


	p += xwrite(outfile, p, 6 + 8 + (nseg << 3));

	// new header entry table for the patch seg...

	unsigned offset = p - body;

	unsigned rel = expr_body_size - 1 - offset;

	write16(buffer, 0, rel); // relative offset
	write16(buffer, 2, 0); // flags
	write32(buffer, 4, 0); // handle

	xwrite(outfile, buffer, 8);

	// now the segment conversion table
	p += xwrite(outfile, p, nseg << 1);
	write16(buffer, 0, patch_seg);
	xwrite(outfile, buffer, 2);

	// now the old headers....
	offset = p - body;
	unsigned sz = expr_body_size - offset - 1; // -1 for omf_end

	xwrite(outfile, p, sz);

	// new header
	write32(buffer, 0, patch_lconst_mark);
	write32(buffer, 4, patch_lconst_size);
	write32(buffer, 8, patch_reloc_mark);
	write32(buffer, 12, patch_reloc_size);

	xwrite(outfile, buffer, 16);

	xwrite(outfile, patch_header + 0x0c, patch_header_size - 0x0c);

	// omf end
	buffer[0] = OMF_END;
	xwrite(outfile, buffer, 1);
	// assert correct size? 


	free(body);
}


int main(int argc, char **argv) {

	int c;

	verbose = 0;
	compress = 1;
	super = 0;
	insensitive = 1;

	segname = "Surgeon";

	// flag to inhibit super / compressed?
	// omf version deduced from input file...
	while ((c = getopt(argc, argv, "hICs:")) != -1) {
		switch(c) {
			case 'h': usage(0); break;
			case 'I': insensitive = 0; break;
			case 'v': verbose = 1; break;
			case 'C': compress = 0; break;
			case 's': segname = optarg; break;
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

	process_load_file(infile, outfile);
	process_obj_file(objfile, outfile);
	if (expressload) process_express(infile, outfile);

	fclose(infile);
	fclose(objfile);
	fclose(outfile);
	return 0;
}
