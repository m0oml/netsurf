/*
 * Copyright 2026 Trevor Cockayne
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Duktape style value stack API implemented on QuickJS.
 *
 * Each QuickJS context has one value stack.  A frame is the part of
 * the stack belonging to the currently executing binding function;
 * non negative indexes are relative to the frame base and negative
 * indexes to the stack top.  Values on the stack hold a reference.
 *
 * Errors raised while binding code runs are held as the pending error
 * of the context and thrown when the binding function returns, as
 * there is no way to unwind the C stack.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"

#include "javascript/quickjs/duk_compat.h"

/** Initial number of value stack entries */
#define QJSDUK_STACK_INITIAL 64

/** Number of arguments which may be passed without allocation */
#define QJSDUK_CALL_ARGS 8

/** Initial number of hidden name table entries, must be a power of 2 */
#define QJSDUK_MAGIC_INITIAL 64

/**
 * Function magic value meaning any number of arguments.
 *
 * QuickJS stores function data magic values as uint16_t so DUK_VARARGS
 * cannot be used directly.
 */
#define QJSDUK_MAGIC_VARARGS 0xFFFF

/** The hidden property holding binding private data */
#define QJSDUK_PRIVATE_NAME "\xFF\xFFNETSURF_DUKTAPE_PRIVATE"

/** Kinds of value stack entry */
enum qjsduk_slot_kind {
	QJSDUK_SLOT_VALUE, /**< a javascript value */
	QJSDUK_SLOT_POINTER, /**< a C pointer */
};

/**
 * Value stack entry
 */
typedef struct qjsduk_slot {
	JSValue v; /**< the value, owned by the slot */
	void *ptr; /**< the pointer for pointer slots */
	const char *cstr; /**< cached C string conversion of v */
	size_t clen; /**< length of cstr */
	enum qjsduk_slot_kind kind; /**< kind of slot */
} qjsduk_slot;

/**
 * Hidden property name table entry
 */
struct qjsduk_magic {
	char *name; /**< the hidden name */
	JSValue sym; /**< private symbol for the name */
	JSAtom atom; /**< atom of sym */
};

/**
 * Compatibility context
 */
struct duk_hthread {
	JSContext *jctx; /**< QuickJS context */
	void *udata; /**< glue data */

	qjsduk_slot *stack; /**< value stack */
	duk_idx_t top; /**< absolute index of the first free entry */
	duk_idx_t alloc; /**< number of entries allocated */
	duk_idx_t base; /**< absolute index of the current frame base */
	JSValue this_val; /**< this of the current frame, not owned */

	bool has_pending; /**< whether there is a pending error */
	JSValue pending; /**< the pending error */

	struct qjsduk_magic *magic; /**< hidden name table */
	unsigned int magic_size; /**< entries in magic table */
	unsigned int magic_count; /**< entries used in magic table */
};

/**
 * Saved frame state
 */
struct qjsduk_frame {
	duk_idx_t base;
	JSValue this_val;
	bool has_pending;
	JSValue pending;
};


/* exported interface documented in duk_compat.h */
duk_context *qjsduk_from_js(JSContext *jctx)
{
	return JS_GetContextOpaque(jctx);
}


/* exported interface documented in duk_compat.h */
JSContext *qjsduk_js(duk_context *ctx)
{
	return ctx->jctx;
}


/* exported interface documented in duk_compat.h */
void *qjsduk_udata(duk_context *ctx)
{
	return ctx->udata;
}


/**
 * Record the exception pending in the QuickJS context.
 *
 * The first error recorded is kept, later ones are discarded.
 */
static void record_exception(duk_context *ctx)
{
	JSValue exc = JS_GetException(ctx->jctx);

	if (ctx->has_pending) {
		JS_FreeValue(ctx->jctx, exc);
	} else {
		ctx->pending = exc;
		ctx->has_pending = true;
	}
}


/**
 * Record an error value, taking ownership of it.
 */
static void record_error(duk_context *ctx, JSValue err)
{
	if (ctx->has_pending) {
		JS_FreeValue(ctx->jctx, err);
	} else {
		ctx->pending = err;
		ctx->has_pending = true;
	}
}


/**
 * Create an error object of the given duktape error code.
 *
 * \return the error object
 */
static JSValue
make_error(duk_context *ctx, duk_errcode_t code, const char *msg)
{
	JSValue exc;

	if (code < 0) {
		code = -code;
	}

	switch (code) {
	case DUK_ERR_RANGE_ERROR:
		JS_ThrowRangeError(ctx->jctx, "%s", msg);
		break;

	case DUK_ERR_REFERENCE_ERROR:
		JS_ThrowReferenceError(ctx->jctx, "%s", msg);
		break;

	case DUK_ERR_SYNTAX_ERROR:
		JS_ThrowSyntaxError(ctx->jctx, "%s", msg);
		break;

	case DUK_ERR_TYPE_ERROR:
		JS_ThrowTypeError(ctx->jctx, "%s", msg);
		break;

	default:
		JS_ThrowPlainError(ctx->jctx, "%s", msg);
		break;
	}

	exc = JS_GetException(ctx->jctx);
	return exc;
}


/**
 * Record a type error for an invalid stack index.
 */
static void invalid_index(duk_context *ctx, duk_idx_t idx)
{
	char msg[64];

	NSLOG(qjsky, WARNING, "invalid stack index %d", idx);
	snprintf(msg, sizeof msg, "invalid stack index %d", idx);
	record_error(ctx, make_error(ctx, DUK_ERR_TYPE_ERROR, msg));
}


/**
 * Get the absolute stack index of a relative index.
 *
 * \return the absolute index or -1 if idx is not valid
 */
static duk_idx_t abs_index(duk_context *ctx, duk_idx_t idx)
{
	duk_idx_t abs;

	if (idx < 0) {
		abs = ctx->top + idx;
	} else {
		abs = ctx->base + idx;
	}

	if ((abs < ctx->base) || (abs >= ctx->top)) {
		return -1;
	}
	return abs;
}


/**
 * Get a stack entry from a relative index.
 *
 * \return the entry or NULL if idx is not valid
 */
static qjsduk_slot *get_slot(duk_context *ctx, duk_idx_t idx)
{
	duk_idx_t abs = abs_index(ctx, idx);

	if (abs < 0) {
		return NULL;
	}
	return &ctx->stack[abs];
}


/**
 * Get the value of a stack entry.
 *
 * \return the value, which remains owned by the stack, or undefined for
 *         pointers and invalid indexes
 */
static JSValueConst get_value(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);

	if ((slot == NULL) || (slot->kind != QJSDUK_SLOT_VALUE)) {
		return JS_UNDEFINED;
	}
	return slot->v;
}


/**
 * Release the resources held by a stack entry.
 */
static void release_slot(duk_context *ctx, qjsduk_slot *slot)
{
	if (slot->cstr != NULL) {
		JS_FreeCString(ctx->jctx, slot->cstr);
		slot->cstr = NULL;
	}
	JS_FreeValue(ctx->jctx, slot->v);
	slot->v = JS_UNDEFINED;
	slot->ptr = NULL;
	slot->kind = QJSDUK_SLOT_VALUE;
}


/**
 * Replace the value of a stack entry, taking ownership of val.
 */
static void set_slot_value(duk_context *ctx, qjsduk_slot *slot, JSValue val)
{
	release_slot(ctx, slot);
	slot->v = val;
}


/**
 * Ensure there is space for another stack entry.
 *
 * \return a new entry at the top of the stack or NULL on memory exhaustion
 */
