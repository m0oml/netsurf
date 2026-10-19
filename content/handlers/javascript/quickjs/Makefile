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

# The class bindings and javascript support files are shared with duktape
QJS_DUKTAPE_DIR := content/handlers/javascript/duktape
QJS_BINDINGS := $(wildcard $(QJS_DUKTAPE_DIR)/*.bnd)

content/handlers/javascript/quickjs/qjsky.c: \
	$(OBJROOT)/quickjs/binding.h \
	$(OBJROOT)/quickjs/generics.js.inc \
	$(OBJROOT)/quickjs/polyfill.js.inc

# Generator for the C include representing the generics.js
$(OBJROOT)/quickjs/generics.js.inc: $(QJS_DUKTAPE_DIR)/generics.js $(TOOLROOT)/xxd
	$(Q)$(MKDIR) -p $(OBJROOT)/quickjs
	$(VQ)echo "     XXD: $<"
	$(Q)$(TOOLROOT)/xxd -i $< $@.tmp
	$(Q)sed -e 's/content_handlers_javascript_duktape_generics_js/generics_js/' $@.tmp > $@

# Generator for the C include representing the polyfill.js
$(OBJROOT)/quickjs/polyfill.js.inc: $(QJS_DUKTAPE_DIR)/polyfill.js $(TOOLROOT)/xxd
	$(Q)$(MKDIR) -p $(OBJROOT)/quickjs
	$(VQ)echo "     XXD: $<"
	$(Q)$(TOOLROOT)/xxd -i $< $@.tmp
	$(Q)sed -e 's/content_handlers_javascript_duktape_polyfill_js/polyfill_js/' $@.tmp > $@

# The binding is the QuickJS binding block followed by the class bindings
# from the duktape binding with their includes made relative to the
# source tree.
$(OBJROOT)/quickjs/netsurf.bnd: content/handlers/javascript/quickjs/binding.bnd $(QJS_DUKTAPE_DIR)/netsurf.bnd
	$(Q)$(MKDIR) -p $(OBJROOT)/quickjs
	$(VQ)echo "  BINDGN: $@"
	$(Q)cat content/handlers/javascript/quickjs/binding.bnd > $@.tmp
	$(Q)sed -e '1,/^};/d' \
		-e 's|^#include "\(.*\)"|#include "$(QJS_DUKTAPE_DIR)/\1"|' \
		$(QJS_DUKTAPE_DIR)/netsurf.bnd >> $@.tmp
	$(Q)mv $@.tmp $@

# ensure genbind generates debugging files
GBFLAGS+=-D

# nsgenbind only rewrites outputs whose content changes, so a stamp
# records when generation last ran and everything generated depends on
# it.  This runs the generator once and ensures changed sources are seen
# by the same make invocation.
$(OBJROOT)/quickjs/generated.stamp: $(OBJROOT)/quickjs/netsurf.bnd $(QJS_BINDINGS)
	$(Q)$(MKDIR) -p $(OBJROOT)/quickjs
	$(VQ)echo " GENBIND: $<"
	$(Q)nsgenbind $(GBFLAGS) -I content/handlers/javascript/WebIDL $< $(OBJROOT)/quickjs
	$(Q)touch $@

$(OBJROOT)/quickjs/binding.h $(OBJROOT)/quickjs/Makefile: $(OBJROOT)/quickjs/generated.stamp ;

ifeq ($(filter $(MAKECMDGOALS),clean test coverage),)
-include $(OBJROOT)/quickjs/Makefile
endif

S_JAVASCRIPT_BINDING:=$(addprefix $(OBJROOT)/quickjs/,$(NSGENBIND_SOURCES))

$(S_JAVASCRIPT_BINDING): $(OBJROOT)/quickjs/generated.stamp ;

S_JAVASCRIPT += content.c quickjs/qjsky.c quickjs/duk_compat.c \
	$(QJS_ENGINE_SOURCES)
