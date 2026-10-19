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
 * The binding bodies in the .bnd files are written against the duktape
 * value stack API.  This provides the subset of that API they use so
 * they can run unchanged on QuickJS.  It is only used by binding code;
 * object construction, finalisation and prototypes are native QuickJS.
 *
 * Differences from duktape which binding code must allow for:
 *
 * - Errors do not unwind the C stack.  duk_error() records the error
 *   and returns a negative value so it must be used as
 *   "return duk_error(...)".  Errors raised by other calls (e.g. a
 *   throwing getter) are recorded and the call pushes undefined; the
 *   recorded error is thrown when the binding function returns.
 *
 * - Strings starting with 0xFF are hidden property names.  When pushed
 *   they become private symbols which scripts cannot observe.
 *
 * - Pointers only exist on the value stack.  They cannot be stored in
 *   javascript objects.
 */

#ifndef NETSURF_JAVASCRIPT_QUICKJS_DUK_COMPAT_H
#define NETSURF_JAVASCRIPT_QUICKJS_DUK_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
/* binding bodies rely on these as duktape.h includes them */
#include <stdlib.h>
#include <string.h>

#include "javascript/quickjs/engine/quickjs.h"

typedef struct duk_hthread duk_context;

typedef int duk_ret_t;
typedef int duk_idx_t;
typedef int duk_int_t;
typedef unsigned int duk_uint_t;
typedef int duk_bool_t;
typedef size_t duk_size_t;
typedef double duk_double_t;
typedef uint32_t duk_uarridx_t;
typedef uint32_t duk_uint32_t;
typedef int32_t duk_int32_t;
typedef int duk_errcode_t;
typedef int duk_small_int_t;

typedef duk_ret_t (*duk_c_function)(duk_context *ctx);
typedef duk_ret_t (*duk_safe_call_function)(duk_context *ctx, void *udata);

/* execution results */
#define DUK_EXEC_SUCCESS 0
#define DUK_EXEC_ERROR 1

/* error codes */
#define DUK_ERR_NONE 0
#define DUK_ERR_ERROR 1
#define DUK_ERR_EVAL_ERROR 2
#define DUK_ERR_RANGE_ERROR 3
#define DUK_ERR_REFERENCE_ERROR 4
#define DUK_ERR_SYNTAX_ERROR 5
#define DUK_ERR_TYPE_ERROR 6
#define DUK_ERR_URI_ERROR 7

/* returning one of these from a c function throws */
#define DUK_RET_ERROR (-DUK_ERR_ERROR)
#define DUK_RET_EVAL_ERROR (-DUK_ERR_EVAL_ERROR)
#define DUK_RET_RANGE_ERROR (-DUK_ERR_RANGE_ERROR)
#define DUK_RET_REFERENCE_ERROR (-DUK_ERR_REFERENCE_ERROR)
#define DUK_RET_SYNTAX_ERROR (-DUK_ERR_SYNTAX_ERROR)
#define DUK_RET_TYPE_ERROR (-DUK_ERR_TYPE_ERROR)
#define DUK_RET_URI_ERROR (-DUK_ERR_URI_ERROR)

/* value types */
#define DUK_TYPE_NONE 0
#define DUK_TYPE_UNDEFINED 1
#define DUK_TYPE_NULL 2
#define DUK_TYPE_BOOLEAN 3
#define DUK_TYPE_NUMBER 4
#define DUK_TYPE_STRING 5
#define DUK_TYPE_OBJECT 6
#define DUK_TYPE_BUFFER 7
#define DUK_TYPE_POINTER 8
#define DUK_TYPE_LIGHTFUNC 9

#define DUK_VARARGS ((duk_int_t) (-1))

/* duk_def_prop() flags */
#define DUK_DEFPROP_WRITABLE (1U << 0)
#define DUK_DEFPROP_ENUMERABLE (1U << 1)
#define DUK_DEFPROP_CONFIGURABLE (1U << 2)
#define DUK_DEFPROP_HAVE_WRITABLE (1U << 3)
#define DUK_DEFPROP_HAVE_ENUMERABLE (1U << 4)
#define DUK_DEFPROP_HAVE_CONFIGURABLE (1U << 5)
#define DUK_DEFPROP_HAVE_VALUE (1U << 6)
#define DUK_DEFPROP_HAVE_GETTER (1U << 7)
#define DUK_DEFPROP_HAVE_SETTER (1U << 8)
#define DUK_DEFPROP_FORCE (1U << 9)

/* duk_push_buffer_object() types */
#define DUK_BUFOBJ_UINT8CLAMPEDARRAY 4

/* duk_pcompile() flags */
#define DUK_COMPILE_EVAL (1U << 3)
#define DUK_COMPILE_FUNCTION (1U << 4)

/* duk_gc() flags */
#define DUK_GC_COMPACT (1U << 0)