static qjsduk_slot *push_slot(duk_context *ctx)
{
	qjsduk_slot *slot;

	if (ctx->top >= ctx->alloc) {
		duk_idx_t nalloc = ctx->alloc * 2;
		qjsduk_slot *nstack;

		nstack = realloc(ctx->stack, nalloc * sizeof(*nstack));
		if (nstack == NULL) {
			NSLOG(qjsky, ERROR, "value stack exhausted");
			return NULL;
		}
		ctx->stack = nstack;
		ctx->alloc = nalloc;
	}

	slot = &ctx->stack[ctx->top++];
	slot->v = JS_UNDEFINED;
	slot->ptr = NULL;
	slot->cstr = NULL;
	slot->clen = 0;
	slot->kind = QJSDUK_SLOT_VALUE;

	return slot;
}


/* exported interface documented in duk_compat.h */
void qjsduk_push_value(duk_context *ctx, JSValue val)
{
	qjsduk_slot *slot;

	if (JS_IsException(val)) {
		record_exception(ctx);
		val = JS_UNDEFINED;
	}

	slot = push_slot(ctx);
	if (slot == NULL) {
		JS_FreeValue(ctx->jctx, val);
		record_error(ctx, JS_ThrowOutOfMemory(ctx->jctx));
		JS_FreeValue(ctx->jctx, JS_GetException(ctx->jctx));
		return;
	}
	slot->v = val;
}


/* exported interface documented in duk_compat.h */
JSValue qjsduk_get_value(duk_context *ctx, duk_idx_t idx)
{
	return JS_DupValue(ctx->jctx, get_value(ctx, idx));
}


/* exported interface documented in duk_compat.h */
bool qjsduk_take_error(duk_context *ctx, JSValue *err)
{
	if (!ctx->has_pending) {
		return false;
	}
	*err = ctx->pending;
	ctx->pending = JS_UNDEFINED;
	ctx->has_pending = false;
	return true;
}


/**
 * Pop entries until the stack top is at an absolute index.
 */
static void pop_to(duk_context *ctx, duk_idx_t abs_top)
{
	while (ctx->top > abs_top) {
		ctx->top--;
		release_slot(ctx, &ctx->stack[ctx->top]);
	}
}


/**
 * Start a new frame.
 *
 * \param ctx The compatibility context
 * \param frame Updated with the state to restore when the frame ends
 * \param base Absolute index of the new frame base
 * \param this_val The this value of the new frame
 */
static void
frame_enter(duk_context *ctx,
	    struct qjsduk_frame *frame,
	    duk_idx_t base,
	    JSValueConst this_val)
{
	frame->base = ctx->base;
	frame->this_val = ctx->this_val;
	frame->has_pending = ctx->has_pending;
	frame->pending = ctx->pending;

	ctx->base = base;
	ctx->this_val = this_val;
	ctx->has_pending = false;
	ctx->pending = JS_UNDEFINED;
}


/**
 * End a frame, discarding its stack entries.
 *
 * Any error pending in the frame must have been taken first.
 */
static void frame_leave(duk_context *ctx, struct qjsduk_frame *frame)
{
	assert(ctx->has_pending == false);

	pop_to(ctx, ctx->base);

	ctx->base = frame->base;
	ctx->this_val = frame->this_val;
	ctx->has_pending = frame->has_pending;
	ctx->pending = frame->pending;
}


/**
 * Convert the result of a binding function to a QuickJS return value.
 *
 * \param ctx The compatibility context
 * \param ret The value returned by the binding function
 * \return The result value or JS_EXCEPTION
 */
static JSValue frame_result(duk_context *ctx, duk_ret_t ret)
{
	JSValue err;

	if (qjsduk_take_error(ctx, &err)) {
		return JS_Throw(ctx->jctx, err);
	}

	if (ret < 0) {
		return JS_Throw(ctx->jctx,
				make_error(ctx, ret, "binding error"));
	}

	if ((ret > 0) && (ctx->top > ctx->base)) {
		return qjsduk_get_value(ctx, -1);
	}

	return JS_UNDEFINED;
}


/* exported interface documented in duk_compat.h */
JSValue
qjsduk_invoke(JSContext *jctx,
	      JSValueConst this_val,
	      int argc,
	      JSValueConst *argv,
	      qjsduk_body_fn body,
	      void *priv)
{
	duk_context *ctx = qjsduk_from_js(jctx);
	struct qjsduk_frame frame;
	JSValue global = JS_UNDEFINED;
	JSValue res;
	duk_ret_t ret;
	int arg;

	/* functions called without an object act on the global object as
	 * they do in browsers
	 */
	if (JS_IsUndefined(this_val)) {
		global = JS_GetGlobalObject(jctx);
		this_val = global;
	}

	frame_enter(ctx, &frame, ctx->top, this_val);

	for (arg = 0; arg < argc; arg++) {
		qjsduk_push_value(ctx, JS_DupValue(jctx, argv[arg]));
	}

	ret = body(ctx, priv);

	res = frame_result(ctx, ret);
	frame_leave(ctx, &frame);

	JS_FreeValue(jctx, global);

	return res;
}


/**
 * Encode a C function pointer as two QuickJS integer values.
 */
static void encode_function(duk_c_function func, JSValue *data)
{
	uint64_t addr = (uint64_t)(uintptr_t)func;

	data[0] = JS_NewInt32(NULL, (int32_t)(uint32_t)addr);
	data[1] = JS_NewInt32(NULL, (int32_t)(uint32_t)(addr >> 32));
}


/**
 * Decode a C function pointer from two QuickJS integer values.
 */
static duk_c_function decode_function(JSValueConst *data)
{
	uint64_t addr;

	addr = (uint64_t)(uint32_t)JS_VALUE_GET_INT(data[0]) |
		((uint64_t)(uint32_t)JS_VALUE_GET_INT(data[1]) << 32);

	return (duk_c_function)(uintptr_t)addr;
}


/**
 * QuickJS function which calls a duktape style C function.
 *
 * The magic value holds the number of arguments the function takes.
 */
static JSValue
c_function_trampoline(JSContext *jctx,
		      JSValueConst this_val,
		      int argc,
		      JSValueConst *argv,
		      int magic,
		      JSValueConst *func_data)
{
	duk_context *ctx = qjsduk_from_js(jctx);
	duk_c_function func = decode_function(func_data);
	struct qjsduk_frame frame;
	JSValue res;
	duk_ret_t ret;
	int arg;
	int nargs = magic;

	if (nargs == QJSDUK_MAGIC_VARARGS) {
		nargs = argc;
	}

	frame_enter(ctx, &frame, ctx->top, this_val);

	for (arg = 0; arg < nargs; arg++) {
		if (arg < argc) {
			qjsduk_push_value(ctx, JS_DupValue(jctx, argv[arg]));
		} else {
			qjsduk_push_value(ctx, JS_UNDEFINED);
		}
	}

	ret = func(ctx);

	res = frame_result(ctx, ret);
	frame_leave(ctx, &frame);

	return res;
}


/**
 * Compute the hash of a hidden name.
 */
static uint32_t magic_hash(const char *name)
{
	uint32_t hash = 2166136261u;

	while (*name != '\0') {
		hash ^= (uint8_t)*name++;
		hash *= 16777619u;
	}
	return hash;
}


/**
 * Find the table entry for a hidden name.
 *
 * \return the entry, which is unused if name is not in the table
 */
static struct qjsduk_magic *magic_find(duk_context *ctx, const char *name)
{
	unsigned int mask = ctx->magic_size - 1;
	unsigned int idx = magic_hash(name) & mask;

	while (ctx->magic[idx].name != NULL) {
		if (strcmp(ctx->magic[idx].name, name) == 0) {
			break;
		}
		idx = (idx + 1) & mask;
	}
	return &ctx->magic[idx];
}


/**
 * Double the size of the hidden name table.
 *
 * \return true on success
 */
static bool magic_grow(duk_context *ctx)
{
	struct qjsduk_magic *old = ctx->magic;
	unsigned int old_size = ctx->magic_size;
	unsigned int idx;

	ctx->magic = calloc(old_size * 2, sizeof(*ctx->magic));
	if (ctx->magic == NULL) {
		ctx->magic = old;
		return false;
	}
	ctx->magic_size = old_size * 2;

	for (idx = 0; idx < old_size; idx++) {
		if (old[idx].name != NULL) {
			*magic_find(ctx, old[idx].name) = old[idx];
		}
	}
	free(old);

	return true;
}


