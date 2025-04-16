# makefile for building dalua

# Warnings valid for both C and C++
CWARNSCPP= \
	-pedantic \
	-Wextra \
	-Wshadow \
	-Wsign-compare \
	-Wundef \
	-Wwrite-strings \
	-Wredundant-decls \
	-Wdisabled-optimization \
	-Waggregate-return \
	-Wdouble-promotion \
	#-Wno-aggressive-loop-optimizations   # not accepted by clang \
	#-Wlogical-op   # not accepted by clang \
	# the next warnings generate too much noise, so they are disabled
	# -Wconversion  -Wno-sign-conversion \
	# -Wsign-conversion \
	# -Wconversion \
	# -Wstrict-overflow=2 \
	# -Wformat=2 \
	# -Wcast-qual \

# The next warnings are neither valid nor needed for C++
CWARNSC= -Wdeclaration-after-statement \
	-Wmissing-prototypes \
	-Wnested-externs \
	-Wstrict-prototypes \
	-Wc++-compat \
	-Wold-style-definition \


CWARNS= $(CWARNSCPP)  $(CWARNSC)


# -DEXTERNMEMCHECK -DHARDSTACKTESTS -DHARDMEMTESTS -DTRACEMEM='"tempmem"'
# -g -DLUA_USER_H='"ltests.h"'
# -pg -malign-double
# -DLUA_USE_CTYPE -DLUA_USE_APICHECK
# (in clang, '-ftrapv' for runtime checks of integer overflows)
# -fsanitize=undefined -ftrapv
TESTS= -DLUA_USER_H='"ltests.h"'

# -mtune=native -fomit-frame-pointer
# -fno-stack-protector
LOCAL = $(TESTS) $(CWARNS) -g



# enable Linux goodies
MYCFLAGS= $(LOCAL) -std=c99 -DLUA_USE_LINUX -DLUA_COMPAT_5_2
MYLDFLAGS= $(LOCAL) -Wl,-E
MYLIBS= -ldl -lreadline


CC= gcc
CFLAGS= -Wall -O2 $(MYCFLAGS)
AR= ar rc
RANLIB= ranlib
RM= rm -f



LIBS = -lm

CORE_T=	liblua.a
CORE_O=	lapi.o lcode.o lctype.o ldebug.o ldo.o ldump.o lfunc.o lgc.o llex.o \
	lmem.o lobject.o lopcodes.o lparser.o lstate.o lstring.o ltable.o \
	ltm.o lundump.o lvm.o lzio.o ltests.o
AUX_O=	lauxlib.o
LIB_O=	lbaselib.o ldblib.o liolib.o lmathlib.o loslib.o ltablib.o lstrlib.o \
	lutf8lib.o lbitlib.o loadlib.o lcorolib.o linit.o

LUA_T=	lua
LUA_O=	lua.o

# LUAC_T=	luac
# LUAC_O=	luac.o print.o

ALL_T= $(CORE_T) $(LUA_T) $(LUAC_T)
ALL_O= $(CORE_O) $(LUA_O) $(LUAC_O) $(AUX_O) $(LIB_O)
ALL_A= $(CORE_T)

################################################################################
# Usage:
#  - `make` without any targets specified

# For cross-compiling for a remote ARM32 device,
# - Defined LUA_32BITS to ensure 32-bit integers and 32-bit floats.
# - Modified size_t in ldump.c to be 32-bit.

export TOP_DIR ?= $(CURDIR)

.PHONY: .build
.DEFAULT_GOAL = .build

# When the makefile is first executed, .build is set as the only target, and then the
# makefile is re-invoked in the .build subdirectory with the specified targets, dalua and
# daluac.
.build:
	mkdir -p $@
	$(MAKE) -C $@ -f $(TOP_DIR)/makefile dalua daluac

CXX = g++

CFLAGS = -O2 -Os -flto=auto -Wall -Wextra -DLUA_32BITS
CFLAGS += -march=native -ffunction-sections -fdata-sections
LDFLAGS = -flto=auto -Wl,--gc-sections -Wl,--strip-all

LIBS = -lm -lreadline

ifeq ($(OS), Windows_NT)
SERIAL = serial_win
LIBS += -lserialport
else
SERIAL = serial_linux
CFLAGS += -DLUA_USE_LINUX
endif

