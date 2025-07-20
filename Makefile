
CFLAGS += -Wall -g

.phony: all

all: omf_surgeon omf_relink

omf_surgeon : surgeon.o parse.o x.o
	$(CC) -o $@ $^

omf_relink : relink.o x.o
	$(CC) -o $@ $^

surgeon.o : surgeon.c surgeon.h x.h read.h
relink.o : relink.c x.h read.h
parse.o : parse.c surgeon.h x.h
x.o : x.c x.h



.PHONY: clean
clean:
	$(RM) omf_surgeon surgeon.o parse.o

