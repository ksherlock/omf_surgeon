#ifndef OMF_H
#define OMF_H

#define OMF_END 0x00
#define OMF_ALIGN 0xe0
#define OMF_ORG 0xe1
#define OMF_RELOC 0xe2
#define OMF_INTERSEG 0xe3
#define OMF_USING 0xe4
#define OMF_STRONG 0xe5
#define OMF_GLOBAL 0xe6
#define OMF_GEQU 0xe7
#define OMF_MEM 0xe8
#define OMF_EXPR 0xeb
#define OMF_ZPEXPR 0xec
#define OMF_BKEXPR 0xed
#define OMF_RELEXPR 0xee
#define OMF_LOCAL 0xef
#define OMF_EQU 0xf0
#define OMF_DS 0xf1
#define OMF_LCONST 0xf2
#define OMF_LEXPR 0xf3
#define OMF_ENTRY 0xf4
#define OMF_CRELOC 0xf5
#define OMF_cRELOC 0xf5
#define OMF_CINTERSEG 0xf6
#define OMF_cINTERSEG 0xf6
#define OMF_SUPER 0xf7

#define EXPR_END 0x00
#define EXPR_ADD 0x01
#define EXPR_SUB 0x02
#define EXPR_MUL 0x03
#define EXPR_DIV 0x04
#define EXPR_MOD 0x05
#define EXPR_NEG 0x06
#define EXPR_SHIFT 0x07
#define EXPR_LAND 0x08
#define EXPR_LOR 0x09
#define EXPR_LEOR 0x0a
#define EXPR_LNOT 0x0b
#define EXPR_LE 0x0c 
#define EXPR_GE 0x0d
#define EXPR_NE 0x0e 
#define EXPR_LT 0x0f 
#define EXPR_GT 0x10 
#define EXPR_EQ 0x11 
#define EXPR_BAND 0x12
#define EXPR_BOR 0x13
#define EXPR_BEOR 0x14
#define EXPR_BNOT 0x15
    
#define EXPR_PC 0x80
#define EXPR_ABS 0x81
#define EXPR_WEAK 0x82
#define EXPR_LABEL 0x83
#define EXPR_LEN 0x84
#define EXPR_TYPE 0x85
#define EXPR_COUNT 0x86
#define EXPR_REL 0x87


enum {
	// omf header displacements i care about
	o_byte_count = 0, // v 2+
	o_block_count = 0, // v0/1
	o_length = 8,
	o_type = 0x0c, // v0/v1
	o_label_length = 0x0d,
	o_number_length = 0x0e,
	o_version = 0x0f,
	o_bank_size = 0x10,
	o_kind = 0x14, // v2+
	o_origin = 0x18,
	o_alignment = 0x1c,
	o_number_sex = 0x20,
	o_segment_number = 0x22,
	o_entry = 0x24,
	o_displacement_name = 0x28,
	o_displacement_data = 0x2a
};

#endif
