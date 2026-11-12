#!/usr/bin/make -f
# hmi-widget-meter.lv2, the plugin of the aLabs HMI widgets tutorial
#   make                  Linux (.so)
#   make LIB_EXT=.dylib   macOS: also change lv2:binary in manifest.ttl
#   make CC=x86_64-w64-mingw32-gcc LIB_EXT=.dll   Windows: likewise

NAME    = hmi-widget-meter
CC     ?= gcc
LIB_EXT ?= .so
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter

ifeq ($(LIB_EXT),.dylib)
SHARED = -dynamiclib
else
SHARED = -shared
endif
ifneq ($(LIB_EXT),.dll)
CFLAGS += -fPIC
endif

all: $(NAME).lv2/$(NAME)$(LIB_EXT)

$(NAME).lv2/$(NAME)$(LIB_EXT): $(NAME).c
	$(CC) $(CFLAGS) -std=gnu99 -fvisibility=hidden -I../../alabs-hmi-widgets.lv2 $< $(SHARED) -o $@ -lm $(LDFLAGS)

clean:
	rm -f $(NAME).lv2/$(NAME).so $(NAME).lv2/$(NAME).dylib $(NAME).lv2/$(NAME).dll

.PHONY: all clean
