#
# NetSurf javascript source file inclusion
#
# Included by javascript/Makefile
#

# QuickJS-ng engine core
QJS_ENGINE_SOURCES := \
	quickjs/engine/quickjs.c \
	quickjs/engine/libregexp.c \
	quickjs/engine/libunicode.c \
	quickjs/engine/dtoa.c

QJS_ENGINE_OBJECTS := $(addprefix $(OBJROOT)/content_handlers_javascript_, \
	$(subst /,_,$(QJS_ENGINE_SOURCES:.c=.o)))

# The engine requires C11 and is third party code so its warnings are
# not ours to fix.  These are target specific so they only apply to
# the engine objects.
$(QJS_ENGINE_OBJECTS): CFLAGS += -std=gnu11
$(QJS_ENGINE_OBJECTS): COMMON_WARNFLAGS := -w
$(QJS_ENGINE_OBJECTS): CWARNFLAGS :=

S_JAVASCRIPT += content.c quickjs/qjsky.c $(QJS_ENGINE_SOURCES)
