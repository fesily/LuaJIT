##############################################################################
# LuaJIT coroutine hook extension tests.
#
# Usage: make -C tests [XCFLAGS=...] [HAS_COROHOOK=0|1]
#
# Links against ../src/libluajit.a (built on demand). The hook cases are only
# built when the library carries the extension: pass the same XCFLAGS as the
# src build, or set HAS_COROHOOK=0 explicitly for a build made with
# -DLUAJIT_DISABLE_COROHOOK.
#
# Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
##############################################################################

CC?=		cc
RM?=		rm -f

LIB=		../src/libluajit.a
BIN=		corohook-tests

CCWARN=		-Wall -Wextra
CCOPT=		-O2 -g
CFLAGS+=	$(CCWARN) $(CCOPT) -I. -I../src
LDFLAGS=	-lm

##############################################################################

ifneq (,$(findstring LUAJIT_DISABLE_COROHOOK,$(XCFLAGS)))
HAS_COROHOOK?=	0
else
HAS_COROHOOK?=	1
endif

SRC=		main.c test.c t_plain.c
ifeq ($(HAS_COROHOOK),0)
CFLAGS+=	-DTEST_HAS_COROHOOK=0
else
CFLAGS+=	-DTEST_HAS_COROHOOK=1
SRC+=		t_api.c t_lua.c t_stress.c t_inspect.c
endif
OBJ=		$(SRC:.c=.o)

##############################################################################

default all: $(BIN)
	@echo "==== Running coroutine hook tests ===="
	./$(BIN)

build: $(BIN)

$(BIN): $(OBJ) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LIB) $(LDFLAGS)

# The library build also generates ../src/luajit.h, so objects depend on it.
$(OBJ): %.o: %.c test.h $(LIB)
	$(CC) $(CFLAGS) -c -o $@ $<

# Build the library with the requested configuration if it is missing.
# MACOSX_DEPLOYMENT_TARGET is forwarded because src/Makefile requires it.
$(LIB):
	$(MAKE) -C ../src XCFLAGS="$(XCFLAGS)" \
	  MACOSX_DEPLOYMENT_TARGET="$(MACOSX_DEPLOYMENT_TARGET)"

clean:
	$(RM) $(BIN) *.o

.PHONY: default all build clean