CXXFLAGS += $(CFLAGS)
CXXFLAGS += -std=c++17  # for C++ features such as inline (const) variables
CXXFLAGS += -fno-exceptions
CXXFLAGS += -fno-ms-extensions
CXXFLAGS += -fno-rtti  # We don't need RTTI as no ambiguous base classes are used.
CXXFLAGS += -fno-threadsafe-statics

# All source files are in the upper directory.
vpath % $(TOP_DIR)

dalua: dalua.o darepl.o option.o serial_common.o $(SERIAL).o $(CORE_O) lauxlib.o
	$(CXX) -o $@ $(LDFLAGS) $^ $(LIBS)

daluac: daluac.o dacomp.o fletcher32.o $(CORE_O) lauxlib.o
	$(CC) -o $@ $(LDFLAGS) $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

dacomp.o: dacomp.c dacomp.h

dalua.o: dalua.cpp darepl.hpp lua.hpp option.hpp

daluac.o: daluac.c dacomp.h

darepl.o: darepl.cpp darepl.hpp lua.hpp option.hpp serial_common.hpp $(SERIAL).hpp

fletcher32.o: checksum/fletcher32.c checksum/fletcher32.h
	$(CC) $(CFLAGS) -I$(TOP_DIR) -c $< -o $@

option.o: option.cpp lua.hpp option.hpp

serial_common.o: serial_common.hpp

$(SERIAL).o: $(SERIAL).cpp lua.hpp option.hpp serial_common.hpp $(SERIAL).hpp
################################################################################

all:	$(ALL_T)

o:	$(ALL_O)

a:	$(ALL_A)

$(CORE_T): $(CORE_O) $(AUX_O) $(LIB_O)
	$(AR) $@ $?
	$(RANLIB) $@

$(LUA_T): $(LUA_O) $(CORE_T)
	$(CC) -o $@ $(MYLDFLAGS) $(LUA_O) $(CORE_T) $(LIBS) $(MYLIBS) $(DL)

$(LUAC_T): $(LUAC_O) $(CORE_T)
	$(CC) -o $@ $(MYLDFLAGS) $(LUAC_O) $(CORE_T) $(LIBS) $(MYLIBS)

clean:
	rcsclean -u
	$(RM) $(ALL_T) $(ALL_O)

depend:
	@$(CC) $(CFLAGS) -MM *.c

echo:
	@echo "CC = $(CC)"
	@echo "CFLAGS = $(CFLAGS)"
	@echo "AR = $(AR)"
	@echo "RANLIB = $(RANLIB)"
	@echo "RM = $(RM)"
	@echo "MYCFLAGS = $(MYCFLAGS)"
	@echo "MYLDFLAGS = $(MYLDFLAGS)"
	@echo "MYLIBS = $(MYLIBS)"
	@echo "DL = $(DL)"

# $(ALL_O): makefile

# DO NOT EDIT
# automatically made with 'gcc -MM l*.c'

lapi.o: lapi.c lprefix.h lua.h luaconf.h lapi.h llimits.h lstate.h \
 lobject.h ltm.h lzio.h lmem.h ldebug.h ldo.h lfunc.h lgc.h lstring.h \
 ltable.h lundump.h lvm.h
lauxlib.o: lauxlib.c lprefix.h lua.h luaconf.h lauxlib.h
lbaselib.o: lbaselib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lbitlib.o: lbitlib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lcode.o: lcode.c lprefix.h lua.h luaconf.h lcode.h llex.h lobject.h \
 llimits.h lzio.h lmem.h lopcodes.h lparser.h ldebug.h lstate.h ltm.h \
 ldo.h lgc.h lstring.h ltable.h lvm.h
lcorolib.o: lcorolib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lctype.o: lctype.c lprefix.h lctype.h lua.h luaconf.h llimits.h
ldblib.o: ldblib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
ldebug.o: ldebug.c lprefix.h lua.h luaconf.h lapi.h llimits.h lstate.h \
 lobject.h ltm.h lzio.h lmem.h lcode.h llex.h lopcodes.h lparser.h \
 ldebug.h ldo.h lfunc.h lstring.h lgc.h ltable.h lvm.h
