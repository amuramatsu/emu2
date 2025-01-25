SHELL=/bin/sh
AR?=ar
CFLAGS?=-O3 -I. -DEMS_SUPPORT
CFLAGS_IA32=-DIA32_INSTRUCTION_TRACE -Wno-unused-value -Wno-unused-but-set-variable -Wno-unused-function -Wno-unused-label -Wno-pointer-sign

include ../../platform.mk

LIBNAME = libia32.a
SRCS = $(shell find . -name '*.c')
OBJS = $(notdir $(SRCS:.c=.o))

.PHONY: all
all: $(LIBNAME)

$(LIBNAME): $(OBJS:%=obj/%)
	echo $(OBJS)
	-rm $(LIBNAME)
	$(AR) -q $@ $^ 

obj/%.o: %.c | obj
	$(CC) $(CFLAGS) $(CFLAGS_IA32) -c -o $@ $<
obj/%.o: */%.c | obj
	$(CC) $(CFLAGS) $(CFLAGS_IA32) -c -o $@ $<
obj/%.o: */*/%.c | obj
	$(CC) $(CFLAGS) $(CFLAGS_IA32) -c -o $@ $<
obj/%.o: */*/*/%.c | obj
	$(CC) $(CFLAGS) $(CFLAGS_IA32) -c -o $@ $<
obj/%.o: */*/*/*/%.c | obj
	$(CC) $(CFLAGS) $(CFLAGS_IA32) -c -o $@ $<
obj:
	mkdir -p obj

.PHONY: clean distclean
clean distclean:
	rm -f $(OBJS:%=obj/%) $(LIBNAME)
	test -d obj && rmdir obj || true

include depend.mk