/**
 * Get the table entry for a hidden name, creating it if necessary.
 *
 * \return the entry or NULL on error
 */
static struct qjsduk_magic *magic_get(duk_context *ctx, const char *name)
{
	struct qjsduk_magic *entry;

	entry = magic_find(ctx, name);
	if (entry->name != NULL) {
		return entry;
	}

	/* keep the table no more than half full */
	if ((ctx->magic_count + 1) * 2 > ctx->magic_size) {
		if (!magic_grow(ctx)) {
			return NULL;
		}
		entry = magic_find(ctx, name);
	}

	entry->sym = JS_NewPrivateSymbol(ctx->jctx, name + 1);
	if (JS_IsException(entry->sym)) {
		record_exception(ctx);
		entry->sym = JS_UNDEFINED;
		return NULL;
	}
	entry->atom = JS_ValueToAtom(ctx->jctx, entry->sym);
	entry->name = strdup(name);
	if (entry->name == NULL) {
		JS_FreeAtom(ctx->jctx, entry->atom);
		JS_FreeValue(ctx->jctx, entry->sym);
		return NULL;
	}
	ctx->magic_count++;

	return entry;
}


/**
 * Check if a C string is a hidden property name.
 */
static inline bool is_magic(const char *name)
{
	return (uint8_t)name[0] == 0xFF;
}


/* exported interface documented in duk_compat.h */
JSAtom qjsduk_magic_atom(duk_context *ctx, const char *name)
{
	struct qjsduk_magic *entry = magic_get(ctx, name);

	if (entry == NULL) {
		return JS_ATOM_NULL;
	}
	return entry->atom;
}


/* exported interface documented in duk_compat.h */
duk_context *qjsduk_create(JSContext *jctx, void *udata)
{
	duk_context *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return NULL;
	}

	ctx->stack = malloc(QJSDUK_STACK_INITIAL * sizeof(*ctx->stack));
	ctx->magic = calloc(QJSDUK_MAGIC_INITIAL, sizeof(*ctx->magic));
	if ((ctx->stack == NULL) || (ctx->magic == NULL)) {
		free(ctx->stack);
		free(ctx->magic);
		free(ctx);
		return NULL;
	}

	ctx->jctx = jctx;
	ctx->udata = udata;
	ctx->alloc = QJSDUK_STACK_INITIAL;
	ctx->this_val = JS_UNDEFINED;
	ctx->pending = JS_UNDEFINED;
	ctx->magic_size = QJSDUK_MAGIC_INITIAL;

	JS_SetContextOpaque(jctx, ctx);

	return ctx;
}


/* exported interface documented in duk_compat.h */
void qjsduk_destroy(duk_context *ctx)
{
	unsigned int idx;

	pop_to(ctx, 0);

	if (ctx->has_pending) {
		JS_FreeValue(ctx->jctx, ctx->pending);
	}

	for (idx = 0; idx < ctx->magic_size; idx++) {
		if (ctx->magic[idx].name != NULL) {
			JS_FreeAtom(ctx->jctx, ctx->magic[idx].atom);
			JS_FreeValue(ctx->jctx, ctx->magic[idx].sym);
			free(ctx->magic[idx].name);
		}
	}

	JS_SetContextOpaque(ctx->jctx, NULL);

	free(ctx->magic);
	free(ctx->stack);
	free(ctx);
}


/**
 * Convert a stack entry to a property key atom.
 *
 * \return a new atom or JS_ATOM_NULL with the error recorded
 */
static JSAtom key_atom(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	JSAtom atom;

	if (slot == NULL) {
		invalid_index(ctx, idx);
		return JS_ATOM_NULL;
	}

	if (slot->kind == QJSDUK_SLOT_POINTER) {
		char buf[32];
		snprintf(buf, sizeof buf, "%p", slot->ptr);
		return JS_NewAtom(ctx->jctx, buf);
	}

	atom = JS_ValueToAtom(ctx->jctx, slot->v);
	if (atom == JS_ATOM_NULL) {
		record_exception(ctx);
	}
	return atom;
}


/**
 * Convert a C string to a property key atom.
 *
 * \return a new atom or JS_ATOM_NULL with the error recorded
 */
static JSAtom name_atom(duk_context *ctx, const char *key)
{
	JSAtom atom;

	if (is_magic(key)) {
		atom = qjsduk_magic_atom(ctx, key);
		if (atom != JS_ATOM_NULL) {
			atom = JS_DupAtom(ctx->jctx, atom);
		}
	} else {
		atom = JS_NewAtom(ctx->jctx, key);
	}
	if (atom == JS_ATOM_NULL) {
		record_exception(ctx);
	}
	return atom;
}


/**
 * Get a C string for a stack entry without coercion.
 *
 * \return the string, valid while the entry is unchanged, or NULL if the
 *         entry is not a string
 */
static const char *slot_string(duk_context *ctx, qjsduk_slot *slot, size_t *len)
{
	if ((slot->kind != QJSDUK_SLOT_VALUE) || !JS_IsString(slot->v)) {
		return NULL;
	}

	if (slot->cstr == NULL) {
		slot->cstr = JS_ToCStringLen(ctx->jctx, &slot->clen, slot->v);
		if (slot->cstr == NULL) {
			record_exception(ctx);
			return NULL;
		}
	}

	if (len != NULL) {
		*len = slot->clen;
	}
	return slot->cstr;
}


/* stack manipulation */

duk_idx_t duk_get_top(duk_context *ctx)
{
	return ctx->top - ctx->base;
}


void duk_set_top(duk_context *ctx, duk_idx_t idx)
{
	duk_idx_t abs;

	if (idx < 0) {
		abs = ctx->top + idx;
	} else {
		abs = ctx->base + idx;
	}

	if (abs < ctx->base) {
		invalid_index(ctx, idx);
		return;
	}

	pop_to(ctx, abs);
	while (ctx->top < abs) {
		qjsduk_push_value(ctx, JS_UNDEFINED);
	}
}


duk_idx_t duk_normalize_index(duk_context *ctx, duk_idx_t idx)
{
	duk_idx_t abs = abs_index(ctx, idx);

	if (abs < 0) {
		return abs;
	}
	return abs - ctx->base;
}


void duk_dup(duk_context *ctx, duk_idx_t from_idx)
{
	qjsduk_slot *src = get_slot(ctx, from_idx);
	qjsduk_slot copy;
	qjsduk_slot *dst;

	if (src == NULL) {
		invalid_index(ctx, from_idx);
		qjsduk_push_value(ctx, JS_UNDEFINED);
		return;
	}

	/* the push may move the stack so copy the source first */
	copy = *src;

	dst = push_slot(ctx);
	if (dst == NULL) {
		return;
	}
	dst->kind = copy.kind;
	dst->ptr = copy.ptr;
	dst->v = JS_DupValue(ctx->jctx, copy.v);
}


void duk_dup_top(duk_context *ctx)
{
	duk_dup(ctx, -1);
}


void duk_insert(duk_context *ctx, duk_idx_t to_idx)
{
	duk_idx_t abs = abs_index(ctx, to_idx);
	qjsduk_slot tmp;

	if (abs < 0) {
		invalid_index(ctx, to_idx);
		return;
	}

	tmp = ctx->stack[ctx->top - 1];
	memmove(&ctx->stack[abs + 1],
		&ctx->stack[abs],
		(ctx->top - 1 - abs) * sizeof(tmp));
	ctx->stack[abs] = tmp;
}


