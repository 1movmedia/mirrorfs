CFLAGS = -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Werror -D_FILE_OFFSET_BITS=64 `pkg-config fuse --cflags`
LDLIBS = `pkg-config fuse --libs`

.PHONY: all clean test

mirrorfs: mirrorfs.o

clean:
	$(RM) mirrorfs mirrorfs.o

all: mirrorfs

test: all
	./test.sh