/* stack manipulation */
duk_idx_t duk_get_top(duk_context *ctx);
void duk_set_top(duk_context *ctx, duk_idx_t idx);
duk_idx_t duk_normalize_index(duk_context *ctx, duk_idx_t idx);
void duk_dup(duk_context *ctx, duk_idx_t from_idx);
void duk_dup_top(duk_context *ctx);
void duk_insert(duk_context *ctx, duk_idx_t to_idx);
void duk_remove(duk_context *ctx, duk_idx_t idx);
void duk_replace(duk_context *ctx, duk_idx_t to_idx);
void duk_pop(duk_context *ctx);
void duk_pop_2(duk_context *ctx);
void duk_pop_3(duk_context *ctx);
void duk_pop_n(duk_context *ctx, duk_idx_t count);

/* pushing values */
void duk_push_undefined(duk_context *ctx);
void duk_push_null(duk_context *ctx);
void duk_push_boolean(duk_context *ctx, duk_bool_t val);
void duk_push_int(duk_context *ctx, duk_int_t val);
void duk_push_uint(duk_context *ctx, duk_uint_t val);
void duk_push_number(duk_context *ctx, duk_double_t val);
const char *duk_push_string(duk_context *ctx, const char *str);
const char *duk_push_lstring(duk_context *ctx, const char *str, duk_size_t len);
const char *duk_push_sprintf(duk_context *ctx, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));
void duk_push_pointer(duk_context *ctx, void *p);
duk_idx_t duk_push_object(duk_context *ctx);
duk_idx_t duk_push_array(duk_context *ctx);
void duk_push_this(duk_context *ctx);
void duk_push_global_object(duk_context *ctx);
duk_idx_t duk_push_c_function(duk_context *ctx, duk_c_function func, duk_idx_t nargs);
duk_idx_t duk_push_error_object(duk_context *ctx, duk_errcode_t err_code, const char *fmt, ...)
	__attribute__ ((format (printf, 3, 4)));
void *duk_push_buffer(duk_context *ctx, duk_size_t size, duk_bool_t dynamic);
void duk_push_buffer_object(duk_context *ctx, duk_idx_t idx_buffer, duk_size_t byte_offset, duk_size_t byte_length, duk_uint_t flags);
void duk_push_context_dump(duk_context *ctx);

#define duk_push_bool(ctx, val) duk_push_boolean((ctx), (val))

