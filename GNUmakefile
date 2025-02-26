# -*- makefile-gmake -*-
ifeq ($(IA32),1)
TARGET=emu2-ia32
OBJDIR=obj.ia32
CFLAGS?=-O3 -DEMS_SUPPORT -DIA32
else
TARGET=emu2
OBJDIR=obj
CFLAGS?=-O3 -DEMS_SUPPORT
endif
SHELL=/bin/sh
LDLIBS?=-liconv -lm
INSTALL?=install
PREFIX?=/usr

include platform.mk

OBJS=\
 codepage.o\
 dbg.o\
 dosnames.o\
 dos.o\
 keyb.o\
 loader.o\
 main.o\
 timer.o\
 utils.o\
 video.o\
 ems.o\
 extmem.o\
 pic.o\

ifneq ($(IA32),1)
OBJS+=\
 cpu.o\
 dis.o\

endif

.PHONY: all
all: $(TARGET)

ifeq ($(IA32),1)
$(TARGET): $(OBJS:%=$(OBJDIR)/%)
	(cd src/i386c && ./mk_depend.sh && $(MAKE))
	$(CC) -o $@ $^ $(LDFLAGS) -L./src/i386c -lia32 $(LDLIBS)
else
$(TARGET): $(OBJS:%=$(OBJDIR)/%)
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)
endif

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OBJDIR):
	mkdir -p $(OBJDIR)

.PHONY: clean distclean
ifeq ($(IA32),1)
clean distclean:
	rm -f .test.c .test.out $(OBJS:%=$(OBJDIR)/%) $(TARGET)
	(cd src/i386c && $(MAKE) clean)
	test -d $(OBJDIR) && rmdir $(OBJDIR) || true
else
clean distclean:
	rm -f .test.c .test.out $(OBJS:%=$(OBJDIR)/%) $(TARGET)
	test -d $(OBJDIR) && rmdir $(OBJDIR) || true
endif

.PHONY: install
install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)${PREFIX}/bin
	$(INSTALL) -s $(TARGET) $(DESTDIR)${PREFIX}/bin

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)${PREFIX}/bin/$(TARGET)

# Generated with gcc -MM src/*.c
$(OBJDIR)/codepage.o: src/codepage.c src/codepage.h src/dbg.h src/os.h src/env.h
$(OBJDIR)/cpu.o: src/cpu.c src/cpu.h src/dbg.h src/os.h src/dis.h src/emu.h
$(OBJDIR)/dbg.o: src/dbg.c src/dbg.h src/os.h src/env.h src/version.h
$(OBJDIR)/dis.o: src/dis.c src/dis.h src/emu.h
$(OBJDIR)/dos.o: src/dos.c src/dos.h src/os.h src/codepage.h src/dbg.h \
  src/dosnames.h src/emu.h src/env.h src/keyb.h src/loader.h src/timer.h \
  src/utils.h src/video.h src/ems.h src/extmem.h
$(OBJDIR)/dosnames.o: src/dosnames.c src/dosnames.h src/dbg.h src/os.h src/emu.h \
  src/env.h src/codepage.h
$(OBJDIR)/ems.o: src/ems.c src/ems.h src/emu.h src/dbg.h src/os.h
$(OBJDIR)/extmem.o: src/extmem.c src/extmem.h src/emu.h src/dbg.h src/os.h \
  src/env.h
$(OBJDIR)/keyb.o: src/keyb.c src/keyb.h src/codepage.h src/dbg.h src/os.h src/emu.h \
  src/extmem.h
$(OBJDIR)/loader.o: src/loader.c src/loader.h src/dbg.h src/os.h src/emu.h \
  src/dosnames.h
$(OBJDIR)/main.o: src/main.c src/dbg.h src/os.h src/dos.h src/dosnames.h src/emu.h \
  src/env.h src/keyb.h src/timer.h src/video.h src/extmem.h src/pic.h
$(OBJDIR)/pic.o: src/pic.c src/pic.h src/dbg.h src/os.h
$(OBJDIR)/timer.o: src/timer.c src/timer.h src/dbg.h src/os.h src/emu.h
$(OBJDIR)/utils.o: src/utils.c src/utils.h src/dbg.h src/os.h
$(OBJDIR)/video.o: src/video.c src/video.h src/codepage.h src/dbg.h src/os.h \
  src/emu.h src/env.h src/keyb.h