ldo.o: ldo.c lprefix.h lua.h luaconf.h lapi.h llimits.h lstate.h \
 lobject.h ltm.h lzio.h lmem.h ldebug.h ldo.h lfunc.h lgc.h lopcodes.h \
 lparser.h lstring.h ltable.h lundump.h lvm.h
ldump.o: ldump.c lprefix.h lua.h luaconf.h lobject.h llimits.h lstate.h \
 ltm.h lzio.h lmem.h lundump.h
lfunc.o: lfunc.c lprefix.h lua.h luaconf.h lfunc.h lobject.h llimits.h \
 lgc.h lstate.h ltm.h lzio.h lmem.h
lgc.o: lgc.c lprefix.h lua.h luaconf.h ldebug.h lstate.h lobject.h \
 llimits.h ltm.h lzio.h lmem.h ldo.h lfunc.h lgc.h lstring.h ltable.h
linit.o: linit.c lprefix.h lua.h luaconf.h lualib.h lauxlib.h
liolib.o: liolib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
llex.o: llex.c lprefix.h lua.h luaconf.h lctype.h llimits.h ldebug.h \
 lstate.h lobject.h ltm.h lzio.h lmem.h ldo.h lgc.h llex.h lparser.h \
 lstring.h ltable.h
lmathlib.o: lmathlib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lmem.o: lmem.c lprefix.h lua.h luaconf.h ldebug.h lstate.h lobject.h \
 llimits.h ltm.h lzio.h lmem.h ldo.h lgc.h
loadlib.o: loadlib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lobject.o: lobject.c lprefix.h lua.h luaconf.h lctype.h llimits.h \
 ldebug.h lstate.h lobject.h ltm.h lzio.h lmem.h ldo.h lstring.h lgc.h \
 lvm.h
lopcodes.o: lopcodes.c lprefix.h lopcodes.h llimits.h lua.h luaconf.h
loslib.o: loslib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lparser.o: lparser.c lprefix.h lua.h luaconf.h lcode.h llex.h lobject.h \
 llimits.h lzio.h lmem.h lopcodes.h lparser.h ldebug.h lstate.h ltm.h \
 ldo.h lfunc.h lstring.h lgc.h ltable.h
lstate.o: lstate.c lprefix.h lua.h luaconf.h lapi.h llimits.h lstate.h \
 lobject.h ltm.h lzio.h lmem.h ldebug.h ldo.h lfunc.h lgc.h llex.h \
 lstring.h ltable.h
lstring.o: lstring.c lprefix.h lua.h luaconf.h ldebug.h lstate.h \
 lobject.h llimits.h ltm.h lzio.h lmem.h ldo.h lstring.h lgc.h
lstrlib.o: lstrlib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
ltable.o: ltable.c lprefix.h lua.h luaconf.h ldebug.h lstate.h lobject.h \
 llimits.h ltm.h lzio.h lmem.h ldo.h lgc.h lstring.h ltable.h lvm.h
ltablib.o: ltablib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
ltests.o: ltests.c lprefix.h lua.h luaconf.h lapi.h llimits.h lstate.h \
 lobject.h ltm.h lzio.h lmem.h lauxlib.h lcode.h llex.h lopcodes.h \
 lparser.h lctype.h ldebug.h ldo.h lfunc.h lstring.h lgc.h ltable.h \
 lualib.h
ltm.o: ltm.c lprefix.h lua.h luaconf.h ldebug.h lstate.h lobject.h \
 llimits.h ltm.h lzio.h lmem.h ldo.h lstring.h lgc.h ltable.h lvm.h
lua.o: lua.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lundump.o: lundump.c lprefix.h lua.h luaconf.h ldebug.h lstate.h \
 lobject.h llimits.h ltm.h lzio.h lmem.h ldo.h lfunc.h lstring.h lgc.h \
 lundump.h
lutf8lib.o: lutf8lib.c lprefix.h lua.h luaconf.h lauxlib.h lualib.h
lvm.o: lvm.c lprefix.h lua.h luaconf.h ldebug.h lstate.h lobject.h \
 llimits.h ltm.h lzio.h lmem.h ldo.h lfunc.h lgc.h lopcodes.h lstring.h \
 ltable.h lvm.h
lzio.o: lzio.c lprefix.h lua.h luaconf.h llimits.h lmem.h lstate.h \
 lobject.h ltm.h lzio.h

# (end of Makefile)
