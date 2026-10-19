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
 * QuickJS implementation of javascript engine functions, interfaces
 * used by the generated bindings.
 *
 * The dukky_ functions have the same behaviour as the duktape versions
 * so the binding bodies can be shared between the engines.  Prototypes
 * are identified by PROTO_NAME() which is an index into qjsky_classes.
 */

#ifndef NETSURF_JAVASCRIPT_QUICKJS_QJSKY_H
#define NETSURF_JAVASCRIPT_QUICKJS_QJSKY_H

#include <stdbool.h>

#include <dom/dom.h>

#include "javascript/quickjs/duk_compat.h"

/**
 * A class generated from a WebIDL interface
 */
struct qjsky_class {
	const char *name; /**< interface name */
	int parent; /**< index of parent class or -1 for none */
	size_t priv_size; /**< size of the private data */
	bool primary_global; /**< this is the global object class */

	/**
	 * Initialise private data.
	 *
	 * The object is at stack index 0 followed by the arguments.
	 */
	void (*construct)(duk_context *ctx, void *priv);

	/**
	 * Finalise private data.
	 *
	 * ctx is NULL when called from the garbage collector.
	 */
	void (*destroy)(duk_context *ctx, void *priv);

	const JSCFunctionListEntry *proto_funcs; /**< prototype members */
	int proto_funcs_len; /**< number of prototype members */
	const JSCFunctionListEntry *const_funcs; /**< constants */
	int const_funcs_len; /**< number of constants */

	/** binding code run with the prototype at stack index 0 */
	duk_safe_call_function proto_extra;
};

/**
 * Get the private data of an object for a binding function.
 *
 * \param jctx The QuickJS context
 * \param obj The object the function was called on
 * \param proto The class the function belongs to
 * \return The private data or NULL if obj is not an instance of proto
 */
void *qjsky_get_private(JSContext *jctx, JSValueConst obj, int proto);

/**
 * Throw an error for a binding function called on the wrong object.
 *
 * \return JS_EXCEPTION
 */
JSValue qjsky_illegal_invocation(JSContext *jctx, const char *iface, const char *member);

duk_ret_t dukky_create_object(duk_context *ctx, int proto, int args);
duk_bool_t dukky_push_node(duk_context *ctx, struct dom_node *node);
duk_bool_t dukky_instanceof(duk_context *ctx, duk_idx_t index, int proto);
void dukky_register_event_listener_for(duk_context *ctx,
				       struct dom_element *ele,
				       dom_string *name,
				       bool capture);
bool dukky_get_current_value_of_event_handler(duk_context *ctx,
					      dom_string *name,
					      dom_event_target *et);
void dukky_push_event(duk_context *ctx, dom_event *evt);
bool dukky_event_target_push_listeners(duk_context *ctx, bool dont_create);

typedef enum {
	ELF_CAPTURE = 1 << 0,
	ELF_PASSIVE = 1 << 1,
	ELF_ONCE    = 1 << 2,
	ELF_NONE    = 0
} event_listener_flags;

void dukky_shuffle_array(duk_context *ctx, duk_uarridx_t idx);

/* pcall something, and if it errored, also dump the error to the log */
duk_int_t dukky_pcall(duk_context *ctx, duk_size_t argc, bool reset_timeout);

/* Push a generics function onto the stack */
void dukky_push_generics(duk_context *ctx, const char *generic);

/* Log the current stack frame if possible */
void dukky_log_stack_frame(duk_context *ctx, const char * reason);

#endif