void duk_remove(duk_context *ctx, duk_idx_t idx)
{
	duk_idx_t abs = abs_index(ctx, idx);

	if (abs < 0) {
		invalid_index(ctx, idx);
		return;
	}

	release_slot(ctx, &ctx->stack[abs]);
	memmove(&ctx->stack[abs],
		&ctx->stack[abs + 1],
		(ctx->top - 1 - abs) * sizeof(ctx->stack[0]));
	ctx->top--;
}


void duk_replace(duk_context *ctx, duk_idx_t to_idx)
{
	duk_idx_t abs = abs_index(ctx, to_idx);

	if (abs < 0) {
		invalid_index(ctx, to_idx);
		return;
	}

	ctx->top--;
	release_slot(ctx, &ctx->stack[abs]);
	if (abs != ctx->top) {
		/* move the popped value, replacing the top with itself is
		 * just a pop
		 */
		ctx->stack[abs] = ctx->stack[ctx->top];
	}
}


void duk_pop_n(duk_context *ctx, duk_idx_t count)
{
	if (count > (ctx->top - ctx->base)) {
		invalid_index(ctx, -count);
		count = ctx->top - ctx->base;
	}
	pop_to(ctx, ctx->top - count);
}


void duk_pop(duk_context *ctx)
{
	duk_pop_n(ctx, 1);
}


void duk_pop_2(duk_context *ctx)
{
	duk_pop_n(ctx, 2);
}


void duk_pop_3(duk_context *ctx)
{
	duk_pop_n(ctx, 3);
}


/* pushing values */

void duk_push_undefined(duk_context *ctx)
{
	qjsduk_push_value(ctx, JS_UNDEFINED);
}


void duk_push_null(duk_context *ctx)
{
	qjsduk_push_value(ctx, JS_NULL);
}


void duk_push_boolean(duk_context *ctx, duk_bool_t val)
{
	qjsduk_push_value(ctx, JS_NewBool(ctx->jctx, val != 0));
}


void duk_push_int(duk_context *ctx, duk_int_t val)
{
	qjsduk_push_value(ctx, JS_NewInt32(ctx->jctx, val));
}


void duk_push_uint(duk_context *ctx, duk_uint_t val)
{
	qjsduk_push_value(ctx, JS_NewUint32(ctx->jctx, val));
}


void duk_push_number(duk_context *ctx, duk_double_t val)
{
	qjsduk_push_value(ctx, JS_NewNumber(ctx->jctx, val));
}


const char *duk_push_lstring(duk_context *ctx, const char *str, duk_size_t len)
{
	if (str == NULL) {
		str = "";
		len = 0;
	}

	if ((len > 0) && is_magic(str)) {
		/* hidden names become private symbols */
		char *name = strndup(str, len);
		struct qjsduk_magic *entry = NULL;

		if (name != NULL) {
			entry = magic_get(ctx, name);
			free(name);
		}
		if (entry == NULL) {
			qjsduk_push_value(ctx, JS_UNDEFINED);
		} else {
			qjsduk_push_value(ctx, JS_DupValue(ctx->jctx,
							  entry->sym));
		}
		return NULL;
	}

	qjsduk_push_value(ctx, JS_NewStringLen(ctx->jctx, str, len));
	return duk_get_string(ctx, -1);
}


const char *duk_push_string(duk_context *ctx, const char *str)
{
	if (str == NULL) {
		qjsduk_push_value(ctx, JS_NULL);
		return NULL;
	}
	return duk_push_lstring(ctx, str, strlen(str));
}


/**
 * Push a formatted string.
 */
static const char *
push_vsprintf(duk_context *ctx, const char *fmt, va_list ap)
{
	char buf[256];
	char *str = buf;
	va_list ap2;
	const char *res;
	int len;

	va_copy(ap2, ap);
	len = vsnprintf(buf, sizeof buf, fmt, ap);
	if (len < 0) {
		va_end(ap2);
		return duk_push_lstring(ctx, "", 0);
	}
	if ((size_t)len >= sizeof buf) {
		str = malloc(len + 1);
		if (str == NULL) {
			va_end(ap2);
			return duk_push_lstring(ctx, buf, sizeof buf - 1);
		}
		vsnprintf(str, len + 1, fmt, ap2);
	}
	va_end(ap2);

	res = duk_push_lstring(ctx, str, len);
	if (str != buf) {
		free(str);
	}
	return res;
}


const char *duk_push_sprintf(duk_context *ctx, const char *fmt, ...)
{
	va_list ap;
	const char *res;

	va_start(ap, fmt);
	res = push_vsprintf(ctx, fmt, ap);
	va_end(ap);

	return res;
}


void duk_push_pointer(duk_context *ctx, void *p)
{
	qjsduk_slot *slot = push_slot(ctx);

	if (slot != NULL) {
		slot->kind = QJSDUK_SLOT_POINTER;
		slot->ptr = p;
	}
}


duk_idx_t duk_push_object(duk_context *ctx)
{
	qjsduk_push_value(ctx, JS_NewObject(ctx->jctx));
	return duk_get_top(ctx) - 1;
}


duk_idx_t duk_push_array(duk_context *ctx)
{
	qjsduk_push_value(ctx, JS_NewArray(ctx->jctx));
	return duk_get_top(ctx) - 1;
}


void duk_push_this(duk_context *ctx)
{
	qjsduk_push_value(ctx, JS_DupValue(ctx->jctx, ctx->this_val));
}


void duk_push_global_object(duk_context *ctx)
{
	qjsduk_push_value(ctx, JS_GetGlobalObject(ctx->jctx));
}


duk_idx_t
duk_push_c_function(duk_context *ctx, duk_c_function func, duk_idx_t nargs)
{
	JSValue data[2];
	int length = nargs;
	int magic = nargs;

	if ((nargs < 0) || (nargs >= QJSDUK_MAGIC_VARARGS)) {
		length = 0;
		magic = QJSDUK_MAGIC_VARARGS;
	}

	encode_function(func, data);
	qjsduk_push_value(ctx, JS_NewCFunctionData(ctx->jctx,
						   c_function_trampoline,
						   length,
						   magic,
						   2,
						   data));
	return duk_get_top(ctx) - 1;
}


duk_idx_t
duk_push_error_object(duk_context *ctx, duk_errcode_t err_code, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	qjsduk_push_value(ctx, make_error(ctx, err_code, msg));
	return duk_get_top(ctx) - 1;
}


void *duk_push_buffer(duk_context *ctx, duk_size_t size, duk_bool_t dynamic)
{
	uint8_t *zero;
	JSValue buf;
	size_t len;

	/* QuickJS copies the initial contents so supply zeroed memory, of
	 * at least one byte as some platforms dislike zero sized blocks.
	 */
	zero = calloc(1, size > 0 ? size : 1);
	if (zero == NULL) {
		qjsduk_push_value(ctx, JS_ThrowOutOfMemory(ctx->jctx));
		return NULL;
	}
	buf = JS_NewArrayBufferCopy(ctx->jctx, zero, size);
	free(zero);

	qjsduk_push_value(ctx, buf);

	return JS_GetArrayBuffer(ctx->jctx, &len, get_value(ctx, -1));
}


void
duk_push_buffer_object(duk_context *ctx,
		       duk_idx_t idx_buffer,
		       duk_size_t byte_offset,
		       duk_size_t byte_length,
		       duk_uint_t flags)
{
	JSValue args[3];
	JSValue res;

	if (flags != DUK_BUFOBJ_UINT8CLAMPEDARRAY) {
		NSLOG(qjsky, ERROR, "unsupported buffer object type %u", flags);
		qjsduk_push_value(ctx, JS_UNDEFINED);
		return;
	}

	args[0] = get_value(ctx, idx_buffer);
	args[1] = JS_NewInt64(ctx->jctx, byte_offset);
	args[2] = JS_NewInt64(ctx->jctx, byte_length);

	res = JS_NewTypedArray(ctx->jctx, 3, args, JS_TYPED_ARRAY_UINT8C);
	qjsduk_push_value(ctx, res);
}


