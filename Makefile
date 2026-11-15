CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
VERSION ?= 0.1
PKGS    := libmpg123

EXE :=
ifneq (,$(findstring mingw,$(CC)))
EXE := .exe
endif

CFLAGS  += -DSENSME_VERSION='"$(VERSION)"'
CFLAGS  += $(shell pkg-config --cflags $(PKGS))

ifeq ($(STATIC),1)
LDFLAGS += -static
PKG_LIBS := $(shell pkg-config --libs --static $(PKGS))
else
PKG_LIBS := $(shell pkg-config --libs $(PKGS))
endif
LDLIBS  += $(PKG_LIBS) -lm

PREFIX  ?= /usr/local
BINDIR  := $(PREFIX)/bin

all: sensme$(EXE)

sensme$(EXE): sensme.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: sensme$(EXE)
	install -Dm755 sensme$(EXE) $(DESTDIR)$(BINDIR)/sensme$(EXE)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/sensme

clean:
	rm -f sensme sensme.exe

.PHONY: all install uninstall clean
