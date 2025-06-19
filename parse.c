#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "surgeon.h"

static char buffer[256];

static char label[64];
static unsigned label_length;
static unsigned ix;
static unsigned line;

enum {
	TK_EOF = 0,
	TK_SEMI,
	TK_COMMA,
	TK_STAR,
	TK_LEFT_BRACKET,
	TK_RIGHT_BRACKET,

	TK_LABEL,
	TK_STRONG,
	TK_SEGMENT,
	TK_WEAK,
	TK_ALIAS,
	// TK_TYPE,
	// TK_CODE,
	// TK_DATA
};


int is_keyword(const char *cp) {
	unsigned c = *cp;
	if (c == 's' && !strcmp(cp, "strong")) return TK_STRONG;
	if (c == 's' && !strcmp(cp, "segment")) return TK_SEGMENT;
	if (c == 'w' && !strcmp(cp, "weak")) return TK_WEAK;
	if (c == 'a' && !strcmp(cp, "alias")) return TK_ALIAS;
	// if (c == 't' && !strcmp(cp, "type")) return TK_TYPE;
	// if (c == 'c' && !strcmp(cp, "code")) return TK_CODE;
	// if (c == 'd' && !strcmp(cp, "data")) return TK_DATA;
	return TK_LABEL;
}


int refill(FILE *f) {

	for(;;) {
		char *cp = fgets(buffer, sizeof(buffer)-1, f);
		++line;
		buffer[255] = 0;
		if (!cp) {
			if (ferror(f)) return -1;
			if (feof(f)) return 0;
		}
		return 1;
	}
}

void parse_err(const char *what) {
	errx(1, "line %u: %s", line, what);
}

int next_token(FILE *f, unsigned st) {


	for(;;) {

		unsigned c = buffer[ix++];
		if (c == 0 || c == '#') {
			int ok = refill(f);
			if (ok <= 0) return ok;
			ix = 0;
			continue;
		}
		if (isspace(c)) continue;

		if (c == '{') return TK_LEFT_BRACKET;
		if (c == '}') return TK_RIGHT_BRACKET;
		if (c == '*') return TK_STAR;
		if (c == ',') return TK_COMMA;
		if (c == ';') return TK_SEMI;

		if (c == '"') {
			unsigned i = 0;
			for(;;) {
				c = buffer[ix++];
				if (c < 0x20) parse_err("bad label");
				if (c == '"') {
					if (i == 0) parse_err("empty label");
					label[i] = 0;
					label_length = i;
					return TK_LABEL;
				}
				label[i++] = c;
				if (i >= 63) parse_err("label too long");
			}
		}
		if (isalpha(c) || c == '~' || c == '_' ) {
			unsigned i = 1;
			label[0] = c;
			for(;;) {
				c = buffer[ix++];
				if (isalpha(c) || isdigit(c) || c == '_' || c == '~') {
					label[i++] = c;
					if (i >= 63) parse_err("label too long");
					continue;
				}
				break;
			}
			ix--;
			label[i] = 0;
			label_length = i;
			return st ? is_keyword(label) : TK_LABEL;
		}

		parse_err("bad char");
	}
}

void expected(const char *what) {
	errx(1, "line %u: expected %s", line, what);
}

int expect(FILE *f, unsigned st, int type, const char *what) {
	int tk = next_token(f, st);
	if (tk == type) return tk;
	errx(1, "line %u: expected %s", line, what);
}



struct seg_list *parse_file(FILE *f) {

	line = 0;
	ix = 0;
	buffer[0] = 0;

	struct seg_list *star_seg = 0;
	struct seg_list *segments = 0;


	for(;;) {

		int tk;
		int type;

		tk = next_token(f, 1);
		if (tk == TK_SEMI) continue; // optional ; after }
		if (tk == TK_EOF) break; // eof

		if (tk != TK_SEGMENT) expected("segment or eof");

		tk = next_token(f, 0);
		if (tk != TK_STAR && tk != TK_LABEL) expected("label");
		type = tk;

		expect(f, 0, TK_LEFT_BRACKET, "{");

		seg_list *seg = 0;
		switch (type) {
		case TK_STAR:
			if (star_seg) seg = star_seg;
			else {
				seg = star_seg = xmalloc(sizeof(seg_list) + 1);
				memset(seg, 0, sizeof(seg_list));
				seg->name[0] = '0';
			}
			break;

		case TK_LABEL:
			// error if already defined...
			seg = segments;
			while (seg) {
				if (!strcmp(label, seg->name)) errx(1, "duplicate segment %s", label);
				seg = seg->next;
			}
			seg = xmalloc(sizeof(seg_list) + 1 + label_length);
			memset(seg, 0, sizeof(seg_list));
			memcpy(seg->name, label, label_length + 1);
			seg->next = segments;
			segments = seg;
		}


		for(;;) {


			tk = next_token(f, 1);
			if (tk == TK_RIGHT_BRACKET) break;
			int saved = tk;

			if (tk == TK_ALIAS && type == TK_STAR) {
				warnx("%u: alias is ignored for * segments", line);
			}

			name_list *head = 0;

			switch (tk) {
			case TK_STRONG: head = seg->strong; break;
			case TK_WEAK: head = seg->weak; break;
			case TK_ALIAS: head = seg->alias; break;
			default:
				expected("strong/weak/alias");
			}

			// list of labels;

			for(;;) {

				expect(f, 0, TK_LABEL, "label");
				// add to the list...

				name_list *e = xmalloc(sizeof(name_list) + 1 + label_length);
	
				memcpy(e->name, label, label_length + 1);
				e->next = head;
				head = e;

				tk = next_token(f, 0);
				if (tk == TK_SEMI) break;
				if (tk == TK_COMMA) continue;
				expected(", or ;");
			}

			switch(saved) {
			case TK_STRONG: seg->strong = head; break;
			case TK_WEAK: seg->weak = head; break;
			case TK_ALIAS: seg->alias = head; break;
			}

		}

	}

	if (star_seg) {
		star_seg->next = segments;
		return star_seg;
	}
	return segments;
}