void duk_push_context_dump(duk_context *ctx)
{
	duk_push_sprintf(ctx, "ctx: top=%d base=%d",
			 (int)(ctx->top - ctx->base), (int)ctx->base);
}


/* type checks */

duk_int_t duk_get_type(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	JSValueConst v;

	if (slot == NULL) {
		return DUK_TYPE_NONE;
	}
	if (slot->kind == QJSDUK_SLOT_POINTER) {
		return DUK_TYPE_POINTER;
	}

	v = slot->v;
	if (JS_IsUndefined(v)) {
		return DUK_TYPE_UNDEFINED;
	}
	if (JS_IsNull(v)) {
		return DUK_TYPE_NULL;
	}
	if (JS_IsBool(v)) {
		return DUK_TYPE_BOOLEAN;
	}
	if (JS_IsNumber(v) || JS_IsBigInt(v)) {
		return DUK_TYPE_NUMBER;
	}
	if (JS_IsString(v) || JS_IsSymbol(v)) {
		return DUK_TYPE_STRING;
	}
	return DUK_TYPE_OBJECT;
}


duk_bool_t duk_check_type(duk_context *ctx, duk_idx_t idx, duk_int_t type)
{
	return duk_get_type(ctx, idx) == type;
}


duk_bool_t duk_is_undefined(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_type(ctx, idx) == DUK_TYPE_UNDEFINED;
}


duk_bool_t duk_is_null(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_type(ctx, idx) == DUK_TYPE_NULL;
}


duk_bool_t duk_is_boolean(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_type(ctx, idx) == DUK_TYPE_BOOLEAN;
}


duk_bool_t duk_is_number(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_type(ctx, idx) == DUK_TYPE_NUMBER;
}


duk_bool_t duk_is_string(duk_context *ctx, duk_idx_t idx)
{
	return duk_get_type(ctx, idx) == DUK_TYPE_STRING;
}


/* reading values without coercion */

/**
 * Clamp a double to an integer range as duktape does.
 */
static double clamp_number(double d, double min, double max)
{
	if (d != d) {
		return 0; /* NaN */
	}
	if (d < min) {
		return min;
	}
	if (d > max) {
		return max;
	}
	return d;
}


duk_bool_t duk_get_boolean(duk_context *ctx, duk_idx_t idx)
{
	JSValueConst v = get_value(ctx, idx);

	if (JS_IsBool(v)) {
		return JS_VALUE_GET_BOOL(v);
	}
	return 0;
}


duk_int_t duk_get_int(duk_context *ctx, duk_idx_t idx)
{
	JSValueConst v = get_value(ctx, idx);
	double d;

	if (!JS_IsNumber(v)) {
		return 0;
	}
	JS_ToFloat64(ctx->jctx, &d, v);
	return (duk_int_t)clamp_number(d, INT32_MIN, INT32_MAX);
}


duk_uint_t duk_get_uint(duk_context *ctx, duk_idx_t idx)
{
	JSValueConst v = get_value(ctx, idx);
	double d;

	if (!JS_IsNumber(v)) {
		return 0;
	}
	JS_ToFloat64(ctx->jctx, &d, v);
	return (duk_uint_t)clamp_number(d, 0, UINT32_MAX);
}


void *duk_get_pointer(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);

	if ((slot == NULL) || (slot->kind != QJSDUK_SLOT_POINTER)) {
		return NULL;
	}
	return slot->ptr;
}


const char *duk_get_string(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);

	if (slot == NULL) {
		return NULL;
	}
	return slot_string(ctx, slot, NULL);
}


duk_size_t duk_get_length(duk_context *ctx, duk_idx_t idx)
{
	JSValueConst v = get_value(ctx, idx);
	int64_t len = 0;

	if (JS_IsString(v)) {
		JSValue lv = JS_GetPropertyStr(ctx->jctx, v, "length");
		JS_ToInt64Ext(ctx->jctx, &len, lv);
		JS_FreeValue(ctx->jctx, lv);
	} else if (JS_IsObject(v)) {
		if (JS_GetLength(ctx->jctx, v, &len) < 0) {
			record_exception(ctx);
			len = 0;
		}
	}

	return len < 0 ? 0 : (duk_size_t)len;
}


duk_bool_t duk_opt_boolean(duk_context *ctx, duk_idx_t idx, duk_bool_t def_value)
{
	int type = duk_get_type(ctx, idx);

	if ((type == DUK_TYPE_NONE) || (type == DUK_TYPE_UNDEFINED)) {
		return def_value;
	}
	return duk_to_boolean(ctx, idx);
}


/**
 * Record a type error for a value of the wrong type.
 */
static void wrong_type(duk_context *ctx, duk_idx_t idx, const char *expect)
{
	char msg[64];

	snprintf(msg, sizeof msg, "value at %d is not a %s", idx, expect);
	record_error(ctx, make_error(ctx, DUK_ERR_TYPE_ERROR, msg));
}


duk_bool_t duk_require_boolean(duk_context *ctx, duk_idx_t idx)
{
	if (!duk_is_boolean(ctx, idx)) {
		wrong_type(ctx, idx, "boolean");
		return 0;
	}
	return duk_get_boolean(ctx, idx);
}


duk_int_t duk_require_int(duk_context *ctx, duk_idx_t idx)
{
	if (!duk_is_number(ctx, idx)) {
		wrong_type(ctx, idx, "number");
		return 0;
	}
	return duk_get_int(ctx, idx);
}


duk_double_t duk_require_number(duk_context *ctx, duk_idx_t idx)
{
	double d = 0;

	if (!duk_is_number(ctx, idx)) {
		wrong_type(ctx, idx, "number");
		return d;
	}
	JS_ToFloat64(ctx->jctx, &d, get_value(ctx, idx));
	return d;
}


const char *duk_require_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	const char *str = NULL;
	size_t len = 0;

	if (slot != NULL) {
		str = slot_string(ctx, slot, &len);
	}
	if (str == NULL) {
		wrong_type(ctx, idx, "string");
		str = "";
		len = 0;
	}
	if (out_len != NULL) {
		*out_len = len;
	}
	return str;
}


const char *duk_require_string(duk_context *ctx, duk_idx_t idx)
{
	return duk_require_lstring(ctx, idx, NULL);
}


/* coercion */

duk_bool_t duk_to_boolean(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	int res;

	if (slot == NULL) {
		invalid_index(ctx, idx);
		return 0;
	}

	if (slot->kind == QJSDUK_SLOT_POINTER) {
		res = slot->ptr != NULL;
	} else {
		res = JS_ToBool(ctx->jctx, slot->v);
		if (res < 0) {
			record_exception(ctx);
			res = 0;
		}
	}
	set_slot_value(ctx, slot, JS_NewBool(ctx->jctx, res));

	return res;
}


/**
 * Coerce a stack entry to a number in place.
 *
 * \return the number or 0 if coercion failed
 */
static double to_number(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	double d = 0;

	if (slot == NULL) {
		invalid_index(ctx, idx);
		return 0;
	}

	if (JS_ToFloat64(ctx->jctx, &d, get_value(ctx, idx)) < 0) {
		record_exception(ctx);
		d = 0;
	}
	set_slot_value(ctx, slot, JS_NewFloat64(ctx->jctx, d));

	return d;
}


duk_int_t duk_to_int(duk_context *ctx, duk_idx_t idx)
{
	double d = to_number(ctx, idx);
	qjsduk_slot *slot = get_slot(ctx, idx);
	duk_int_t res;

	res = (duk_int_t)clamp_number(d, INT32_MIN, INT32_MAX);
	if (slot != NULL) {
		set_slot_value(ctx, slot, JS_NewInt32(ctx->jctx, res));
	}

	return res;
}


