
#define read8(base, offset) base[offset]
#define write8(base, offset, value) base[offset] = (value) & 0xff

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
#define write16(base, offset, value) *(unsigned *)(base + offset) = (value)
#else
#define write16(base, offset, value) do { \
	uint16_t _value = (value); \
	base[offset] = _value & 0xff; base[offset+1] = (_value >> 8) & 0xff; \
} while(0)
#endif


#ifdef __ORCAC__
#define write32(base, offset, value) *(unsigned long *)(base + offset) = (value)
#else
#define write32(base, offset, value) do { \
	uint32_t _value = (value); \
	base[offset] = _value & 0xff; \
	base[offset+1] = (_value >> 8) & 0xff; \
	base[offset+2] = (_value >> 16) & 0xff; \
	base[offset+3] = (_value >> 24) & 0xff; \
} while(0)
#endif
