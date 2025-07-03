
CFLAGS += -Wall -g

omf_surgeon : surgeon.o parse.o
	$(CC) -o $@ $^
surgeon.o : surgeon.c surgeon.h
parse.o : parse.c surgeon.h

.PHONY: clean
clean:
	$(RM) omf_surgeon surgeon.o parse.o