duk_uint_t duk_to_uint(duk_context *ctx, duk_idx_t idx)
{
	double d = to_number(ctx, idx);
	qjsduk_slot *slot = get_slot(ctx, idx);
	duk_uint_t res;

	res = (duk_uint_t)clamp_number(d, 0, UINT32_MAX);
	if (slot != NULL) {
		set_slot_value(ctx, slot, JS_NewUint32(ctx->jctx, res));
	}

	return res;
}


duk_uint32_t duk_to_uint32(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	uint32_t res = 0;

	if (slot == NULL) {
		invalid_index(ctx, idx);
		return 0;
	}

	if (JS_ToUint32(ctx->jctx, &res, get_value(ctx, idx)) < 0) {
		record_exception(ctx);
		res = 0;
	}
	set_slot_value(ctx, slot, JS_NewUint32(ctx->jctx, res));

	return res;
}


/**
 * Coerce a stack entry to a string in place.
 *
 * \param ctx The compatibility context
 * \param idx The stack index
 * \param out_len Updated with the string length if not NULL
 * \param safe When true conversion errors are suppressed
 * \return the string, valid while the entry is unchanged
 */
static const char *
to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len, bool safe)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	const char *str;
	JSValue sv;

	if (slot == NULL) {
		invalid_index(ctx, idx);
		if (out_len != NULL) {
			*out_len = 0;
		}
		return "";
	}

	if (slot->kind == QJSDUK_SLOT_POINTER) {
		char buf[32];
		snprintf(buf, sizeof buf, "%p", slot->ptr);
		sv = JS_NewString(ctx->jctx, buf);
	} else if (JS_IsSymbol(slot->v)) {
		/* hidden names and symbols do not convert implicitly */
		sv = JS_NewString(ctx->jctx, "[symbol]");
	} else {
		sv = JS_ToString(ctx->jctx, slot->v);
	}

	if (JS_IsException(sv)) {
		if (safe) {
			JS_FreeValue(ctx->jctx, JS_GetException(ctx->jctx));
			sv = JS_NewString(ctx->jctx, "Error");
		} else {
			record_exception(ctx);
			sv = JS_NewString(ctx->jctx, "");
		}
	}
	set_slot_value(ctx, slot, sv);

	str = slot_string(ctx, slot, out_len);
	if (str == NULL) {
		if (out_len != NULL) {
			*out_len = 0;
		}
		str = "";
	}
	return str;
}


const char *duk_to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len)
{
	return to_lstring(ctx, idx, out_len, false);
}


const char *duk_to_string(duk_context *ctx, duk_idx_t idx)
{
	return to_lstring(ctx, idx, NULL, false);
}


const char *duk_safe_to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len)
{
	return to_lstring(ctx, idx, out_len, true);
}


const char *duk_safe_to_string(duk_context *ctx, duk_idx_t idx)
{
	return to_lstring(ctx, idx, NULL, true);
}


/**
 * Join stack entries as strings into a new string value.
 *
 * Each entry is coerced to a string in place.
 *
 * \param ctx The compatibility context
 * \param first Index of the first entry to join
 * \param count Number of entries to join
 * \param sep Separator between entries
 * \return The joined string or JS_EXCEPTION
 */
static JSValue join_strings(duk_context *ctx, duk_idx_t first, duk_idx_t count, const char *sep)
{
	size_t seplen = strlen(sep);
	size_t total = 0;
	size_t len;
	char *buf;
	char *pos;
	JSValue res;
	duk_idx_t idx;

	for (idx = first; idx < first + count; idx++) {
		duk_to_lstring(ctx, idx, &len);
		total += len + seplen;
	}

	buf = malloc(total + 1);
	if (buf == NULL) {
		return JS_ThrowOutOfMemory(ctx->jctx);
	}

	pos = buf;
	for (idx = first; idx < first + count; idx++) {
		const char *str = duk_to_lstring(ctx, idx, &len);
		if (idx != first) {
			memcpy(pos, sep, seplen);
			pos += seplen;
		}
		memcpy(pos, str, len);
		pos += len;
	}

	res = JS_NewStringLen(ctx->jctx, buf, pos - buf);
	free(buf);

	return res;
}


const char *duk_safe_to_stacktrace(duk_context *ctx, duk_idx_t idx)
{
	qjsduk_slot *slot = get_slot(ctx, idx);
	JSValue stack;
	JSValue res;

	if ((slot == NULL) ||
	    (slot->kind != QJSDUK_SLOT_VALUE) ||
	    !JS_IsError(slot->v)) {
		return to_lstring(ctx, idx, NULL, true);
	}

	stack = JS_GetPropertyStr(ctx->jctx, slot->v, "stack");
	if (!JS_IsString(stack)) {
		JS_FreeValue(ctx->jctx, stack);
		return to_lstring(ctx, idx, NULL, true);
	}

	/* "message\nstack" as duktape would give */
	duk_dup(ctx, idx);
	duk_safe_to_string(ctx, -1);
	qjsduk_push_value(ctx, stack);
	res = join_strings(ctx, -2, 2, "\n");
	duk_pop_2(ctx);

	if (JS_IsException(res)) {
		JS_FreeValue(ctx->jctx, JS_GetException(ctx->jctx));
		return to_lstring(ctx, idx, NULL, true);
	}

	set_slot_value(ctx, get_slot(ctx, idx), res);
	return to_lstring(ctx, idx, NULL, true);
}


/* properties */

/**
 * Get a property and push it.
 *
 * \param ctx The compatibility context
 * \param obj The object, owned by the caller
 * \param atom The key, owned by the caller
 * \return true if the value is not undefined
 */
static duk_bool_t get_and_push(duk_context *ctx, JSValueConst obj, JSAtom atom)
{
	JSValue val;

	if (atom == JS_ATOM_NULL) {
		qjsduk_push_value(ctx, JS_UNDEFINED);
		return 0;
	}

	val = JS_GetProperty(ctx->jctx, obj, atom);
	qjsduk_push_value(ctx, val);

	return !duk_is_undefined(ctx, -1);
}


