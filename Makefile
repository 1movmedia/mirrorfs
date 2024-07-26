CFLAGS = -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Werror `pkg-config fuse3 --cflags` -D_FILE_OFFSET_BITS=64
LDLIBS = `pkg-config fuse3 --libs`

.PHONY: all clean test

mirrorfs: mirrorfs.o

clean:
	$(RM) mirrorfs mirrorfs.o

all: mirrorfs

test: all
	./test.sh
