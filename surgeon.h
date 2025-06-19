


enum {
	IS_DEFINED = 1,
	IS_STRONG = 2,
	IS_WEAK = 4,

	MAKE_STRONG = 0x10,
	MAKE_WEAK = 0x20,
	MAKE_ENTRY = 0x40,

	IS_STAR = 0x8000, 
};

enum {
	SEG_DELETE = 1
};


typedef struct hash_entry {
	struct hash_entry *next;
	unsigned hash_value;

	unsigned bits;

	char name[];
} hash_entry;


// TODO -- namelist could have a link to the corresponding hash_entry to save a lookup.
typedef struct name_list {
	struct name_list *next;
	char name[];
} name_list;

typedef struct seg_list {
	struct seg_list *next;
	struct name_list *alias;
	struct name_list *strong;
	struct name_list *weak;
	unsigned bits;
	char name[];
} seg_list;





void *xmalloc(unsigned size);
struct seg_list *parse_file(FILE *f);

enum {
	// omf header displacements i care about
	o_byte_count = 0,
	o_label_length = 0x0d,
	o_number_length = 0x0e,
	o_version = 0x0f,
	o_kind = 0x14,
	o_number_sex = 0x20,
	o_displacement_name = 0x28,
	o_displacement_data = 0x2a
};