duk_bool_t duk_get_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = key_atom(ctx, -1);
	duk_bool_t res;

	duk_pop(ctx);
	res = get_and_push(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t
duk_get_prop_string(duk_context *ctx, duk_idx_t obj_idx, const char *key)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	duk_bool_t res;
	JSAtom atom;

	if (strcmp(key, QJSDUK_PRIVATE_NAME) == 0) {
		/* binding private data is native, not a property */
		void *priv = qjsduk_lookup_private(ctx, obj);
		JS_FreeValue(ctx->jctx, obj);
		if (priv == NULL) {
			qjsduk_push_value(ctx, JS_UNDEFINED);
			return 0;
		}
		duk_push_pointer(ctx, priv);
		return 1;
	}

	atom = name_atom(ctx, key);
	res = get_and_push(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t
duk_get_prop_index(duk_context *ctx, duk_idx_t obj_idx, duk_uarridx_t arr_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSValue val;

	val = JS_GetPropertyUint32(ctx->jctx, obj, arr_idx);
	qjsduk_push_value(ctx, val);

	JS_FreeValue(ctx->jctx, obj);
	return !duk_is_undefined(ctx, -1);
}


/**
 * Set a property from the value on the stack top and pop it.
 *
 * \param ctx The compatibility context
 * \param obj The object, owned by the caller
 * \param atom The key, owned by the caller
 * \return true on success
 */
static duk_bool_t put_and_pop(duk_context *ctx, JSValueConst obj, JSAtom atom)
{
	qjsduk_slot *slot = get_slot(ctx, -1);
	JSValue val;
	int res = -1;

	if (slot == NULL) {
		invalid_index(ctx, -1);
		return 0;
	}

	if (slot->kind == QJSDUK_SLOT_POINTER) {
		NSLOG(qjsky, ERROR, "pointers cannot be stored in objects");
		val = JS_UNDEFINED;
	} else {
		val = JS_DupValue(ctx->jctx, slot->v);
	}

	if (atom != JS_ATOM_NULL) {
		res = JS_SetProperty(ctx->jctx, obj, atom, val);
		if (res < 0) {
			record_exception(ctx);
		}
	} else {
		JS_FreeValue(ctx->jctx, val);
	}

	duk_pop(ctx);
	return res > 0;
}


duk_bool_t duk_put_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = key_atom(ctx, -2);
	duk_bool_t res;

	duk_remove(ctx, -2);
	res = put_and_pop(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t
duk_put_prop_string(duk_context *ctx, duk_idx_t obj_idx, const char *key)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom;
	duk_bool_t res;

	if (strcmp(key, QJSDUK_PRIVATE_NAME) == 0) {
		NSLOG(qjsky, ERROR, "binding private data cannot be set");
		JS_FreeValue(ctx->jctx, obj);
		duk_pop(ctx);
		return 0;
	}

	atom = name_atom(ctx, key);
	res = put_and_pop(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t
duk_put_prop_index(duk_context *ctx, duk_idx_t obj_idx, duk_uarridx_t arr_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = JS_NewAtomUInt32(ctx->jctx, arr_idx);
	duk_bool_t res;

	res = put_and_pop(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


/**
 * Delete a property.
 *
 * \return true if the property no longer exists
 */
static duk_bool_t del_atom(duk_context *ctx, JSValueConst obj, JSAtom atom)
{
	int res;

	if (atom == JS_ATOM_NULL) {
		return 0;
	}

	res = JS_DeleteProperty(ctx->jctx, obj, atom, 0);
	if (res < 0) {
		record_exception(ctx);
		return 0;
	}
	return res;
}


duk_bool_t duk_del_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = key_atom(ctx, -1);
	duk_bool_t res;

	duk_pop(ctx);
	res = del_atom(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t
duk_del_prop_string(duk_context *ctx, duk_idx_t obj_idx, const char *key)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = name_atom(ctx, key);
	duk_bool_t res;

	res = del_atom(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t
duk_del_prop_index(duk_context *ctx, duk_idx_t obj_idx, duk_uarridx_t arr_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = JS_NewAtomUInt32(ctx->jctx, arr_idx);
	duk_bool_t res;

	res = del_atom(ctx, obj, atom);

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


duk_bool_t duk_has_prop(duk_context *ctx, duk_idx_t obj_idx)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSAtom atom = key_atom(ctx, -1);
	int res = 0;

	duk_pop(ctx);

	if (!JS_IsObject(obj)) {
		wrong_type(ctx, obj_idx, "object");
	} else if (atom != JS_ATOM_NULL) {
		res = JS_HasProperty(ctx->jctx, obj, atom);
		if (res < 0) {
			record_exception(ctx);
			res = 0;
		}
	}

	JS_FreeAtom(ctx->jctx, atom);
	JS_FreeValue(ctx->jctx, obj);
	return res;
}


void duk_def_prop(duk_context *ctx, duk_idx_t obj_idx, duk_uint_t flags)
{
	JSValue obj = qjsduk_get_value(ctx, obj_idx);
	JSValue value = JS_UNDEFINED;
	JSValue getter = JS_UNDEFINED;
	JSValue setter = JS_UNDEFINED;
	duk_idx_t nvals = 0;
	int jsflags = 0;
	JSAtom atom;

	/* stack is [ ... key value ] or [ ... key getter? setter? ] */
	if (flags & DUK_DEFPROP_HAVE_VALUE) {
		value = qjsduk_get_value(ctx, -1);
		jsflags |= JS_PROP_HAS_VALUE;
		nvals = 1;
	} else {
		if (flags & DUK_DEFPROP_HAVE_SETTER) {
			setter = qjsduk_get_value(ctx, -1 - nvals);
			jsflags |= JS_PROP_HAS_SET;
			nvals++;
		}
		if (flags & DUK_DEFPROP_HAVE_GETTER) {
			getter = qjsduk_get_value(ctx, -1 - nvals);
			jsflags |= JS_PROP_HAS_GET;
			nvals++;
		}
	}

	if (flags & DUK_DEFPROP_HAVE_WRITABLE) {
		jsflags |= JS_PROP_HAS_WRITABLE;
		if (flags & DUK_DEFPROP_WRITABLE) {
			jsflags |= JS_PROP_WRITABLE;
		}
	}
	if (flags & DUK_DEFPROP_HAVE_ENUMERABLE) {
		jsflags |= JS_PROP_HAS_ENUMERABLE;
		if (flags & DUK_DEFPROP_ENUMERABLE) {
			jsflags |= JS_PROP_ENUMERABLE;
		}
	}
	if (flags & DUK_DEFPROP_HAVE_CONFIGURABLE) {
		jsflags |= JS_PROP_HAS_CONFIGURABLE;
		if (flags & DUK_DEFPROP_CONFIGURABLE) {
			jsflags |= JS_PROP_CONFIGURABLE;
		}
	}

	atom = key_atom(ctx, -1 - nvals);
	if (atom != JS_ATOM_NULL) {
		if (JS_DefineProperty(ctx->jctx, obj, atom,
				      value, getter, setter,
				      jsflags | JS_PROP_THROW) < 0) {
			record_exception(ctx);
		}
		JS_FreeAtom(ctx->jctx, atom);
	}

	duk_pop_n(ctx, nvals + 1);

	JS_FreeValue(ctx->jctx, value);
	JS_FreeValue(ctx->jctx, getter);
	JS_FreeValue(ctx->jctx, setter);
	JS_FreeValue(ctx->jctx, obj);
}


duk_bool_t duk_get_global_string(duk_context *ctx, const char *key)
{
	duk_bool_t res;

	duk_push_global_object(ctx);
	res = duk_get_prop_string(ctx, -1, key);
	duk_remove(ctx, -2);

	return res;
}


duk_bool_t duk_put_global_string(duk_context *ctx, const char *key)
{
	duk_bool_t res;

	duk_push_global_object(ctx);
	duk_insert(ctx, -2);
	res = duk_put_prop_string(ctx, -2, key);
	duk_pop(ctx);

	return res;
}


void duk_get_prototype(duk_context *ctx, duk_idx_t idx)
{
	JSValueConst v = get_value(ctx, idx);
	JSValue proto;

	if (!JS_IsObject(v)) {
		qjsduk_push_value(ctx, JS_UNDEFINED);
		return;
	}

	proto = JS_GetPrototype(ctx->jctx, v);
	if (JS_IsNull(proto)) {
		/* duktape gives undefined for no prototype */
		proto = JS_UNDEFINED;
	}
	qjsduk_push_value(ctx, proto);
}


void duk_set_prototype(duk_context *ctx, duk_idx_t idx)
{
	JSValue obj = qjsduk_get_value(ctx, idx);
	JSValueConst proto = get_value(ctx, -1);

	if (JS_IsUndefined(proto)) {
		proto = JS_NULL;
	}
	if (JS_SetPrototype(ctx->jctx, obj, proto) < 0) {
		record_exception(ctx);
	}
	duk_pop(ctx);

	JS_FreeValue(ctx->jctx, obj);
}


/* calls */

/**
 * Call a function from the stack.
 *
 * The stack is [ ... func this? args ] which is replaced by the result.
 *
 * \param ctx The compatibility context
 * \param nargs The number of arguments
 * \param has_this true if a this value precedes the arguments
 * \param result Updated with the result, owned by the caller
 * \return true on success, false with the exception taken into result
 */
static bool
call_from_stack(duk_context *ctx, duk_idx_t nargs, bool has_this, JSValue *result)
{
	JSValueConst argv_buf[QJSDUK_CALL_ARGS];
	JSValueConst *argv = argv_buf;
	duk_idx_t nvals = nargs + (has_this ? 2 : 1);
	JSValue func;
	JSValue this_val = JS_UNDEFINED;
	duk_idx_t arg;
	JSValue res;

	if ((nargs < 0) || (nvals > (ctx->top - ctx->base))) {
		invalid_index(ctx, -nvals);
		qjsduk_take_error(ctx, result);
		return false;
	}

	if (nargs > QJSDUK_CALL_ARGS) {
		argv = malloc(nargs * sizeof(*argv));
		if (argv == NULL) {
			*result = JS_ThrowOutOfMemory(ctx->jctx);
			*result = JS_GetException(ctx->jctx);
			pop_to(ctx, ctx->top - nvals);
			return false;
		}
	}

	func = qjsduk_get_value(ctx, -nvals);
	if (has_this) {
		this_val = qjsduk_get_value(ctx, -nvals + 1);
	}
	/* the arguments remain referenced from the stack during the call */
	for (arg = 0; arg < nargs; arg++) {
		argv[arg] = get_value(ctx, -nargs + arg);
	}

	res = JS_Call(ctx->jctx, func, this_val, nargs, argv);

	if (argv != argv_buf) {
		free(argv);
	}
	JS_FreeValue(ctx->jctx, func);
	JS_FreeValue(ctx->jctx, this_val);
	pop_to(ctx, ctx->top - nvals);

	if (JS_IsException(res)) {
		*result = JS_GetException(ctx->jctx);
		return false;
	}
	*result = res;
	return true;
}


void duk_call(duk_context *ctx, duk_idx_t nargs)
{
	JSValue res;

	if (!call_from_stack(ctx, nargs, false, &res)) {
		record_error(ctx, res);
		res = JS_UNDEFINED;
	}
	qjsduk_push_value(ctx, res);
}


duk_int_t duk_pcall(duk_context *ctx, duk_idx_t nargs)
{
	JSValue res;
	bool ok;

	ok = call_from_stack(ctx, nargs, false, &res);
	qjsduk_push_value(ctx, res);

	return ok ? DUK_EXEC_SUCCESS : DUK_EXEC_ERROR;
}


duk_int_t duk_pcall_method(duk_context *ctx, duk_idx_t nargs)
{
	JSValue res;
	bool ok;

	ok = call_from_stack(ctx, nargs, true, &res);
	qjsduk_push_value(ctx, res);

	return ok ? DUK_EXEC_SUCCESS : DUK_EXEC_ERROR;
}


duk_int_t
duk_safe_call(duk_context *ctx,
	      duk_safe_call_function func,
	      void *udata,
	      duk_idx_t nargs,
	      duk_idx_t nrets)
{
	struct qjsduk_frame frame;
	duk_idx_t res_base;
	duk_idx_t nres;
	duk_idx_t idx;
	duk_ret_t ret;
	JSValue err;
	bool failed;

	if ((nargs < 0) || (nargs > (ctx->top - ctx->base))) {
		invalid_index(ctx, -nargs);
		return DUK_EXEC_ERROR;
	}

	frame_enter(ctx, &frame, ctx->top - nargs, ctx->this_val);

	ret = func(ctx, udata);

	failed = qjsduk_take_error(ctx, &err);
	if (!failed && (ret < 0)) {
		err = make_error(ctx, ret, "safe call error");
		failed = true;
	}

	/* move the results to the frame base */
	res_base = ctx->base;
	if (failed) {
		pop_to(ctx, res_base);
		qjsduk_push_value(ctx, err);
		nres = 1;
	} else {
		if (ret > (ctx->top - ctx->base)) {
			ret = ctx->top - ctx->base;
		}
		nres = ret;
		for (idx = 0; idx < nres; idx++) {
			qjsduk_slot *src = &ctx->stack[ctx->top - nres + idx];
			qjsduk_slot *dst = &ctx->stack[res_base + idx];
			if (src != dst) {
				release_slot(ctx, dst);
				*dst = *src;
				src->v = JS_UNDEFINED;
				src->cstr = NULL;
				src->kind = QJSDUK_SLOT_VALUE;
			}
		}
		pop_to(ctx, res_base + nres);
	}

	/* adjust to the requested number of results */
	if (nres > nrets) {
		pop_to(ctx, res_base + nrets);
	}
	while (ctx->top < res_base + nrets) {
		qjsduk_push_value(ctx, JS_UNDEFINED);
	}

	/* restore the caller frame keeping the results */
	ctx->base = frame.base;
	ctx->this_val = frame.this_val;
	ctx->has_pending = frame.has_pending;
	ctx->pending = frame.pending;

	return failed ? DUK_EXEC_ERROR : DUK_EXEC_SUCCESS;
}


duk_int_t duk_pcompile(duk_context *ctx, duk_uint_t flags)
{
	const char *src;
	const char *filename;
	size_t srclen;
	char *wrapped;
	JSValue res;

	/* stack is [ ... source filename ] */
	if (!(flags & DUK_COMPILE_FUNCTION)) {
		NSLOG(qjsky, ERROR, "only function compilation is supported");
		duk_pop_2(ctx);
		duk_push_error_object(ctx, DUK_ERR_ERROR,
				      "unsupported compile flags");
		return DUK_EXEC_ERROR;
	}

	filename = duk_safe_to_string(ctx, -1);
	src = duk_safe_to_lstring(ctx, -2, &srclen);

	/* a function expression must be parenthesised to evaluate to the
	 * function, the newline allows a trailing line comment.
	 */
	wrapped = malloc(srclen + 4);
	if (wrapped == NULL) {
		duk_pop_2(ctx);
		duk_push_error_object(ctx, DUK_ERR_ERROR, "out of memory");
		return DUK_EXEC_ERROR;
	}
	wrapped[0] = '(';
	memcpy(wrapped + 1, src, srclen);
	memcpy(wrapped + 1 + srclen, "\n)", 3);

	res = JS_Eval(ctx->jctx, wrapped, srclen + 3, filename,
		      JS_EVAL_TYPE_GLOBAL);
	free(wrapped);

	duk_pop_2(ctx);

	if (JS_IsException(res)) {
		qjsduk_push_value(ctx, JS_GetException(ctx->jctx));
		return DUK_EXEC_ERROR;
	}
	if (!JS_IsFunction(ctx->jctx, res)) {
		JS_FreeValue(ctx->jctx, res);
		duk_push_error_object(ctx, DUK_ERR_SYNTAX_ERROR,
				      "source is not a function");
		return DUK_EXEC_ERROR;
	}

	qjsduk_push_value(ctx, res);
	return DUK_EXEC_SUCCESS;
}


/* errors */

duk_ret_t duk_error(duk_context *ctx, duk_errcode_t err_code, const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	record_error(ctx, make_error(ctx, err_code, msg));

	return DUK_RET_ERROR;
}


/* miscellaneous */

void duk_concat(duk_context *ctx, duk_idx_t count)
{
	JSValue res;

	if ((count < 0) || (count > (ctx->top - ctx->base))) {
		invalid_index(ctx, -count);
		return;
	}

	res = join_strings(ctx, ctx->top - ctx->base - count, count, "");

	duk_pop_n(ctx, count);
	qjsduk_push_value(ctx, res);
}


duk_bool_t duk_strict_equals(duk_context *ctx, duk_idx_t idx1, duk_idx_t idx2)
{
	qjsduk_slot *s1 = get_slot(ctx, idx1);
	qjsduk_slot *s2 = get_slot(ctx, idx2);

	if ((s1 == NULL) || (s2 == NULL)) {
		return 0;
	}
	if ((s1->kind == QJSDUK_SLOT_POINTER) ||
	    (s2->kind == QJSDUK_SLOT_POINTER)) {
		return (s1->kind == s2->kind) && (s1->ptr == s2->ptr);
	}
	return JS_IsStrictEqual(ctx->jctx, s1->v, s2->v);
}


void duk_gc(duk_context *ctx, duk_uint_t flags)
{
	JS_RunGC(JS_GetRuntime(ctx->jctx));
}