/* type checks */
duk_int_t duk_get_type(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_check_type(duk_context *ctx, duk_idx_t idx, duk_int_t type);
duk_bool_t duk_is_undefined(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_is_null(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_is_boolean(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_is_number(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_is_string(duk_context *ctx, duk_idx_t idx);

/* reading values without coercion */
duk_bool_t duk_get_boolean(duk_context *ctx, duk_idx_t idx);
duk_int_t duk_get_int(duk_context *ctx, duk_idx_t idx);
duk_uint_t duk_get_uint(duk_context *ctx, duk_idx_t idx);
void *duk_get_pointer(duk_context *ctx, duk_idx_t idx);
const char *duk_get_string(duk_context *ctx, duk_idx_t idx);
duk_size_t duk_get_length(duk_context *ctx, duk_idx_t idx);
duk_bool_t duk_opt_boolean(duk_context *ctx, duk_idx_t idx, duk_bool_t def_value);
duk_bool_t duk_require_boolean(duk_context *ctx, duk_idx_t idx);
duk_int_t duk_require_int(duk_context *ctx, duk_idx_t idx);
duk_double_t duk_require_number(duk_context *ctx, duk_idx_t idx);
const char *duk_require_string(duk_context *ctx, duk_idx_t idx);
const char *duk_require_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len);

/* coercion, replacing the value in place */
duk_bool_t duk_to_boolean(duk_context *ctx, duk_idx_t idx);
duk_int_t duk_to_int(duk_context *ctx, duk_idx_t idx);
duk_uint_t duk_to_uint(duk_context *ctx, duk_idx_t idx);
duk_uint32_t duk_to_uint32(duk_context *ctx, duk_idx_t idx);
const char *duk_to_string(duk_context *ctx, duk_idx_t idx);
const char *duk_to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len);
const char *duk_safe_to_string(duk_context *ctx, duk_idx_t idx);
const char *duk_safe_to_lstring(duk_context *ctx, duk_idx_t idx, duk_size_t *out_len);
const char *duk_safe_to_stacktrace(duk_context *ctx, duk_idx_t idx);

/* properties */
duk_bool_t duk_get_prop(duk_context *ctx, duk_idx_t obj_idx);
duk_bool_t duk_get_prop_string(duk_context *ctx, duk_idx_t obj_idx, const char *key);
duk_bool_t duk_get_prop_index(duk_context *ctx, duk_idx_t obj_idx, duk_uarridx_t arr_idx);
duk_bool_t duk_put_prop(duk_context *ctx, duk_idx_t obj_idx);
duk_bool_t duk_put_prop_string(duk_context *ctx, duk_idx_t obj_idx, const char *key);
duk_bool_t duk_put_prop_index(duk_context *ctx, duk_idx_t obj_idx, duk_uarridx_t arr_idx);
duk_bool_t duk_del_prop(duk_context *ctx, duk_idx_t obj_idx);
duk_bool_t duk_del_prop_string(duk_context *ctx, duk_idx_t obj_idx, const char *key);
duk_bool_t duk_del_prop_index(duk_context *ctx, duk_idx_t obj_idx, duk_uarridx_t arr_idx);
duk_bool_t duk_has_prop(duk_context *ctx, duk_idx_t obj_idx);
void duk_def_prop(duk_context *ctx, duk_idx_t obj_idx, duk_uint_t flags);
duk_bool_t duk_get_global_string(duk_context *ctx, const char *key);
duk_bool_t duk_put_global_string(duk_context *ctx, const char *key);
void duk_get_prototype(duk_context *ctx, duk_idx_t idx);
void duk_set_prototype(duk_context *ctx, duk_idx_t idx);

/* calls */
void duk_call(duk_context *ctx, duk_idx_t nargs);
duk_int_t duk_pcall(duk_context *ctx, duk_idx_t nargs);
duk_int_t duk_pcall_method(duk_context *ctx, duk_idx_t nargs);
duk_int_t duk_safe_call(duk_context *ctx, duk_safe_call_function func, void *udata, duk_idx_t nargs, duk_idx_t nrets);
duk_int_t duk_pcompile(duk_context *ctx, duk_uint_t flags);

/* errors */
duk_ret_t duk_error(duk_context *ctx, duk_errcode_t err_code, const char *fmt, ...)
	__attribute__ ((format (printf, 3, 4)));
#define duk_generic_error(ctx, ...) duk_error((ctx), DUK_ERR_ERROR, __VA_ARGS__)

/* miscellaneous */
void duk_concat(duk_context *ctx, duk_idx_t count);
duk_bool_t duk_strict_equals(duk_context *ctx, duk_idx_t idx1, duk_idx_t idx2);
void duk_gc(duk_context *ctx, duk_uint_t flags);


/*
 * Interfaces used by the QuickJS glue rather than binding bodies.
 */

/** The body of a generated binding function */
typedef duk_ret_t (*qjsduk_body_fn)(duk_context *ctx, void *priv);

/**
 * Lookup a private pointer for an object.
 *
 * Implemented by the glue so the compatibility layer can resolve the
 * duktape private magic property.
 *
 * \param ctx The compatibility context
 * \param obj The object to examine
 * \return The private pointer or NULL if obj is not a binding object
 */
void *qjsduk_lookup_private(duk_context *ctx, JSValueConst obj);

/**
 * Create a compatibility context for a QuickJS context.
 *
 * The QuickJS context opaque pointer is set to the new context.
 *
 * \param jctx The QuickJS context
 * \param udata Glue data for the context
 * \return The new context or NULL on memory exhaustion
 */
duk_context *qjsduk_create(JSContext *jctx, void *udata);

/**
 * Destroy a compatibility context, releasing all values it holds.
 *
 * Must be called before the QuickJS context is freed.
 */
void qjsduk_destroy(duk_context *ctx);

/** Get the compatibility context for a QuickJS context */
duk_context *qjsduk_from_js(JSContext *jctx);

/** Get the QuickJS context of a compatibility context */
JSContext *qjsduk_js(duk_context *ctx);

/** Get the glue data of a compatibility context */
void *qjsduk_udata(duk_context *ctx);

/**
 * Call a binding body from a QuickJS C function.
 *
 * The arguments are placed on a new stack frame, the body run and its
 * result or error converted to a QuickJS return value.
 *
 * \param jctx The QuickJS context
 * \param this_val The javascript this value
 * \param argc The number of arguments
 * \param argv The arguments
 * \param body The binding body to call
 * \param priv The private data passed to the body
 * \return The result value or JS_EXCEPTION
 */
JSValue qjsduk_invoke(JSContext *jctx, JSValueConst this_val, int argc, JSValueConst *argv, qjsduk_body_fn body, void *priv);

/**
 * Push a QuickJS value onto the stack, taking ownership of it.
 *
 * An exception value records the pending exception and pushes undefined.
 */
void qjsduk_push_value(duk_context *ctx, JSValue val);

/**
 * Get a new reference to a value on the stack.
 *
 * \return The value or undefined for pointers and invalid indexes
 */
JSValue qjsduk_get_value(duk_context *ctx, duk_idx_t idx);

/**
 * Take the pending error, if any.
 *
 * \param ctx The compatibility context
 * \param err Updated with the error which the caller must free
 * \return true if there was a pending error
 */
bool qjsduk_take_error(duk_context *ctx, JSValue *err);

/**
 * Get the private symbol used for a hidden property name.
 *
 * \param ctx The compatibility context
 * \param name The hidden name, starting with 0xFF
 * \return The atom which remains owned by the context or JS_ATOM_NULL
 */
JSAtom qjsduk_magic_atom(duk_context *ctx, const char *name);

#endif
