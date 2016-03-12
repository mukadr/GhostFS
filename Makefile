CC = gcc
PROG = ghost ghost-fuse

CFLAGS  = -std=gnu99 -Wall -O2
CFLAGS += -Werror-implicit-function-declaration
CFLAGS += -Wshadow
CFLAGS += $(shell pkg-config fuse --cflags)

LDFLAGS = $(shell pkg-config fuse --libs)

OBJS  = fs.o
OBJS += md5.o
OBJS += steg.o
OBJS += steg_bmp.o
OBJS += steg_wav.o

all: $(PROG)

ghost: $(OBJS) ghost.o
	@echo "  LINK    $@"
	@$(CC) $^ $(LDFLAGS) -o $@

ghost-fuse: $(OBJS) fuse.o
	@echo "  LINK    $@"
	@$(CC) $^ $(LDFLAGS) -o $@

-include $(patsubst %.o,.%.d,$(OBJS) fuse.o ghost.o)

%.o: %.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -MMD -MF .$*.d -c $<

clean:
	rm -f $(PROG) *.o *.so .*.d

.PHONY: all clean
