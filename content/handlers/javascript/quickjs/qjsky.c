/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2015 Daniel Dilverstone <dsilvers@netsurf-browser.org>
 * Copyright 2016 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2016 John-Mark Bell <jmb@netsurf-browser.org>
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
 * QuickJS-ng implementation of javascript engine functions.
 *
 * A jsheap is a QuickJS runtime and each jsthread is a context within
 * that runtime.  Contexts in one runtime share the garbage collector
 * but each has its own global object.
 *
 * Binding classes are registered with each runtime and their prototypes
 * created in each context.  The global object of a context is the
 * Window; it keeps its QuickJS builtins and gets the Window prototype.
 *
 * The node and event handling is shared in structure with the duktape
 * implementation and uses the duktape compatibility value stack.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <nsutils/time.h>

#include "netsurf/inttypes.h"
#include "utils/utils.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "utils/nsurl.h"
#include "utils/utf8.h"
#include "content/content.h"
#include "content/hlcache.h"
#include "content/llcache.h"
#include "utils/ascii.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"

#include "javascript/js.h"
#include "javascript/content.h"
#include "javascript/script_fetch.h"
#include "html/private.h"

#include "quickjs/binding.h"
#include "quickjs/private.h"
#include "quickjs/generics.js.inc"
#include "quickjs/polyfill.js.inc"
#include "quickjs/webcompat.js.inc"

#include "engine/quickjs.h"
#include "duk_compat.h"
#include "qjsky.h"

#include <dom/dom.h>

/* QuickJS reinterprets doubles as 64 bit integers, which assumes the
 * IEEE word order.  The FPA format used by the GCCSDK 4 RISC OS
 * toolchain (arm-unknown-riscos) stores the words the other way round
 * so numbers would be silently corrupted.  Use the EABI toolchain
 * (arm-riscos-gnueabi) instead.
 */
#if defined(__FLOAT_WORD_ORDER__) && defined(__BYTE_ORDER__) && \
	(__FLOAT_WORD_ORDER__ != __BYTE_ORDER__)
#error "QuickJS requires doubles in IEEE word order"
#endif

#define EVENT_MAGIC MAGIC(EVENT_MAP)
#define HANDLER_LISTENER_MAGIC MAGIC(HANDLER_LISTENER_MAP)
#define HANDLER_MAGIC MAGIC(HANDLER_MAP)
#define EVENT_LISTENER_JS_MAGIC MAGIC(EVENT_LISTENER_JS_MAP)
#define GENERICS_MAGIC MAGIC(GENERICS_TABLE)

/** Wallclock time a script may run before it is interrupted */
#define JS_EXEC_TIMEOUT_MS 10000 /* 10 seconds */

/**
 * qjsky javascript heap
 */
struct jsheap {
	JSRuntime *rt; /**< QuickJS runtime */
	bool pending_destroy; /**< Whether this heap is pending destruction */
	unsigned int live_threads; /**< number of live threads */
	uint64_t exec_start_time; /**< when the current execution began */
	unsigned int depth; /**< nesting of calls into javascript */
	JSClassID class_id[QJSKY_PROTO__COUNT]; /**< binding class ids */
	struct qjsky_rejection *rejections; /**< unhandled promise rejections */
	unsigned int rejection_count; /**< number of unhandled rejections */
	unsigned int rejection_alloc; /**< allocated size of rejections */
};

/**
 * A promise rejection with no handler, reported at the microtask
 * checkpoint unless one is added.
 */
struct qjsky_rejection {
	JSContext *jctx; /**< context of the promise */
	JSValue promise; /**< the rejected promise */
	JSValue reason; /**< the rejection reason */
};

/**
 * qjsky javascript thread
 */
struct jsthread {
	bool pending_destroy; /**< Whether this thread is pending destruction */
	unsigned int in_use; /**< The number of times this thread is in use */
	jsheap *heap; /**< The heap this thread belongs to */
	JSContext *jctx; /**< The QuickJS context */
	duk_context *ctx; /**< The compatibility context */
	JSValue global; /**< The global object */
	void *window_priv; /**< private data of the global Window */
	struct qjsky_fetch *fetches; /**< requests in progress */
	int32_t next_fetch_id; /**< identifier for the next request */
	struct qjsky_module *modules; /**< modules fetched or compiled */
	struct js_module *module_loads; /**< module scripts and imports */
	unsigned int inline_modules; /**< number of modules in the document */
};

/**
 * A network request made by a script.
 */
struct qjsky_fetch {
	struct qjsky_fetch *next; /**< next request of the thread */
	struct qjsky_fetch *prev; /**< previous request of the thread */
	jsthread *thread; /**< thread which made the request */
	struct script_fetch *sf; /**< the request */
	JSValue callback; /**< function to call with the response */
	int32_t id; /**< identifier given to the script */
};


/**
 * Get the thread of a compatibility context.
 */
static inline jsthread *qjsky_thread(duk_context *ctx)
{
	return qjsduk_udata(ctx);
}


/**
 * Get the class index of a class id.
 *
 * \return the index or -1 if the class is not a binding class
 */
static int qjsky_class_index(jsheap *heap, JSClassID cid)
{
	if ((cid >= heap->class_id[0]) &&
	    (cid < heap->class_id[0] + QJSKY_PROTO__COUNT)) {
		return cid - heap->class_id[0];
	}
	return -1;
}


/**
 * Check if a class is, or inherits from, another class.
 */
static bool qjsky_class_is_a(int idx, int proto)
{
	while (idx >= 0) {
		if (idx == proto) {
			return true;
		}
		idx = qjsky_classes[idx]->parent;
	}
	return false;
}


/**
 * Check if a value is the global object of a thread.
 */
static bool qjsky_is_global(jsthread *thread, JSValueConst obj)
{
	return JS_IsObject(obj) &&
		JS_IsStrictEqual(thread->jctx, obj, thread->global);
}


/**
 * Get the binding class index of an object.
 *
 * \return the index or -1 if obj is not a binding object
 */
static int qjsky_object_class(jsthread *thread, JSValueConst obj)
{
	if (qjsky_is_global(thread, obj)) {
		return QJSKY_PROTO_GLOBAL;
	}
	if (!JS_IsObject(obj)) {
		return -1;
	}
	return qjsky_class_index(thread->heap, JS_GetClassID(obj));
}


/** Maximum depth of proxies to look through for a binding object */
#define QJSKY_PROXY_DEPTH 8

/**
 * Find the binding class and private data of an object.
 *
 * Binding objects wrapped in proxies, such as the list proxies made by
 * generics.js, are resolved to the object they wrap.
 *
 * \param thread The thread the object belongs to
 * \param obj The object to examine
 * \param priv Updated with the private data
 * \return The class index or -1 if obj is not a binding object
 */
static int qjsky_resolve(jsthread *thread, JSValueConst obj, void **priv)
{
	JSValue cur = JS_DupValue(thread->jctx, obj);
	int depth = 0;
	int idx;

	while (JS_IsProxy(cur) && (depth++ < QJSKY_PROXY_DEPTH)) {
		JSValue target = JS_GetProxyTarget(thread->jctx, cur);
		JS_FreeValue(thread->jctx, cur);
		if (JS_IsException(target)) {
			/* revoked proxy */
			JS_FreeValue(thread->jctx, JS_GetException(thread->jctx));
			return -1;
		}
		cur = target;
	}

	idx = qjsky_object_class(thread, cur);
	if (idx == QJSKY_PROTO_GLOBAL) {
		*priv = thread->window_priv;
	} else if (idx >= 0) {
		*priv = JS_GetOpaque(cur, thread->heap->class_id[idx]);
	}

	/* the object, and so its private data, is kept alive by obj */
	JS_FreeValue(thread->jctx, cur);

	return idx;
}


/* exported interface documented in duk_compat.h */
void *qjsduk_lookup_private(duk_context *ctx, JSValueConst obj)
{
	void *priv = NULL;

	if (qjsky_resolve(qjsky_thread(ctx), obj, &priv) < 0) {
		return NULL;
	}
	return priv;
}


/* exported interface documented in qjsky.h */
void *qjsky_get_private(JSContext *jctx, JSValueConst obj, int proto)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	void *priv = NULL;
	int idx;

	if (qjsky_classes[proto]->primary_global) {
		/* the global is the only instance */
		return thread->window_priv;
	}

	if (JS_IsUndefined(obj)) {
		/* called without an object, use the global */
		obj = thread->global;
	}

	idx = qjsky_resolve(thread, obj, &priv);
	if (!qjsky_class_is_a(idx, proto)) {
		return NULL;
	}
	return priv;
}


/* exported interface documented in qjsky.h */
JSValue
qjsky_illegal_invocation(JSContext *jctx, const char *iface, const char *member)
{
	return JS_ThrowTypeError(jctx, "%s.%s called on an object which is not a %s",
				 iface, member, iface);
}


/* exported interface documented in qjsky.h */
duk_bool_t dukky_instanceof(duk_context *ctx, duk_idx_t index, int proto)
{
	jsthread *thread = qjsky_thread(ctx);
	JSValue obj = qjsduk_get_value(ctx, index);
	void *priv;
	bool res;

	res = qjsky_class_is_a(qjsky_resolve(thread, obj, &priv), proto);
	JS_FreeValue(thread->jctx, obj);

	return res;
}


/**
 * Finaliser for all binding classes.
 */
static void qjsky_finalizer(JSRuntime *rt, JSValueConst val)
{
	jsheap *heap = JS_GetRuntimeOpaque(rt);
	JSClassID cid = JS_GetClassID(val);
	int idx = qjsky_class_index(heap, cid);
	void *priv;

	if (idx < 0) {
		return;
	}

	priv = JS_GetOpaque(val, cid);
	if (priv != NULL) {
		qjsky_classes[idx]->destroy(NULL, priv);
		free(priv);
	}
}


/**
 * Data for constructing an object.
 */
struct qjsky_construct {
	const struct qjsky_class *cls;
	void *priv;
};


/**
 * Safe call function which runs a class initialiser.
 */
static duk_ret_t qjsky_construct_call(duk_context *ctx, void *udata)
{
	struct qjsky_construct *cons = udata;

	cons->cls->construct(ctx, cons->priv);
	return 0;
}


/**
 * Initialise a binding object.
 *
 * The initialiser arguments are on the stack top.  On success they are
 * replaced with the object, on error with the error.
 *
 * \param ctx The compatibility context
 * \param proto The class of the object
 * \param obj The object, ownership is taken
 * \param priv The private data of the object
 * \param nargs The number of initialiser arguments
 * \return DUK_EXEC_SUCCESS or DUK_EXEC_ERROR
 */
static duk_ret_t
qjsky_init_object(duk_context *ctx, int proto, JSValue obj, void *priv, int nargs)
{
	struct qjsky_construct cons;

	cons.cls = qjsky_classes[proto];
	cons.priv = priv;

	/* ... args */
	qjsduk_push_value(ctx, JS_DupValue(qjsduk_js(ctx), obj));
	/* ... args obj */
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, HANDLER_LISTENER_MAGIC);
	duk_push_object(ctx);
	duk_put_prop_string(ctx, -2, HANDLER_MAGIC);
	duk_insert(ctx, -(nargs + 1));
	/* ... obj args */
	if (duk_safe_call(ctx, qjsky_construct_call, &cons, nargs + 1, 1) !=
	    DUK_EXEC_SUCCESS) {
		/* ... err */
		JS_FreeValue(qjsduk_js(ctx), obj);
		return DUK_EXEC_ERROR;
	}
	/* ... undefined */
	duk_pop(ctx);
	qjsduk_push_value(ctx, obj);
	/* ... obj */
	return DUK_EXEC_SUCCESS;
}


/* exported interface documented in qjsky.h */
duk_ret_t dukky_create_object(duk_context *ctx, int proto, int args)
{
	jsthread *thread = qjsky_thread(ctx);
	JSValue obj;
	void *priv;

	NSLOG(qjsky, DEEPDEBUG, "class=%s nargs=%d",
	      qjsky_classes[proto]->name, args);

	priv = calloc(1, qjsky_classes[proto]->priv_size);
	if (priv == NULL) {
		duk_pop_n(ctx, args);
		duk_push_error_object(ctx, DUK_ERR_ERROR, "out of memory");
		return DUK_EXEC_ERROR;
	}

	obj = JS_NewObjectClass(thread->jctx, thread->heap->class_id[proto]);
	if (JS_IsException(obj)) {
		free(priv);
		duk_pop_n(ctx, args);
		qjsduk_push_value(ctx, JS_GetException(thread->jctx));
		return DUK_EXEC_ERROR;
	}
	/* the finaliser frees the private data from here on */
	JS_SetOpaque(obj, priv);

	return qjsky_init_object(ctx, proto, obj, priv, args);
}


/**
 * Prototype for interface objects which cannot be constructed.
 */
static JSValue
qjsky_bad_constructor(JSContext *jctx,
		      JSValueConst this_val,
		      int argc,
		      JSValueConst *argv)
{
	return JS_ThrowTypeError(jctx, "Illegal constructor");
}


/**
 * Create the binding prototypes and interface objects in a thread.
 *
 * \param thread The thread to create them in
 * \return NSERROR_OK on success
 */
static nserror qjsky_create_prototypes(jsthread *thread)
{
	JSContext *jctx = thread->jctx;
	int idx;

	for (idx = 0; idx < QJSKY_PROTO__COUNT; idx++) {
		const struct qjsky_class *cls = qjsky_classes[idx];
		JSValue proto;
		JSValue ctor;

		if (cls->parent >= 0) {
			JSValue parent;

			assert(cls->parent < idx);
			parent = JS_GetClassProto(jctx,
					thread->heap->class_id[cls->parent]);
			proto = JS_NewObjectProto(jctx, parent);
			JS_FreeValue(jctx, parent);
		} else {
			proto = JS_NewObject(jctx);
		}
		if (JS_IsException(proto)) {
			return NSERROR_NOMEM;
		}

		JS_SetPropertyFunctionList(jctx, proto,
					   cls->proto_funcs,
					   cls->proto_funcs_len);
		if (cls->const_funcs_len > 0) {
			JS_SetPropertyFunctionList(jctx, proto,
						   cls->const_funcs,
						   cls->const_funcs_len);
		}

		/* the interface object */
		ctor = JS_NewCFunction2(jctx, qjsky_bad_constructor, cls->name,
					0, JS_CFUNC_constructor_or_func, 0);
		if (JS_IsException(ctor)) {
			JS_FreeValue(jctx, proto);
			return NSERROR_NOMEM;
		}
		JS_SetConstructor(jctx, ctor, proto);
		if (cls->const_funcs_len > 0) {
			JS_SetPropertyFunctionList(jctx, ctor,
						   cls->const_funcs,
						   cls->const_funcs_len);
		}
		JS_DefinePropertyValueStr(jctx, thread->global, cls->name, ctor,
					  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

		if (cls->proto_extra != NULL) {
			qjsduk_push_value(thread->ctx, JS_DupValue(jctx, proto));
			if (duk_safe_call(thread->ctx, cls->proto_extra,
					  NULL, 1, 1) != DUK_EXEC_SUCCESS) {
				NSLOG(qjsky, WARNING,
				      "Failed to complete prototype for %s: %s",
				      cls->name,
				      duk_safe_to_string(thread->ctx, -1));
			}
			duk_pop(thread->ctx);
		}

		JS_SetClassProto(jctx, thread->heap->class_id[idx], proto);
	}

	return NSERROR_OK;
}


#define SET_HTML_CLASS(CLASS) \
		*html_class = PROTO_NAME(HTML##CLASS##ELEMENT);

static void dukky_html_element_class_from_tag_type(dom_html_element_type type,
		int *html_class)
{
	switch(type) {
	case DOM_HTML_ELEMENT_TYPE_HTML:
		SET_HTML_CLASS(HTML)
		break;
	case DOM_HTML_ELEMENT_TYPE_HEAD:
		SET_HTML_CLASS(HEAD)
		break;
	case DOM_HTML_ELEMENT_TYPE_META:
		SET_HTML_CLASS(META)
		break;
	case DOM_HTML_ELEMENT_TYPE_BASE:
		SET_HTML_CLASS(BASE)
		break;
	case DOM_HTML_ELEMENT_TYPE_TITLE:
		SET_HTML_CLASS(TITLE)
		break;
	case DOM_HTML_ELEMENT_TYPE_BODY:
		SET_HTML_CLASS(BODY)
		break;
	case DOM_HTML_ELEMENT_TYPE_DIV:
		SET_HTML_CLASS(DIV)
		break;
	case DOM_HTML_ELEMENT_TYPE_FORM:
		SET_HTML_CLASS(FORM)
		break;
	case DOM_HTML_ELEMENT_TYPE_LINK:
		SET_HTML_CLASS(LINK)
		break;
	case DOM_HTML_ELEMENT_TYPE_BUTTON:
		SET_HTML_CLASS(BUTTON)
		break;
	case DOM_HTML_ELEMENT_TYPE_INPUT:
		SET_HTML_CLASS(INPUT)
		break;
	case DOM_HTML_ELEMENT_TYPE_TEXTAREA:
		SET_HTML_CLASS(TEXTAREA)
		break;
	case DOM_HTML_ELEMENT_TYPE_OPTGROUP:
		SET_HTML_CLASS(OPTGROUP)
		break;
	case DOM_HTML_ELEMENT_TYPE_OPTION:
		SET_HTML_CLASS(OPTION)
		break;
	case DOM_HTML_ELEMENT_TYPE_SELECT:
		SET_HTML_CLASS(SELECT)
		break;
	case DOM_HTML_ELEMENT_TYPE_HR:
		SET_HTML_CLASS(HR)
		break;
	case DOM_HTML_ELEMENT_TYPE_DL:
		SET_HTML_CLASS(DLIST)
		break;
	case DOM_HTML_ELEMENT_TYPE_DIR:
		SET_HTML_CLASS(DIRECTORY)
		break;
	case DOM_HTML_ELEMENT_TYPE_MENU:
		SET_HTML_CLASS(MENU)
		break;
	case DOM_HTML_ELEMENT_TYPE_FIELDSET:
		SET_HTML_CLASS(FIELDSET)
		break;
	case DOM_HTML_ELEMENT_TYPE_LEGEND:
		SET_HTML_CLASS(LEGEND)
		break;
	case DOM_HTML_ELEMENT_TYPE_P:
		SET_HTML_CLASS(PARAGRAPH)
		break;
	case DOM_HTML_ELEMENT_TYPE_H1:
	case DOM_HTML_ELEMENT_TYPE_H2:
	case DOM_HTML_ELEMENT_TYPE_H3:
	case DOM_HTML_ELEMENT_TYPE_H4:
	case DOM_HTML_ELEMENT_TYPE_H5:
	case DOM_HTML_ELEMENT_TYPE_H6:
		SET_HTML_CLASS(HEADING)
		break;
	case DOM_HTML_ELEMENT_TYPE_BLOCKQUOTE:
	case DOM_HTML_ELEMENT_TYPE_Q:
		SET_HTML_CLASS(QUOTE)
		break;
	case DOM_HTML_ELEMENT_TYPE_PRE:
		SET_HTML_CLASS(PRE)
		break;
	case DOM_HTML_ELEMENT_TYPE_BR:
		SET_HTML_CLASS(BR)
		break;
	case DOM_HTML_ELEMENT_TYPE_LABEL:
		SET_HTML_CLASS(LABEL)
		break;
	case DOM_HTML_ELEMENT_TYPE_UL:
		SET_HTML_CLASS(ULIST)
		break;
	case DOM_HTML_ELEMENT_TYPE_OL:
		SET_HTML_CLASS(OLIST)
		break;
	case DOM_HTML_ELEMENT_TYPE_LI:
		SET_HTML_CLASS(LI)
		break;
	case DOM_HTML_ELEMENT_TYPE_FONT:
		SET_HTML_CLASS(FONT)
		break;
	case DOM_HTML_ELEMENT_TYPE_DEL:
	case DOM_HTML_ELEMENT_TYPE_INS:
		SET_HTML_CLASS(MOD)
		break;
	case DOM_HTML_ELEMENT_TYPE_A:
		SET_HTML_CLASS(ANCHOR)
		break;
	case DOM_HTML_ELEMENT_TYPE_BASEFONT:
		/* there is no binding for this obsolete element */
		SET_HTML_CLASS(UNKNOWN)
		break;
	case DOM_HTML_ELEMENT_TYPE_IMG:
		SET_HTML_CLASS(IMAGE)
		break;
	case DOM_HTML_ELEMENT_TYPE_OBJECT:
		SET_HTML_CLASS(OBJECT)
		break;
	case DOM_HTML_ELEMENT_TYPE_PARAM:
		SET_HTML_CLASS(PARAM)
		break;
	case DOM_HTML_ELEMENT_TYPE_APPLET:
		SET_HTML_CLASS(APPLET)
		break;
	case DOM_HTML_ELEMENT_TYPE_MAP:
		SET_HTML_CLASS(MAP)
		break;
	case DOM_HTML_ELEMENT_TYPE_AREA:
		SET_HTML_CLASS(AREA)
		break;
	case DOM_HTML_ELEMENT_TYPE_SCRIPT:
		SET_HTML_CLASS(SCRIPT)
		break;
	case DOM_HTML_ELEMENT_TYPE_CAPTION:
		SET_HTML_CLASS(TABLECAPTION)
		break;
	case DOM_HTML_ELEMENT_TYPE_TD:
	case DOM_HTML_ELEMENT_TYPE_TH:
		SET_HTML_CLASS(TABLECELL)
		break;
	case DOM_HTML_ELEMENT_TYPE_COL:
	case DOM_HTML_ELEMENT_TYPE_COLGROUP:
		SET_HTML_CLASS(TABLECOL)
		break;
	case DOM_HTML_ELEMENT_TYPE_THEAD:
	case DOM_HTML_ELEMENT_TYPE_TBODY:
	case DOM_HTML_ELEMENT_TYPE_TFOOT:
		SET_HTML_CLASS(TABLESECTION)
		break;
	case DOM_HTML_ELEMENT_TYPE_TABLE:
		SET_HTML_CLASS(TABLE)
		break;
	case DOM_HTML_ELEMENT_TYPE_TR:
		SET_HTML_CLASS(TABLEROW)
		break;
	case DOM_HTML_ELEMENT_TYPE_STYLE:
		SET_HTML_CLASS(STYLE)
		break;
	case DOM_HTML_ELEMENT_TYPE_FRAMESET:
		SET_HTML_CLASS(FRAMESET)
		break;
	case DOM_HTML_ELEMENT_TYPE_FRAME:
		SET_HTML_CLASS(FRAME)
		break;
	case DOM_HTML_ELEMENT_TYPE_IFRAME:
		SET_HTML_CLASS(IFRAME)
		break;
	case DOM_HTML_ELEMENT_TYPE_ISINDEX:
		/* there is no binding for this obsolete element */
		SET_HTML_CLASS(UNKNOWN)
		break;
	case DOM_HTML_ELEMENT_TYPE_CANVAS:
		SET_HTML_CLASS(CANVAS)
		break;
	case DOM_HTML_ELEMENT_TYPE__COUNT:
		assert(type != DOM_HTML_ELEMENT_TYPE__COUNT);
		fallthrough;
	case DOM_HTML_ELEMENT_TYPE__UNKNOWN:
		SET_HTML_CLASS(UNKNOWN)
		break;
	default:
		/* Known HTML element without a specialisation */
		*html_class = PROTO_NAME(HTMLELEMENT);
		break;
	}
	return;
}

#undef SET_HTML_CLASS


/**
 * Get the binding class for a node.
 */
static int dukky_node_klass(struct dom_node *node)
{
	dom_node_type nodetype;
	dom_exception err;
	int klass = PROTO_NAME(NODE);

	err = dom_node_get_node_type(node, &nodetype);
	if (err != DOM_NO_ERR) {
		/* Oh bum, just node then */
		return klass;
	}

	switch(nodetype) {
	case DOM_ELEMENT_NODE: {
		dom_string *namespace;
		dom_html_element_type type;
		err = dom_node_get_namespace(node, &namespace);
		if (err != DOM_NO_ERR) {
			/* Feck it, element */
			NSLOG(qjsky, ERROR,
			      "dom_node_get_namespace() failed");
			klass = PROTO_NAME(ELEMENT);
			break;
		}
		if (namespace == NULL) {
			/* No namespace, -> element */
			NSLOG(qjsky, DEBUG, "no namespace");
			klass = PROTO_NAME(ELEMENT);
			break;
		}

		if (dom_string_isequal(namespace, corestring_dom_html_namespace) == false) {
			/* definitely not an HTML element of some kind */
			klass = PROTO_NAME(ELEMENT);
			dom_string_unref(namespace);
			break;
		}
		dom_string_unref(namespace);

		err = dom_html_element_get_tag_type(node, &type);
		if (err != DOM_NO_ERR) {
			type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
		}

		dukky_html_element_class_from_tag_type(type, &klass);
		break;
	}
	case DOM_TEXT_NODE:
		klass = PROTO_NAME(TEXT);
		break;
	case DOM_COMMENT_NODE:
		klass = PROTO_NAME(COMMENT);
		break;
	case DOM_DOCUMENT_NODE:
		klass = PROTO_NAME(DOCUMENT);
		break;
	case DOM_DOCUMENT_FRAGMENT_NODE:
		klass = PROTO_NAME(DOCUMENTFRAGMENT);
		break;
	case DOM_DOCUMENT_TYPE_NODE:
		klass = PROTO_NAME(DOCUMENTTYPE);
		break;
	case DOM_CDATA_SECTION_NODE:
		/* CDATASection has no binding of its own */
		klass = PROTO_NAME(TEXT);
		break;
	case DOM_ATTRIBUTE_NODE:
		klass = PROTO_NAME(ATTR);
		break;
	case DOM_PROCESSING_INSTRUCTION_NODE:
	case DOM_NOTATION_NODE:
	case DOM_ENTITY_REFERENCE_NODE:
	case DOM_ENTITY_NODE:
	default:
		/* Oh bum, just node then */
		break;
	}

	return klass;
}


/* exported interface documented in qjsky.h */
duk_bool_t dukky_push_node(duk_context *ctx, struct dom_node *node)
{
	NSLOG(qjsky, DEEPDEBUG, "Pushing node %p", node);
	/* First check if we can find the node */
	/* ... */
	duk_get_global_string(ctx, NODE_MAGIC);
	/* ... nodes */
	duk_push_pointer(ctx, node);
	/* ... nodes nodeptr */
	duk_get_prop(ctx, -2);
	/* ... nodes node/undefined */
	if (!duk_is_undefined(ctx, -1)) {
		/* ... nodes node */
		duk_insert(ctx, -2);
		/* ... node nodes */
		duk_pop(ctx);
		/* ... node */
		return true;
	}
	/* ... nodes undefined */
	duk_pop(ctx);
	/* ... nodes */

	/* We couldn't, so now we determine the node type and then
	 * we ask for it to be created
	 */
	duk_push_pointer(ctx, node);
	/* ... nodes nodeptr */
	if (dukky_create_object(ctx, dukky_node_klass(node), 1) !=
	    DUK_EXEC_SUCCESS) {
		/* ... nodes err */
		NSLOG(qjsky, ERROR, "Failed to create node object: %s",
		      duk_safe_to_string(ctx, -1));
		duk_pop_2(ctx);
		/* ... */
		duk_push_undefined(ctx);
		return false;
	}
	/* ... nodes node */
	duk_push_pointer(ctx, node);
	/* ... nodes node nodeptr */
	duk_dup(ctx, -2);
	/* ... nodes node nodeptr node */
	duk_put_prop(ctx, -4);
	/* ... nodes node */
	duk_insert(ctx, -2);
	/* ... node nodes */
	duk_pop(ctx);
	/* ... node */
	return true;
}


/**
 * Interrupt handler which stops scripts that run for too long.
 *
 * \param rt The runtime being executed
 * \param opaque The heap which owns the runtime
 * \return non zero to interrupt execution
 */
static int qjsky_check_timeout(JSRuntime *rt, void *opaque)
{
	jsheap *heap = opaque;
	uint64_t now;

	/* This may be called during construction, so only test for
	 * execution timeout if a start time has been recorded.
	 */
	if (heap->exec_start_time == 0) {
		return 0;
	}

	(void) nsu_getmonotonic_ms(&now);

	if (now > (heap->exec_start_time + JS_EXEC_TIMEOUT_MS)) {
		NSLOG(qjsky, WARNING, "Javascript execution timed out");
		return 1;
	}
	return 0;
}


/**
 * Log an uncaught javascript error value.
 *
 * \param jctx The context the error belongs to
 * \param what Description of how the error was raised
 * \param err The thrown value, usually an Error object
 */
static void
qjsky_log_error(JSContext *jctx, const char *what, JSValueConst err)
{
	JSValue stack;
	const char *msg;
	const char *trace = NULL;

	msg = JS_ToCString(jctx, err);
	if (msg == NULL) {
		/* converting the value threw, discard that exception */
		JS_FreeValue(jctx, JS_GetException(jctx));
	}

	if (JS_IsError(err)) {
		stack = JS_GetPropertyStr(jctx, err, "stack");
		if (JS_IsString(stack)) {
			trace = JS_ToCString(jctx, stack);
		}
		JS_FreeValue(jctx, stack);
	}

	NSLOG(jserrors, WARNING, "%s: %s%s%s",
	      what,
	      msg != NULL ? msg : "[unprintable]",
	      trace != NULL ? "\n" : "",
	      trace != NULL ? trace : "");

	JS_FreeCString(jctx, trace);
	JS_FreeCString(jctx, msg);
}


/**
 * Log the pending exception of a context and clear it.
 */
static void qjsky_dump_exception(JSContext *jctx)
{
	JSValue exc;

	exc = JS_GetException(jctx);
	qjsky_log_error(jctx, "Uncaught error in JS", exc);
	JS_FreeValue(jctx, exc);
}


/**
 * Promise rejection tracker.
 *
 * Rejections without a handler are noted, and forgotten if a handler
 * is added, so those still unhandled at the end of the microtask
 * checkpoint can be reported as browsers do.
 */
static void
qjsky_promise_rejection(JSContext *jctx,
			JSValueConst promise,
			JSValueConst reason,
			bool is_handled,
			void *opaque)
{
	jsheap *heap = opaque;
	unsigned int i;

	if (is_handled) {
		for (i = 0; i < heap->rejection_count; i++) {
			if (JS_IsStrictEqual(jctx, heap->rejections[i].promise,
					     promise)) {
				JS_FreeValueRT(heap->rt, heap->rejections[i].promise);
				JS_FreeValueRT(heap->rt, heap->rejections[i].reason);
				heap->rejections[i] =
					heap->rejections[--heap->rejection_count];
				break;
			}
		}
		return;
	}

	if (heap->rejection_count == heap->rejection_alloc) {
		unsigned int alloc = heap->rejection_alloc == 0 ?
			8 : heap->rejection_alloc * 2;
		struct qjsky_rejection *r;

		r = realloc(heap->rejections, alloc * sizeof *r);
		if (r == NULL) {
			/* report it now rather than not at all */
			qjsky_log_error(jctx, "Uncaught (in promise)", reason);
			return;
		}
		heap->rejections = r;
		heap->rejection_alloc = alloc;
	}
	heap->rejections[heap->rejection_count].jctx = jctx;
	heap->rejections[heap->rejection_count].promise =
		JS_DupValueRT(heap->rt, promise);
	heap->rejections[heap->rejection_count].reason =
		JS_DupValueRT(heap->rt, reason);
	heap->rejection_count++;
}


/**
 * Report and forget the unhandled promise rejections of a heap.
 *
 * \param heap The heap
 * \param jctx Only forget, without reporting, rejections of this
 *             context, or NULL to report all
 */
static void qjsky_flush_rejections(jsheap *heap, JSContext *jctx)
{
	unsigned int i = 0;

	while (i < heap->rejection_count) {
		struct qjsky_rejection r = heap->rejections[i];

		if (jctx != NULL && r.jctx != jctx) {
			i++;
			continue;
		}
		heap->rejections[i] = heap->rejections[--heap->rejection_count];
		if (jctx == NULL) {
			qjsky_log_error(r.jctx, "Uncaught (in promise)", r.reason);
		}
		JS_FreeValueRT(heap->rt, r.promise);
		JS_FreeValueRT(heap->rt, r.reason);
	}
}


/**
 * Run all pending jobs (promise reactions) in a heap.
 *
 * This is the microtask checkpoint which must be performed when
 * javascript execution returns to the browser.
 */
static void qjsky_run_jobs(jsheap *heap)
{
	JSContext *job_ctx;
	int res;

	heap->depth++;
	while ((res = JS_ExecutePendingJob(heap->rt, &job_ctx)) != 0) {
		if (res < 0) {
			qjsky_dump_exception(job_ctx);
		}
	}
	heap->depth--;

	qjsky_flush_rejections(heap, NULL);
}


/**
 * Note that the browser is calling into javascript.
 *
 * \param heap The heap about to execute code
 * \param reset_timeout Whether to restart the execution timeout
 */
static void qjsky_enter_js(jsheap *heap, bool reset_timeout)
{
	if (reset_timeout || (heap->depth == 0)) {
		(void) nsu_getmonotonic_ms(&heap->exec_start_time);
	}
	if (heap->depth == 0) {
		/* the C stack position differs between entry points */
		JS_UpdateStackTop(heap->rt);
	}
	heap->depth++;
}


/**
 * Note that a call into javascript has returned.
 *
 * When control returns to the browser the microtask checkpoint is run.
 */
static void qjsky_leave_js(jsheap *heap)
{
	assert(heap->depth > 0);

	heap->depth--;
	if (heap->depth == 0) {
		qjsky_run_jobs(heap);
		heap->exec_start_time = 0;
	}
}


static void dukky_dump_error(duk_context *ctx)
{
	/* stack is ..., errobj */
	duk_dup(ctx, -1);
	/* ..., errobj, errobj */
	NSLOG(jserrors, WARNING, "Uncaught error in JS: %s", duk_safe_to_stacktrace(ctx, -1));
	/* ..., errobj, errobj.stackstring */
	duk_pop(ctx);
	/* ..., errobj */
}


/* exported interface documented in qjsky.h */
duk_int_t dukky_pcall(duk_context *ctx, duk_size_t argc, bool reset_timeout)
{
	jsheap *heap = qjsky_thread(ctx)->heap;
	duk_int_t ret;

	qjsky_enter_js(heap, reset_timeout);
	ret = duk_pcall(ctx, argc);
	qjsky_leave_js(heap);

	if (ret) {
		/* Something went wrong calling this... */
		dukky_dump_error(ctx);
	}

	return ret;
}


/**
 * Call a method from the stack, tracking the call into javascript.
 *
 * \see duk_pcall_method()
 */
static duk_int_t qjsky_pcall_method(duk_context *ctx, duk_idx_t nargs)
{
	jsheap *heap = qjsky_thread(ctx)->heap;
	duk_int_t ret;

	qjsky_enter_js(heap, true);
	ret = duk_pcall_method(ctx, nargs);
	qjsky_leave_js(heap);

	return ret;
}


/* exported interface documented in qjsky.h */
void dukky_push_generics(duk_context *ctx, const char *generic)
{
	/* ... */
	duk_get_global_string(ctx, GENERICS_MAGIC);
	/* ..., generics */
	duk_get_prop_string(ctx, -1, generic);
	/* ..., generics, generic */
	duk_remove(ctx, -2);
	/* ..., generic */
}


/* exported interface documented in qjsky.h */
void dukky_log_stack_frame(duk_context *ctx, const char * reason)
{
	duk_push_context_dump(ctx);
	NSLOG(qjsky, DEEPDEBUG, "%s, stack is: %s", reason, duk_safe_to_string(ctx, -1));
	duk_pop(ctx);
}


/**
 * Evaluate javascript source.
 *
 * \param jctx The context to evaluate in
 * \param txt The source, which need not be terminated
 * \param txtlen The length of the source
 * \param prefix Text to prepend to the source
 * \param suffix Text to append to the source
 * \param name The source name for error messages
 * \return The result value or JS_EXCEPTION
 */
static JSValue
qjsky_eval(JSContext *jctx,
	   const uint8_t *txt,
	   size_t txtlen,
	   const char *prefix,
	   const char *suffix,
	   const char *name)
{
	size_t prelen = strlen(prefix);
	size_t suflen = strlen(suffix);
	JSValue res;
	char *src;

	/* JS_Eval requires the source to be NUL terminated, which the
	 * callers do not promise, so take a terminated copy.
	 */
	src = malloc(prelen + txtlen + suflen + 1);
	if (src == NULL) {
		return JS_ThrowOutOfMemory(jctx);
	}
	memcpy(src, prefix, prelen);
	memcpy(src + prelen, txt, txtlen);
	memcpy(src + prelen + txtlen, suffix, suflen + 1);

	res = JS_Eval(jctx, src, prelen + txtlen + suflen, name,
		      JS_EVAL_TYPE_GLOBAL);
	free(src);

	return res;
}


/**
 * Get the HTML content of a thread.
 *
 * \return The content or NULL if the thread has no Window
 */
static html_content *qjsky_html_content(jsthread *thread)
{
	window_private_t *window = thread->window_priv;

	return (window == NULL) ? NULL : window->htmlc;
}


/**
 * Get the DOM node of a javascript Node object.
 *
 * \return The node or NULL if obj is not a Node
 */
static dom_node *qjsky_node_of(jsthread *thread, JSValueConst obj)
{
	void *priv = NULL;
	int idx;

	idx = qjsky_resolve(thread, obj, &priv);
	if (!qjsky_class_is_a(idx, QJSKY_PROTO_NODE) || (priv == NULL)) {
		return NULL;
	}
	return ((node_private_t *)priv)->node;
}


/**
 * Native layout query: the geometry of an element.
 *
 * Returns [x, y, width, height, clientWidth, clientHeight, scrollWidth,
 * scrollHeight] in document coordinates or null if not displayed.
 */
static JSValue
qjsky_native_geometry(JSContext *jctx,
		      JSValueConst this_val,
		      int argc,
		      JSValueConst *argv)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	html_content *htmlc = qjsky_html_content(thread);
	struct html_layout_geometry g;
	dom_node *node;
	JSValue res;

	node = (argc > 0) ? qjsky_node_of(thread, argv[0]) : NULL;
	if ((htmlc == NULL) || (node == NULL) ||
	    !html_layout_node_geometry(htmlc, node, &g)) {
		return JS_NULL;
	}

	res = JS_NewArray(jctx);
	JS_SetPropertyUint32(jctx, res, 0, JS_NewInt32(jctx, g.x));
	JS_SetPropertyUint32(jctx, res, 1, JS_NewInt32(jctx, g.y));
	JS_SetPropertyUint32(jctx, res, 2, JS_NewInt32(jctx, g.width));
	JS_SetPropertyUint32(jctx, res, 3, JS_NewInt32(jctx, g.height));
	JS_SetPropertyUint32(jctx, res, 4, JS_NewInt32(jctx, g.client_width));
	JS_SetPropertyUint32(jctx, res, 5, JS_NewInt32(jctx, g.client_height));
	JS_SetPropertyUint32(jctx, res, 6, JS_NewInt32(jctx, g.scroll_width));
	JS_SetPropertyUint32(jctx, res, 7, JS_NewInt32(jctx, g.scroll_height));

	return res;
}


/**
 * Native layout query: the viewport.
 *
 * Returns [width, height, scrollX, scrollY].
 */
static JSValue
qjsky_native_viewport(JSContext *jctx,
		      JSValueConst this_val,
		      int argc,
		      JSValueConst *argv)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	html_content *htmlc = qjsky_html_content(thread);
	int width = 0;
	int height = 0;
	int scroll_x = 0;
	int scroll_y = 0;
	JSValue res;

	if (htmlc != NULL) {
		html_layout_viewport(htmlc, &width, &height,
				     &scroll_x, &scroll_y);
	}

	res = JS_NewArray(jctx);
	JS_SetPropertyUint32(jctx, res, 0, JS_NewInt32(jctx, width));
	JS_SetPropertyUint32(jctx, res, 1, JS_NewInt32(jctx, height));
	JS_SetPropertyUint32(jctx, res, 2, JS_NewInt32(jctx, scroll_x));
	JS_SetPropertyUint32(jctx, res, 3, JS_NewInt32(jctx, scroll_y));

	return res;
}


/**
 * Native layout query: a computed style value of an element.
 *
 * Returns the value as a string or undefined if it is not known.
 */
static JSValue
qjsky_native_style(JSContext *jctx,
		   JSValueConst this_val,
		   int argc,
		   JSValueConst *argv)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	html_content *htmlc = qjsky_html_content(thread);
	const char *property;
	dom_node *node;
	char *value;
	JSValue res;

	if ((htmlc == NULL) || (argc < 2)) {
		return JS_UNDEFINED;
	}
	node = qjsky_node_of(thread, argv[0]);
	if (node == NULL) {
		return JS_UNDEFINED;
	}

	property = JS_ToCString(jctx, argv[1]);
	if (property == NULL) {
		return JS_EXCEPTION;
	}
	value = html_layout_computed_style(htmlc, node, property);
	JS_FreeCString(jctx, property);

	if (value == NULL) {
		return JS_UNDEFINED;
	}
	res = JS_NewString(jctx, value);
	free(value);

	return res;
}


/**
 * Native: scroll the window to a position in the document.
 */
static JSValue
qjsky_native_scroll_to(JSContext *jctx,
		       JSValueConst this_val,
		       int argc,
		       JSValueConst *argv)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	html_content *htmlc = qjsky_html_content(thread);
	int32_t x = 0;
	int32_t y = 0;

	if ((htmlc == NULL) || (htmlc->bw == NULL) || (argc < 2)) {
		return JS_UNDEFINED;
	}
	if ((JS_ToInt32(jctx, &x, argv[0]) < 0) ||
	    (JS_ToInt32(jctx, &y, argv[1]) < 0)) {
		return JS_EXCEPTION;
	}

	browser_window_scroll_to(htmlc->bw, max(x, 0), max(y, 0));

	return JS_UNDEFINED;
}


static void qjsky_enter_thread(jsthread *thread);
static void qjsky_leave_thread(jsthread *thread);


/**
 * Remove a request from its thread and free it.
 */
static void qjsky_fetch_free(struct qjsky_fetch *qf)
{
	jsthread *thread = qf->thread;

	if (qf->prev != NULL) {
		qf->prev->next = qf->next;
	} else {
		thread->fetches = qf->next;
	}
	if (qf->next != NULL) {
		qf->next->prev = qf->prev;
	}
	JS_FreeValue(thread->jctx, qf->callback);
	free(qf);
}


/**
 * Abort every request a thread has in progress.
 */
static void qjsky_fetch_abort_all(jsthread *thread)
{
	while (thread->fetches != NULL) {
		script_fetch_abort(thread->fetches->sf);
		qjsky_fetch_free(thread->fetches);
	}
}


/**
 * Pass the response to a request to the script which made it.
 */
static void
qjsky_fetch_done(const struct script_fetch_response *response, void *pw)
{
	struct qjsky_fetch *qf = pw;
	jsthread *thread = qf->thread;
	JSContext *jctx = thread->jctx;
	JSValue callback = JS_DupValue(jctx, qf->callback);
	JSValue res;
	JSValue ret;

	/* the request is freed after this callback */
	qf->sf = NULL;
	qjsky_fetch_free(qf);

	res = JS_NewObject(jctx);
	if (response->error != NULL) {
		JS_SetPropertyStr(jctx, res, "error",
				  JS_NewString(jctx, response->error));
	} else {
		JS_SetPropertyStr(jctx, res, "status",
				  JS_NewInt32(jctx, response->status));
		JS_SetPropertyStr(jctx, res, "statusText",
				  JS_NewString(jctx, response->status_text));
		JS_SetPropertyStr(jctx, res, "url",
				  JS_NewString(jctx, nsurl_access(response->url)));
		JS_SetPropertyStr(jctx, res, "redirected",
				  JS_NewBool(jctx, response->redirected));
		JS_SetPropertyStr(jctx, res, "headers",
				  JS_NewString(jctx, response->headers));
		JS_SetPropertyStr(jctx, res, "body",
				  JS_NewArrayBufferCopy(jctx, response->body,
							response->body_len));
	}

	qjsky_enter_thread(thread);
	qjsky_enter_js(thread->heap, true);
	ret = JS_Call(jctx, callback, JS_UNDEFINED, 1, (JSValueConst *)&res);
	if (JS_IsException(ret)) {
		qjsky_dump_exception(jctx);
	}
	JS_FreeValue(jctx, ret);
	JS_FreeValue(jctx, res);
	JS_FreeValue(jctx, callback);
	qjsky_leave_js(thread->heap);
	qjsky_leave_thread(thread);
}


/**
 * Native network request.
 *
 * __netsurf_fetch(method, url, headers, body, credentials, redirect,
 * callback) starts a request and returns its identifier.  headers is an
 * array of alternating names and values, body is undefined, a string or
 * an ArrayBuffer, credentials is "omit", "same-origin" or "include" and
 * redirect is "follow" or "error".  callback is called with an object
 * holding the response, or an error property for a network error.
 */
static JSValue
qjsky_native_fetch(JSContext *jctx,
		   JSValueConst this_val,
		   int argc,
		   JSValueConst *argv)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	html_content *htmlc = qjsky_html_content(thread);
	struct script_fetch_request request;
	struct qjsky_fetch *qf = NULL;
	const char *method = NULL;
	const char *url_s = NULL;
	const char *credentials = NULL;
	const char *redirect = NULL;
	const char *body_s = NULL;
	const char **headers = NULL;
	uint32_t nheaders = 0;
	uint32_t i;
	JSValue ret = JS_EXCEPTION;
	nserror err;

	memset(&request, 0, sizeof request);

	if (argc < 7 || !JS_IsFunction(jctx, argv[6])) {
		return JS_ThrowTypeError(jctx, "bad arguments");
	}
	if (htmlc == NULL || htmlc->base_url == NULL) {
		return JS_ThrowTypeError(jctx, "no document");
	}

	method = JS_ToCString(jctx, argv[0]);
	url_s = JS_ToCString(jctx, argv[1]);
	credentials = JS_ToCString(jctx, argv[4]);
	redirect = JS_ToCString(jctx, argv[5]);
	if (method == NULL || url_s == NULL ||
	    credentials == NULL || redirect == NULL) {
		goto out;
	}

	if (JS_IsArray(argv[2])) {
		int64_t len;

		if (JS_GetLength(jctx, argv[2], &len) < 0) {
			goto out;
		}
		nheaders = (uint32_t)min(len, 256);
		headers = calloc(nheaders + 1, sizeof(char *));
		if (headers == NULL) {
			JS_ThrowOutOfMemory(jctx);
			goto out;
		}
		for (i = 0; i < nheaders; i++) {
			JSValue v = JS_GetPropertyUint32(jctx, argv[2], i);

			headers[i] = JS_ToCString(jctx, v);
			JS_FreeValue(jctx, v);
			if (headers[i] == NULL) {
				goto out;
			}
		}
	}

	if (JS_IsString(argv[3])) {
		size_t len;

		body_s = JS_ToCStringLen(jctx, &len, argv[3]);
		if (body_s == NULL) {
			goto out;
		}
		request.body = (const uint8_t *)body_s;
		request.body_len = len;
	} else if (!JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
		size_t len;
		uint8_t *buf = JS_GetArrayBuffer(jctx, &len, argv[3]);

		if (buf == NULL) {
			goto out;
		}
		/* never NULL for a body, even an empty one */
		request.body = len == 0 ? (const uint8_t *)"" : buf;
		request.body_len = len;
	}

	err = nsurl_join(htmlc->base_url, url_s, &request.url);
	if (err != NSERROR_OK) {
		JS_ThrowTypeError(jctx, "invalid URL");
		goto out;
	}

	request.document = llcache_handle_get_url(htmlc->base.llcache);
	request.method = method;
	request.headers = headers;
	request.redirect_error = strcmp(redirect, "error") == 0;
	if (strcmp(credentials, "omit") == 0) {
		request.credentials = SCRIPT_FETCH_CREDENTIALS_OMIT;
	} else if (strcmp(credentials, "include") == 0) {
		request.credentials = SCRIPT_FETCH_CREDENTIALS_INCLUDE;
	} else {
		request.credentials = SCRIPT_FETCH_CREDENTIALS_SAME_ORIGIN;
	}

	qf = calloc(1, sizeof *qf);
	if (qf == NULL) {
		JS_ThrowOutOfMemory(jctx);
		goto out;
	}

	err = script_fetch_start(&request, qjsky_fetch_done, qf, &qf->sf);
	if (err != NSERROR_OK) {
		free(qf);
		JS_ThrowTypeError(jctx, "request failed");
		goto out;
	}

	qf->thread = thread;
	qf->callback = JS_DupValue(jctx, argv[6]);
	qf->id = ++thread->next_fetch_id;
	qf->next = thread->fetches;
	if (qf->next != NULL) {
		qf->next->prev = qf;
	}
	thread->fetches = qf;

	ret = JS_NewInt32(jctx, qf->id);

out:
	if (request.url != NULL) {
		nsurl_unref(request.url);
	}
	if (headers != NULL) {
		for (i = 0; i < nheaders; i++) {
			JS_FreeCString(jctx, headers[i]);
		}
		free(headers);
	}
	JS_FreeCString(jctx, body_s);
	JS_FreeCString(jctx, method);
	JS_FreeCString(jctx, url_s);
	JS_FreeCString(jctx, credentials);
	JS_FreeCString(jctx, redirect);
	return ret;
}


/**
 * Native abort of a network request: __netsurf_fetch_abort(id).
 */
static JSValue
qjsky_native_fetch_abort(JSContext *jctx,
			 JSValueConst this_val,
			 int argc,
			 JSValueConst *argv)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	struct qjsky_fetch *qf;
	int32_t id;

	if (argc < 1 || JS_ToInt32(jctx, &id, argv[0]) < 0) {
		return JS_EXCEPTION;
	}

	for (qf = thread->fetches; qf != NULL; qf = qf->next) {
		if (qf->id == id) {
			script_fetch_abort(qf->sf);
			qjsky_fetch_free(qf);
			break;
		}
	}

	return JS_UNDEFINED;
}


/**
 * Native text decoding: __netsurf_decode(buffer, charset).
 *
 * Decodes an ArrayBuffer in the named character set, or UTF-8 if the
 * character set is unknown.  Invalid sequences are replaced.
 */
static JSValue
qjsky_native_decode(JSContext *jctx,
		    JSValueConst this_val,
		    int argc,
		    JSValueConst *argv)
{
	const char *charset = NULL;
	uint8_t *buf;
	size_t len;
	JSValue ret;

	if (argc < 1) {
		return JS_ThrowTypeError(jctx, "bad arguments");
	}
	buf = JS_GetArrayBuffer(jctx, &len, argv[0]);
	if (buf == NULL) {
		return JS_EXCEPTION;
	}
	if (argc > 1 && JS_IsString(argv[1])) {
		charset = JS_ToCString(jctx, argv[1]);
	}

	if (charset != NULL && charset[0] != '\0' &&
	    strcasecmp(charset, "utf-8") != 0 &&
	    strcasecmp(charset, "utf8") != 0) {
		char *utf8;
		size_t utf8_len;

		if (len > 0 && utf8_from_enc((const char *)buf, charset, len,
					     &utf8, &utf8_len) == NSERROR_OK) {
			ret = JS_NewStringLen(jctx, utf8, utf8_len);
			free(utf8);
			JS_FreeCString(jctx, charset);
			return ret;
		}
	}
	JS_FreeCString(jctx, charset);

	/* skip a byte order mark */
	if (len >= 3 && buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
		buf += 3;
		len -= 3;
	}
	return JS_NewStringLen(jctx, (const char *)buf, len);
}


/**
 * Native functions for webcompat.js, which removes them from the
 * global object once it has them.
 */
static const JSCFunctionListEntry qjsky_native_funcs[] = {
	JS_CFUNC_DEF("__netsurf_geometry", 1, qjsky_native_geometry),
	JS_CFUNC_DEF("__netsurf_viewport", 0, qjsky_native_viewport),
	JS_CFUNC_DEF("__netsurf_style", 2, qjsky_native_style),
	JS_CFUNC_DEF("__netsurf_scroll_to", 2, qjsky_native_scroll_to),
	JS_CFUNC_DEF("__netsurf_fetch", 7, qjsky_native_fetch),
	JS_CFUNC_DEF("__netsurf_fetch_abort", 1, qjsky_native_fetch_abort),
	JS_CFUNC_DEF("__netsurf_decode", 2, qjsky_native_decode),
};


/*
 * Module scripts
 *
 * QuickJS resolves the modules a module imports synchronously, while
 * NetSurf fetches them asynchronously.  So a module is only resolved
 * once every module it imports, directly or not, has been fetched and
 * compiled.  Compiled modules are kept by QuickJS in the context and
 * found by name, which is the URL each was requested with; this side
 * only remembers which URLs have been fetched, never pointers to the
 * compiled modules, as QuickJS frees unresolved modules when any
 * module fails to resolve.
 */

/**
 * A module known to a thread, by URL.
 */
struct qjsky_module {
	struct qjsky_module *next; /**< next module of the thread */
	jsthread *thread; /**< thread the module belongs to */
	char *url; /**< name of the module */
	enum js_module_status status; /**< pending while being fetched */
	struct hlcache_handle *handle; /**< fetch of the source, or NULL */
};

/**
 * A module script, or a call of import(), and the modules it imports.
 */
struct js_module {
	struct js_module *next; /**< next of the thread */
	struct js_module *prev; /**< previous of the thread */
	jsthread *thread; /**< thread the module is loaded in */
	char *url; /**< name of the module */
	bool waiting; /**< whether imported modules are being fetched */
	js_module_cb *cb; /**< callback for a module script */
	void *pw; /**< private data for cb */
	JSValue resolving_funcs[2]; /**< promise functions for import() */
};

static void qjsky_module_check_loads(jsthread *thread);


/**
 * Check if a module specifier starts with a URL scheme.
 */
static bool qjsky_module_has_scheme(const char *s)
{
	if (!ascii_is_alpha(*s)) {
		return false;
	}
	for (s++; *s != '\0'; s++) {
		if (*s == ':') {
			return true;
		}
		if (!ascii_is_alphanumerical(*s) &&
		    (*s != '+') && (*s != '-') && (*s != '.')) {
			return false;
		}
	}
	return false;
}


/**
 * Resolve a module specifier against the module importing it.
 *
 * Module normalisation callback for QuickJS.  Bare specifiers, which
 * need an import map, are not supported.
 *
 * \param jctx The context importing the module
 * \param base Name of the importing module or script
 * \param name The specifier
 * \param opaque The heap
 * \return The absolute URL of the module, allocated with js_malloc(), or
 *         NULL with an exception pending
 */
static char *
qjsky_module_normalize(JSContext *jctx,
		       const char *base,
		       const char *name,
		       void *opaque)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	html_content *htmlc = qjsky_html_content(thread);
	nsurl *base_url = NULL;
	nsurl *joined;
	char *ret;
	nserror err;

	if ((strncmp(name, "/", 1) != 0) &&
	    (strncmp(name, "./", 2) != 0) &&
	    (strncmp(name, "../", 3) != 0) &&
	    !qjsky_module_has_scheme(name)) {
		JS_ThrowTypeError(jctx, "module specifier \"%s\" is not a "
				  "relative or absolute URL", name);
		return NULL;
	}

	/* classic scripts are named by URL, unless they are inline */
	if ((base != NULL) && qjsky_module_has_scheme(base)) {
		if (nsurl_create(base, &base_url) != NSERROR_OK) {
			base_url = NULL;
		}
	}
	if ((base_url == NULL) && (htmlc != NULL) && (htmlc->base_url != NULL)) {
		base_url = nsurl_ref(htmlc->base_url);
	}
	if (base_url == NULL) {
		JS_ThrowTypeError(jctx, "no base URL for module \"%s\"", name);
		return NULL;
	}

	err = nsurl_join(base_url, name, &joined);
	nsurl_unref(base_url);
	if (err != NSERROR_OK) {
		JS_ThrowTypeError(jctx, "invalid module URL \"%s\"", name);
		return NULL;
	}

	ret = js_strdup(jctx, nsurl_access(joined));
	nsurl_unref(joined);

	return ret;
}


/**
 * Module loader callback for QuickJS.
 *
 * Modules are compiled before they are resolved, so QuickJS only asks
 * for modules which could not be loaded.
 */
static JSModuleDef *
qjsky_module_loader(JSContext *jctx, const char *name, void *opaque)
{
	JS_ThrowReferenceError(jctx, "module \"%s\" has not been loaded", name);
	return NULL;
}


/**
 * Find a module a thread knows by URL.
 */
static struct qjsky_module *
qjsky_module_find(jsthread *thread, const char *url)
{
	struct qjsky_module *mod;

	for (mod = thread->modules; mod != NULL; mod = mod->next) {
		if (strcmp(mod->url, url) == 0) {
			return mod;
		}
	}
	return NULL;
}


/**
 * Add a module to the modules a thread knows.
 */
static struct qjsky_module *
qjsky_module_new(jsthread *thread, const char *url)
{
	struct qjsky_module *mod;

	mod = calloc(1, sizeof(*mod));
	if (mod == NULL) {
		return NULL;
	}
	mod->url = strdup(url);
	if (mod->url == NULL) {
		free(mod);
		return NULL;
	}
	mod->thread = thread;
	mod->status = JS_MODULE_PENDING;
	mod->next = thread->modules;
	thread->modules = mod;

	return mod;
}


/**
 * Compile a module, which QuickJS keeps by name.
 *
 * \param thread The thread to compile the module in
 * \param name The name of the module
 * \param txt The source
 * \param txtlen The length of the source
 * \param meta_url The URL for import.meta.url
 * \return JS_MODULE_READY, or JS_MODULE_FAILED if it does not compile
 */
static enum js_module_status
qjsky_module_compile(jsthread *thread,
		     const char *name,
		     const uint8_t *txt,
		     size_t txtlen,
		     const char *meta_url)
{
	JSContext *jctx = thread->jctx;
	JSModuleDef *m;
	JSValue val;
	JSValue meta;
	char *src;

	/* JS_Eval requires the source to be NUL terminated */
	src = malloc(txtlen + 1);
	if (src == NULL) {
		return JS_MODULE_FAILED;
	}
	memcpy(src, txt, txtlen);
	src[txtlen] = '\0';

	qjsky_enter_js(thread->heap, true);
	val = JS_Eval(jctx, src, txtlen, name,
		      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY |
		      JS_EVAL_FLAG_NO_RESOLVE);
	free(src);
	if (JS_IsException(val)) {
		qjsky_dump_exception(jctx);
		qjsky_leave_js(thread->heap);
		return JS_MODULE_FAILED;
	}

	m = JS_VALUE_GET_PTR(val);
	meta = JS_GetImportMeta(jctx, m);
	if (!JS_IsException(meta)) {
		JS_DefinePropertyValueStr(jctx, meta, "url",
					  JS_NewString(jctx, meta_url),
					  JS_PROP_C_W_E);
	}
	JS_FreeValue(jctx, meta);

	/* QuickJS keeps the module, so the reference can go */
	JS_FreeValue(jctx, val);
	qjsky_leave_js(thread->heap);

	return JS_MODULE_READY;
}


/**
 * Callback for the fetch of an imported module.
 */
static nserror
qjsky_module_fetch_cb(hlcache_handle *handle,
		      const hlcache_event *event,
		      void *pw)
{
	struct qjsky_module *mod = pw;
	jsthread *thread = mod->thread;

	switch (event->type) {
	case CONTENT_MSG_DONE:
		if (content_get_type(handle) != CONTENT_JS) {
			NSLOG(qjsky, INFO, "module %s is not javascript",
			      mod->url);
			mod->status = JS_MODULE_FAILED;
		} else {
			const uint8_t *data;
			size_t size;

			data = content_get_source_data(handle, &size);
			mod->status = qjsky_module_compile(thread, mod->url,
					data, size,
					nsurl_access(hlcache_handle_get_url(handle)));
		}
		break;

	case CONTENT_MSG_ERROR:
		NSLOG(qjsky, INFO, "module %s failed: %s", mod->url,
		      event->data.errordata.errormsg);
		mod->status = JS_MODULE_FAILED;
		break;

	default:
		return NSERROR_OK;
	}

	hlcache_handle_release(handle);
	mod->handle = NULL;

	qjsky_enter_thread(thread);
	qjsky_module_check_loads(thread);
	qjsky_leave_thread(thread);

	return NSERROR_OK;
}


/**
 * Start fetching a module a thread does not know yet.
 */
static struct qjsky_module *
qjsky_module_fetch(jsthread *thread, const char *url)
{
	html_content *htmlc = qjsky_html_content(thread);
	struct qjsky_module *mod;
	hlcache_child_context child;
	nsurl *nsurl;
	nserror err;

	mod = qjsky_module_new(thread, url);
	if (mod == NULL) {
		return NULL;
	}

	if ((htmlc == NULL) ||
	    (nsurl_create(url, &nsurl) != NSERROR_OK)) {
		mod->status = JS_MODULE_FAILED;
		return mod;
	}

	child.charset = htmlc->encoding;
	child.quirks = htmlc->base.quirks;

	NSLOG(qjsky, INFO, "fetching module %s", url);

	err = hlcache_handle_retrieve(nsurl, 0, content_get_url(&htmlc->base),
				      NULL, qjsky_module_fetch_cb, mod,
				      &child, CONTENT_SCRIPT, &mod->handle);
	nsurl_unref(nsurl);
	if (err != NSERROR_OK) {
		mod->handle = NULL;
		mod->status = JS_MODULE_FAILED;
	}

	return mod;
}


/**
 * Get the status of a module and every module it imports.
 *
 * Modules which have not been requested yet are fetched.
 *
 * \param thread The thread to load the modules in
 * \param url Name of the module
 * \return JS_MODULE_READY if every module has been compiled,
 *         JS_MODULE_PENDING if some are being fetched or
 *         JS_MODULE_FAILED if any failed
 */
static enum js_module_status
qjsky_module_graph(jsthread *thread, const char *url)
{
	JSContext *jctx = thread->jctx;
	enum js_module_status status = JS_MODULE_READY;
	char **urls;
	size_t count = 0;
	size_t alloc = 16;
	size_t next;

	/* every module of the graph, in the order found */
	urls = malloc(alloc * sizeof(*urls));
	if (urls == NULL) {
		return JS_MODULE_FAILED;
	}
	urls[count] = strdup(url);
	if (urls[count] == NULL) {
		free(urls);
		return JS_MODULE_FAILED;
	}
	count++;

	for (next = 0; (next < count) && (status != JS_MODULE_FAILED); next++) {
		const char *u = urls[next];
		struct qjsky_module *mod;
		JSModuleDef *m;
		int i;
		int n;

		mod = qjsky_module_find(thread, u);
		if (mod == NULL) {
			mod = qjsky_module_fetch(thread, u);
			if (mod == NULL) {
				status = JS_MODULE_FAILED;
				break;
			}
		}
		if (mod->status == JS_MODULE_FAILED) {
			status = JS_MODULE_FAILED;
			break;
		}
		if (mod->status == JS_MODULE_PENDING) {
			status = JS_MODULE_PENDING;
			continue;
		}

		m = JS_FindLoadedModule(jctx, u);
		if (m == NULL) {
			NSLOG(qjsky, INFO, "module %s has gone", u);
			mod->status = JS_MODULE_FAILED;
			status = JS_MODULE_FAILED;
			break;
		}

		n = JS_GetModuleRequestCount(m);
		for (i = 0; i < n; i++) {
			JSAtom atom = JS_GetModuleRequest(jctx, m, i);
			const char *spec = JS_AtomToCString(jctx, atom);
			char *resolved = NULL;
			size_t j;

			JS_FreeAtom(jctx, atom);
			if (spec != NULL) {
				resolved = qjsky_module_normalize(jctx, u,
								  spec, NULL);
				JS_FreeCString(jctx, spec);
			}
			if (resolved == NULL) {
				qjsky_dump_exception(jctx);
				status = JS_MODULE_FAILED;
				break;
			}

			for (j = 0; j < count; j++) {
				if (strcmp(urls[j], resolved) == 0) {
					break;
				}
			}
			if (j < count) {
				/* already in the graph */
				js_free(jctx, resolved);
				continue;
			}

			if (count == alloc) {
				char **n_urls = realloc(urls,
						alloc * 2 * sizeof(*urls));
				if (n_urls == NULL) {
					js_free(jctx, resolved);
					status = JS_MODULE_FAILED;
					break;
				}
				urls = n_urls;
				alloc *= 2;
			}
			urls[count] = strdup(resolved);
			js_free(jctx, resolved);
			if (urls[count] == NULL) {
				status = JS_MODULE_FAILED;
				break;
			}
			count++;
		}
	}

	for (next = 0; next < count; next++) {
		free(urls[next]);
	}
	free(urls);

	return status;
}


/**
 * Free a module script or import(), removing it from its thread.
 */
static void qjsky_module_free(struct js_module *load)
{
	jsthread *thread = load->thread;

	if (load->prev != NULL) {
		load->prev->next = load->next;
	} else {
		thread->module_loads = load->next;
	}
	if (load->next != NULL) {
		load->next->prev = load->prev;
	}

	JS_FreeValue(thread->jctx, load->resolving_funcs[0]);
	JS_FreeValue(thread->jctx, load->resolving_funcs[1]);
	free(load->url);
	free(load);
}


/**
 * Create a module script or import() of a module.
 */
static struct js_module *qjsky_module_new_load(jsthread *thread, const char *url)
{
	struct js_module *load;

	load = calloc(1, sizeof(*load));
	if (load == NULL) {
		return NULL;
	}
	load->url = strdup(url);
	if (load->url == NULL) {
		free(load);
		return NULL;
	}
	load->thread = thread;
	load->resolving_funcs[0] = JS_UNDEFINED;
	load->resolving_funcs[1] = JS_UNDEFINED;

	load->next = thread->module_loads;
	if (load->next != NULL) {
		load->next->prev = load;
	}
	thread->module_loads = load;

	return load;
}


/**
 * Job which settles the promise of import() with the module.
 *
 * argv holds the promise resolve and reject functions and the module
 * URL.  Every module the import needs has been compiled, so loading it
 * evaluates it.
 */
static JSValue
qjsky_module_import_job(JSContext *jctx, int argc, JSValueConst *argv)
{
	const char *url = JS_ToCString(jctx, argv[2]);
	JSValue promise;
	JSValue ret;

	if (url == NULL) {
		promise = JS_EXCEPTION;
	} else {
		promise = JS_LoadModule(jctx, url, url);
		JS_FreeCString(jctx, url);
	}

	if (JS_IsException(promise)) {
		JSValue err = JS_GetException(jctx);
		ret = JS_Call(jctx, argv[1], JS_UNDEFINED, 1,
			      (JSValueConst *)&err);
		JS_FreeValue(jctx, err);
	} else {
		ret = JS_Call(jctx, argv[0], JS_UNDEFINED, 1,
			      (JSValueConst *)&promise);
		JS_FreeValue(jctx, promise);
	}
	JS_FreeValue(jctx, ret);

	return JS_UNDEFINED;
}


/**
 * Settle the promise of an import() whose modules have loaded or failed.
 */
static void
qjsky_module_import_settle(struct js_module *load, enum js_module_status status)
{
	JSContext *jctx = load->thread->jctx;

	if (status == JS_MODULE_READY) {
		JSValue args[3];

		args[0] = load->resolving_funcs[0];
		args[1] = load->resolving_funcs[1];
		args[2] = JS_NewString(jctx, load->url);
		JS_EnqueueJob(jctx, qjsky_module_import_job, 3,
			      (JSValueConst *)args);
		JS_FreeValue(jctx, args[2]);
	} else {
		JSValue err;
		JSValue ret;

		err = JS_NewError(jctx);
		JS_DefinePropertyValueStr(jctx, err, "name",
					  JS_NewString(jctx, "TypeError"),
					  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
		JS_DefinePropertyValueStr(jctx, err, "message",
				JS_NewString(jctx, "Failed to fetch dynamically "
					     "imported module"),
				JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
		ret = JS_Call(jctx, load->resolving_funcs[1], JS_UNDEFINED,
			      1, (JSValueConst *)&err);
		JS_FreeValue(jctx, ret);
		JS_FreeValue(jctx, err);
	}
}


/**
 * Complete module scripts and imports whose modules have been fetched.
 */
static void qjsky_module_check_loads(jsthread *thread)
{
	struct js_module *load;
	bool again = true;

	/* callbacks can change the list, so start again after each */
	while (again) {
		again = false;
		for (load = thread->module_loads; load != NULL; load = load->next) {
			enum js_module_status status;

			if (load->waiting == false) {
				continue;
			}

			status = qjsky_module_graph(thread, load->url);
			if (status == JS_MODULE_PENDING) {
				continue;
			}

			load->waiting = false;
			if (load->cb == NULL) {
				/* import() */
				qjsky_enter_js(thread->heap, true);
				qjsky_module_import_settle(load, status);
				qjsky_module_free(load);
				qjsky_leave_js(thread->heap);
			} else {
				load->cb(load->pw, load, status);
				if (status != JS_MODULE_READY) {
					qjsky_module_free(load);
				}
			}
			again = true;
			break;
		}
	}
}


/**
 * Dynamic import hook for QuickJS: import() of a module.
 *
 * \return 0 when the promise will be settled, or -1 with an exception
 *         pending to reject it now
 */
static int
qjsky_module_dynamic_import(JSContext *jctx,
			    JSValueConst basename,
			    JSValueConst specifier,
			    JSValueConst attributes,
			    JSValueConst *resolving_funcs,
			    void *opaque)
{
	jsthread *thread = qjsky_thread(qjsduk_from_js(jctx));
	const char *base = NULL;
	const char *spec;
	char *url;
	struct js_module *load;
	enum js_module_status status;

	if (JS_IsObject(attributes)) {
		JSValue type = JS_GetPropertyStr(jctx, attributes, "type");
		bool typed = !JS_IsUndefined(type);

		JS_FreeValue(jctx, type);
		if (typed) {
			JS_ThrowTypeError(jctx, "module type not supported");
			return -1;
		}
	}

	spec = JS_ToCString(jctx, specifier);
	if (spec == NULL) {
		return -1;
	}
	if (JS_IsString(basename)) {
		base = JS_ToCString(jctx, basename);
	}
	url = qjsky_module_normalize(jctx, base, spec, NULL);
	JS_FreeCString(jctx, base);
	JS_FreeCString(jctx, spec);
	if (url == NULL) {
		return -1;
	}

	load = qjsky_module_new_load(thread, url);
	js_free(jctx, url);
	if (load == NULL) {
		JS_ThrowOutOfMemory(jctx);
		return -1;
	}
	load->resolving_funcs[0] = JS_DupValue(jctx, resolving_funcs[0]);
	load->resolving_funcs[1] = JS_DupValue(jctx, resolving_funcs[1]);

	status = qjsky_module_graph(thread, load->url);
	if (status == JS_MODULE_PENDING) {
		load->waiting = true;
	} else {
		qjsky_module_import_settle(load, status);
		qjsky_module_free(load);
	}

	return 0;
}


/**
 * Forget the modules of a thread, before its context is freed.
 */
static void qjsky_module_free_all(jsthread *thread)
{
	while (thread->module_loads != NULL) {
		qjsky_module_free(thread->module_loads);
	}

	while (thread->modules != NULL) {
		struct qjsky_module *mod = thread->modules;

		thread->modules = mod->next;
		if (mod->handle != NULL) {
			hlcache_handle_abort(mod->handle);
			hlcache_handle_release(mod->handle);
		}
		free(mod->url);
		free(mod);
	}
}


/* exported interface documented in js.h */
bool js_supports_modules(void)
{
	return true;
}


/* exported interface documented in js.h */
enum js_module_status
js_module_load(jsthread *thread,
	       const uint8_t *txt,
	       size_t txtlen,
	       const char *url,
	       bool is_inline,
	       js_module_cb *cb,
	       void *pw,
	       struct js_module **module_out)
{
	struct qjsky_module *mod;
	struct js_module *load;
	enum js_module_status status;
	char *name = NULL;

	*module_out = NULL;

	if (thread->pending_destroy) {
		return JS_MODULE_FAILED;
	}

	if (is_inline) {
		/* each module in a document needs its own name, so the
		 * fragment of the document's URL is replaced
		 */
		size_t len = strcspn(url, "#");

		name = malloc(len + 32);
		if (name == NULL) {
			return JS_MODULE_FAILED;
		}
		snprintf(name, len + 32, "%.*s#netsurf-module-%u",
			 (int)len, url, ++thread->inline_modules);
	} else {
		name = strdup(url);
		if (name == NULL) {
			return JS_MODULE_FAILED;
		}
	}

	qjsky_enter_thread(thread);

	mod = qjsky_module_find(thread, name);
	if (mod == NULL) {
		/* not imported by another module yet */
		mod = qjsky_module_new(thread, name);
		if (mod != NULL) {
			mod->status = qjsky_module_compile(thread, name,
							   txt, txtlen, url);
		}
	}

	load = (mod != NULL) ? qjsky_module_new_load(thread, name) : NULL;
	free(name);
	if (load == NULL) {
		qjsky_leave_thread(thread);
		return JS_MODULE_FAILED;
	}
	load->cb = cb;
	load->pw = pw;

	status = qjsky_module_graph(thread, load->url);
	switch (status) {
	case JS_MODULE_READY:
		*module_out = load;
		break;
	case JS_MODULE_PENDING:
		load->waiting = true;
		*module_out = load;
		break;
	default:
		qjsky_module_free(load);
		break;
	}

	qjsky_leave_thread(thread);

	return status;
}


/* exported interface documented in js.h */
bool js_module_exec(jsthread *thread, struct js_module *module)
{
	JSContext *jctx = thread->jctx;
	JSModuleDef *m;
	bool ret = false;

	assert(module->waiting == false);

	if (thread->pending_destroy) {
		qjsky_module_free(module);
		return false;
	}

	qjsky_enter_thread(thread);
	qjsky_enter_js(thread->heap, true);

	m = JS_FindLoadedModule(jctx, module->url);
	if (m != NULL) {
		JSValue val = JS_DupValue(jctx, JS_MKPTR(JS_TAG_MODULE, m));

		if (JS_ResolveModule(jctx, val) < 0) {
			qjsky_dump_exception(jctx);
			JS_FreeValue(jctx, val);
		} else {
			/* evaluation returns a promise, which is rejected
			 * if the module throws and is then reported as
			 * an unhandled rejection
			 */
			JSValue res = JS_EvalFunction(jctx, val);

			if (JS_IsException(res)) {
				qjsky_dump_exception(jctx);
			} else {
				ret = true;
			}
			JS_FreeValue(jctx, res);
		}
	}

	qjsky_module_free(module);

	qjsky_leave_js(thread->heap);
	qjsky_leave_thread(thread);

	return ret;
}


/* exported interface documented in js.h */
void js_initialise(void)
{
	javascript_init();
}


/* exported interface documented in js.h */
void js_finalise(void)
{
	/* NADA for now */
}


/* exported interface documented in js.h */
nserror js_newheap(int timeout, jsheap **heap)
{
	jsheap *ret;
	int idx;

	*heap = NULL;

	ret = calloc(1, sizeof(*ret));
	if (ret == NULL) {
		return NSERROR_NOMEM;
	}

	NSLOG(qjsky, DEBUG, "Creating new QuickJS javascript heap");

	ret->rt = JS_NewRuntime();
	if (ret->rt == NULL) {
		free(ret);
		return NSERROR_NOMEM;
	}

	JS_SetRuntimeOpaque(ret->rt, ret);
	JS_SetInterruptHandler(ret->rt, qjsky_check_timeout, ret);
	JS_SetHostPromiseRejectionTracker(ret->rt, qjsky_promise_rejection, ret);
	JS_SetModuleLoaderFunc(ret->rt, qjsky_module_normalize,
			       qjsky_module_loader, ret);
	JS_SetDynamicImportHook(ret->rt, qjsky_module_dynamic_import, ret);

	/* register the binding classes, class ids are allocated
	 * sequentially so the range check in qjsky_class_index works
	 */
	for (idx = 0; idx < QJSKY_PROTO__COUNT; idx++) {
		JSClassDef def = {
			.class_name = qjsky_classes[idx]->name,
			.finalizer = qjsky_finalizer,
		};
		JSClassID cid = 0;

		JS_NewClassID(ret->rt, &cid);
		assert((idx == 0) || (cid == ret->class_id[0] + idx));
		if (JS_NewClass(ret->rt, cid, &def) != 0) {
			JS_FreeRuntime(ret->rt);
			free(ret);
			return NSERROR_NOMEM;
		}
		ret->class_id[idx] = cid;
	}

	*heap = ret;
	return NSERROR_OK;
}


/**
 * Destroy a heap once it has no threads left.
 */
static void qjsky_destroyheap(jsheap *heap)
{
	assert(heap->pending_destroy == true);
	assert(heap->live_threads == 0);

	NSLOG(qjsky, DEBUG, "Destroying QuickJS javascript heap");

	assert(heap->rejection_count == 0);
	free(heap->rejections);
	JS_FreeRuntime(heap->rt);
	free(heap);
}


/* exported interface documented in js.h */
void js_destroyheap(jsheap *heap)
{
	heap->pending_destroy = true;
	if (heap->live_threads == 0) {
		qjsky_destroyheap(heap);
	}
}


/**
 * Load the generics table into a thread.
 *
 * generics.js is evaluated in a function so its NetSurf variable does
 * not become a global.
 */
static nserror qjsky_load_generics(jsthread *thread)
{
	JSValue res;

	res = qjsky_eval(thread->jctx, generics_js, generics_js_len,
			 "(function(){\n", "\nreturn NetSurf;\n})()",
			 "generics.js");
	if (JS_IsException(res)) {
		qjsky_dump_exception(thread->jctx);
		return NSERROR_INIT_FAILED;
	}

	qjsduk_push_value(thread->ctx, res);
	duk_put_global_string(thread->ctx, GENERICS_MAGIC);

	return NSERROR_OK;
}


/**
 * Construct the Window as the global object of a thread.
 */
static nserror
qjsky_create_window(jsthread *thread, void *win_priv, void *doc_priv)
{
	duk_context *ctx = thread->ctx;
	JSValue proto;
	int res;

	proto = JS_GetClassProto(thread->jctx,
				 thread->heap->class_id[QJSKY_PROTO_GLOBAL]);
	res = JS_SetPrototype(thread->jctx, thread->global, proto);
	JS_FreeValue(thread->jctx, proto);
	if (res < 0) {
		qjsky_dump_exception(thread->jctx);
		return NSERROR_INIT_FAILED;
	}

	thread->window_priv = calloc(1, qjsky_classes[QJSKY_PROTO_GLOBAL]->priv_size);
	if (thread->window_priv == NULL) {
		return NSERROR_NOMEM;
	}

	/* win_priv is a browser_window, doc_priv is an html content struct */
	duk_push_pointer(ctx, win_priv);
	duk_push_pointer(ctx, doc_priv);
	if (qjsky_init_object(ctx, QJSKY_PROTO_GLOBAL,
			      JS_DupValue(thread->jctx, thread->global),
			      thread->window_priv, 2) != DUK_EXEC_SUCCESS) {
		NSLOG(qjsky, CRITICAL, "Unable to construct Window: %s",
		      duk_safe_to_string(ctx, -1));
		duk_pop(ctx);
		return NSERROR_INIT_FAILED;
	}
	duk_pop(ctx);

	return NSERROR_OK;
}


/* exported interface documented in js.h */
nserror
js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
	jsthread *ret;
	JSValue res;
	nserror err;

	assert(heap != NULL);
	assert(heap->pending_destroy == false);

	*thread = NULL;

	ret = calloc(1, sizeof (*ret));
	if (ret == NULL) {
		NSLOG(qjsky, ERROR, "Unable to allocate new JS thread structure");
		return NSERROR_NOMEM;
	}

	NSLOG(qjsky, DEBUG,
	      "New javascript/QuickJS thread, win_priv=%p, doc_priv=%p",
	      win_priv, doc_priv);

	ret->heap = heap;
	ret->global = JS_UNDEFINED;
	ret->jctx = JS_NewContext(heap->rt);
	if (ret->jctx == NULL) {
		free(ret);
		return NSERROR_NOMEM;
	}
	ret->ctx = qjsduk_create(ret->jctx, ret);
	if (ret->ctx == NULL) {
		JS_FreeContext(ret->jctx);
		free(ret);
		return NSERROR_NOMEM;
	}
	ret->global = JS_GetGlobalObject(ret->jctx);
	heap->live_threads++;

	/* stop the timeout firing during construction */
	heap->exec_start_time = 0;

	err = qjsky_create_prototypes(ret);
	if (err == NSERROR_OK) {
		err = qjsky_create_window(ret, win_priv, doc_priv);
	}
	if (err != NSERROR_OK) {
		NSLOG(qjsky, CRITICAL, "Unable to create Window, thread aborted");
		js_destroythread(ret);
		return err;
	}

	/* Now we need to prepare our node mapping table */
	duk_push_object(ret->ctx);
	duk_push_pointer(ret->ctx, NULL);
	duk_push_null(ret->ctx);
	duk_put_prop(ret->ctx, -3);
	duk_put_global_string(ret->ctx, NODE_MAGIC);

	/* And now the event mapping table */
	duk_push_object(ret->ctx);
	duk_put_global_string(ret->ctx, EVENT_MAGIC);

	/* Now load the polyfills */
	res = qjsky_eval(ret->jctx, polyfill_js, polyfill_js_len,
			 "", "", "polyfill.js");
	if (JS_IsException(res)) {
		qjsky_dump_exception(ret->jctx);
		NSLOG(qjsky, CRITICAL, "Unable to run polyfill.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}
	JS_FreeValue(ret->jctx, res);

	/* Now load the NetSurf table in */
	if (qjsky_load_generics(ret) != NSERROR_OK) {
		NSLOG(qjsky, CRITICAL, "Unable to run generics.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}

	/* And the interfaces implemented in javascript */
	JS_SetPropertyFunctionList(ret->jctx, ret->global, qjsky_native_funcs,
				   NOF_ELEMENTS(qjsky_native_funcs));
	res = qjsky_eval(ret->jctx, webcompat_js, webcompat_js_len,
			 "", "", "webcompat.js");
	if (JS_IsException(res)) {
		qjsky_dump_exception(ret->jctx);
		NSLOG(qjsky, CRITICAL, "Unable to run webcompat.js, thread aborted");
		js_destroythread(ret);
		return NSERROR_INIT_FAILED;
	}
	JS_FreeValue(ret->jctx, res);

	dukky_log_stack_frame(ret->ctx, "New thread created");
	NSLOG(qjsky, DEBUG, "New thread is %p in heap %p", ret, heap);
	*thread = ret;

	return NSERROR_OK;
}


/* Now switch to the long term CTX behaviour */
#define CTX (thread->ctx)

/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	/* We can always close down a thread, it might just confuse
	 * the code running, though we don't mind since we're in the
	 * process of destruction at this point
	 */
	duk_idx_t top = duk_get_top(CTX);

	/* Closing down the extant thread */
	NSLOG(qjsky, DEBUG, "Closing down extant thread %p in heap %p", thread, thread->heap);
	qjsky_fetch_abort_all(thread);
	duk_get_global_string(CTX, MAGIC(closedownThread));
	dukky_pcall(CTX, 0, true);

	/* Restore whatever stack we had */
	duk_set_top(CTX, top);

	return NSERROR_OK;
}


/**
 * Destroy a QuickJS thread
 */
static void qjsky_destroythread(jsthread *thread)
{
	jsheap *heap = thread->heap;

	assert(thread->in_use == 0);
	assert(thread->pending_destroy == true);

	NSLOG(qjsky, DEBUG, "Destroying thread %p in heap %p", thread, heap);

	qjsky_fetch_abort_all(thread);

	if (thread->window_priv != NULL) {
		/* Closing down the extant thread */
		duk_get_global_string(CTX, MAGIC(closedownThread));
		dukky_pcall(CTX, 0, true);
		duk_set_top(CTX, 0);

		/* The Window is not finalised by QuickJS as the global
		 * object is not a binding object.
		 */
		qjsky_classes[QJSKY_PROTO_GLOBAL]->destroy(CTX, thread->window_priv);
		free(thread->window_priv);
		thread->window_priv = NULL;
	}

	qjsky_module_free_all(thread);
	qjsduk_destroy(thread->ctx);
	qjsky_flush_rejections(heap, thread->jctx);
	JS_FreeValue(thread->jctx, thread->global);
	JS_FreeContext(thread->jctx);
	free(thread);

	/* Finally give the heap a chance to clean up */
	JS_RunGC(heap->rt);
	heap->live_threads--;

	/* And if the heap should now go, blow it away */
	if (heap->pending_destroy == true && heap->live_threads == 0) {
		qjsky_destroyheap(heap);
	}
}


/* exported interface documented in js.h */
void js_destroythread(jsthread *thread)
{
	thread->pending_destroy = true;
	if (thread->in_use == 0) {
		qjsky_destroythread(thread);
	}
}


static void qjsky_enter_thread(jsthread *thread)
{
	assert(thread != NULL);
	thread->in_use++;
}


static void qjsky_leave_thread(jsthread *thread)
{
	assert(thread != NULL);
	assert(thread->in_use > 0);

	thread->in_use--;
	if (thread->in_use == 0 && thread->pending_destroy == true) {
		qjsky_destroythread(thread);
	}
}


/* exported interface documented in js.h */
bool
js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
	JSValue res;
	bool ret = false;

	assert(thread);

	if (txt == NULL || txtlen == 0) {
		return false;
	}

	if (thread->pending_destroy) {
		NSLOG(qjsky, DEEPDEBUG, "Skipping exec call because thread is dead");
		return false;
	}

	qjsky_enter_thread(thread);

	NSLOG(qjsky, DEEPDEBUG, "Running %"PRIsizet" bytes from %s", txtlen, name);

	qjsky_enter_js(thread->heap, true);
	res = qjsky_eval(thread->jctx, txt, txtlen, "", "",
			 name != NULL ? name : "?unknown source?");
	if (JS_IsException(res)) {
		NSLOG(qjsky, DEBUG, "Failed to execute JavaScript");
		qjsky_dump_exception(thread->jctx);
	} else {
		ret = JS_IsBool(res) && JS_ToBool(thread->jctx, res);
		JS_FreeValue(thread->jctx, res);
	}
	qjsky_leave_js(thread->heap);

	qjsky_leave_thread(thread);
	return ret;
}

static int dukky_event_proto(dom_event *evt)
{
	int ret = PROTO_NAME(EVENT);
	dom_string *type = NULL;
	dom_exception err;

	err = dom_event_get_type(evt, &type);
	if (err != DOM_NO_ERR) {
		goto out;
	}

	if (dom_string_isequal(type, corestring_dom_keydown)) {
		ret = PROTO_NAME(KEYBOARDEVENT);
		goto out;
	} else if (dom_string_isequal(type, corestring_dom_keyup)) {
		ret = PROTO_NAME(KEYBOARDEVENT);
		goto out;
	} else if (dom_string_isequal(type, corestring_dom_keypress)) {
		ret = PROTO_NAME(KEYBOARDEVENT);
		goto out;
	}

out:
	if (type != NULL) {
		dom_string_unref(type);
	}

	return ret;
}

/*** New style event handling ***/

/* exported interface documented in qjsky.h */
void dukky_push_event(duk_context *ctx, dom_event *evt)
{
	/* ... */
	duk_get_global_string(ctx, EVENT_MAGIC);
	/* ... events */
	duk_push_pointer(ctx, evt);
	/* ... events eventptr */
	duk_get_prop(ctx, -2);
	/* ... events event? */
	if (duk_is_undefined(ctx, -1)) {
		/* ... events undefined */
		duk_pop(ctx);
		/* ... events */
		duk_push_pointer(ctx, evt);
		if (dukky_create_object(ctx, dukky_event_proto(evt), 1) != DUK_EXEC_SUCCESS) {
			/* ... events err */
			duk_pop(ctx);
			/* ... events */
			duk_push_object(ctx);
			/* ... events eobj[meh] */
		}
		/* ... events eobj */
		duk_push_pointer(ctx, evt);
		/* ... events eobj eventptr */
		duk_dup(ctx, -2);
		/* ... events eobj eventptr eobj */
		duk_put_prop(ctx, -4);
		/* ... events eobj */
	}
	/* ... events event */
	duk_replace(ctx, -2);
	/* ... event */
}

static void dukky_push_handler_code_(duk_context *ctx, dom_string *name,
				     dom_event_target *et)
{
	dom_string *onname, *val;
	dom_element *ele = (dom_element *)et;
	dom_exception exc;
	dom_node_type ntype;

	/* If et is NULL, then we're actually dealing with the Window object
	 * which has no default handlers and no way to assign handlers
	 * which aren't directly stored in the HANDLER_MAGIC
	 */
	if (et == NULL) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	/* The rest of this assumes et is a proper event target and expands
	 * out from there based on the assumption that all valid event targets
	 * are nodes.
	 */
	exc = dom_node_get_node_type(et, &ntype);
	if (exc != DOM_NO_ERR) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	if (ntype != DOM_ELEMENT_NODE) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	exc = dom_string_concat(corestring_dom_on, name, &onname);
	if (exc != DOM_NO_ERR) {
		duk_push_lstring(ctx, "", 0);
		return;
	}

	exc = dom_element_get_attribute(ele, onname, &val);
	if ((exc != DOM_NO_ERR) || (val == NULL)) {
		dom_string_unref(onname);
		duk_push_lstring(ctx, "", 0);
		return;
	}

	dom_string_unref(onname);
	duk_push_lstring(ctx, dom_string_data(val), dom_string_length(val));
	dom_string_unref(val);
}

/* exported interface documented in qjsky.h */
bool dukky_get_current_value_of_event_handler(duk_context *ctx,
					      dom_string *name,
					      dom_event_target *et)
{
	/* Must be entered as:
	 * ... node(et)
	 */
	duk_get_prop_string(ctx, -1, HANDLER_MAGIC);
	/* ... node handlers */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	/* ... node handlers name */
	duk_get_prop(ctx, -2);
	/* ... node handlers handler? */
	if (duk_is_undefined(ctx, -1)) {
		/* ... node handlers undefined */
		duk_pop_2(ctx);
		/* ... node */
		dukky_push_handler_code_(ctx, name, et);
		/* ... node handlercode? */
		/* TODO: If this is null, clean up and propagate */
		/* ... node handlercode */
		/** @todo This is entirely wrong, but it's hard to get right */
		duk_push_string(ctx, "function (event) {");
		/* ... node handlercode prefix */
		duk_insert(ctx, -2);
		/* ... node prefix handlercode */
		duk_push_string(ctx, "}");
		/* ... node prefix handlercode suffix */
		duk_concat(ctx, 3);
		/* ... node fullhandlersrc */
		duk_push_string(ctx, "internal raw uncompiled handler");
		/* ... node fullhandlersrc filename */
		if (duk_pcompile(ctx, DUK_COMPILE_FUNCTION) != 0) {
			/* ... node err */
			NSLOG(qjsky, DEBUG,
			      "Unable to proceed with handler, could not compile");
			duk_pop_2(ctx);
			return false;
		}
		/* ... node handler */
		duk_insert(ctx, -2);
		/* ... handler node */
	} else {
		/* ... node handlers handler */
		duk_insert(ctx, -3);
		/* ... handler node handlers */
		duk_pop(ctx);
		/* ... handler node */
	}
	/* ... handler node */
	return true;
}

/**
 * Log an error raised by an event handler which is at the stack top.
 */
static void qjsky_log_handler_error(duk_context *ctx)
{
	/* ... err */
	duk_get_prop_string(ctx, -1, "name");
	duk_get_prop_string(ctx, -2, "message");
	duk_get_prop_string(ctx, -3, "fileName");
	duk_get_prop_string(ctx, -4, "lineNumber");
	duk_get_prop_string(ctx, -5, "stack");
	/* ... err name message fileName lineNumber stack */
	NSLOG(jserrors, WARNING, "Uncaught error in JS: %s: %s",
	      duk_safe_to_string(ctx, -5),
	      duk_safe_to_string(ctx, -4));
	NSLOG(jserrors, INFO, "              was at: %s line %s",
	      duk_safe_to_string(ctx, -3),
	      duk_safe_to_string(ctx, -2));
	NSLOG(jserrors, INFO, "         Stack trace: %s",
	      duk_safe_to_string(ctx, -1));
	duk_pop_n(ctx, 5);
	/* ... err */
}

/**
 * Push the object for an event target, NULL being the Window.
 */
static void qjsky_push_target(duk_context *ctx, dom_event_target *targ)
{
	if (targ == NULL) {
		duk_push_global_object(ctx);
	} else {
		dukky_push_node(ctx, (dom_node *)targ);
	}
}

/**
 * Call the javascript listeners added with addEventListener.
 *
 * \param ctx The compatibility context
 * \param name The event type
 * \param evt The event
 * \param phase The phase of the event flow
 * \param targ The current target or NULL for the Window
 */
static void
qjsky_call_listeners(duk_context *ctx,
		     dom_string *name,
		     dom_event *evt,
		     dom_event_flow_phase phase,
		     dom_event_target *targ)
{
	dom_exception exc;
	duk_uarridx_t idx;
	event_listener_flags flags;

	/* ... */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	qjsky_push_target(ctx, targ);
	/* ... type node */
	if (dukky_event_target_push_listeners(ctx, true)) {
		/* Nothing to do */
		duk_pop(ctx);
		return;
	}
	/* ... sublisteners */
	duk_push_array(ctx);
	/* ... sublisteners copy */
	idx = 0;
	while (duk_get_prop_index(ctx, -2, idx)) {
		/* ... sublisteners copy handler */
		duk_get_prop_index(ctx, -1, 1);
		/* ... sublisteners copy handler flags */
		if ((event_listener_flags)duk_to_int(ctx, -1) & ELF_ONCE) {
			duk_dup(ctx, -4);
			/* ... subl copy handler flags subl */
			dukky_shuffle_array(ctx, idx);
			duk_pop(ctx);
			/* ... subl copy handler flags */
		}
		duk_pop(ctx);
		/* ... sublisteners copy handler */
		duk_put_prop_index(ctx, -2, idx);
		/* ... sublisteners copy */
		idx++;
	}
	/* ... sublisteners copy undefined */
	duk_pop(ctx);
	/* ... sublisteners copy */
	duk_insert(ctx, -2);
	/* ... copy sublisteners */
	duk_pop(ctx);
	/* ... copy */
	idx = 0;
	while (duk_get_prop_index(ctx, -1, idx++)) {
		/* ... copy handler */
		if (duk_get_prop_index(ctx, -1, 2)) {
			/* ... copy handler meh */
			duk_pop_2(ctx);
			continue;
		}
		duk_pop(ctx);
		duk_get_prop_index(ctx, -1, 0);
		duk_get_prop_index(ctx, -2, 1);
		/* ... copy handler callback flags */
		flags = (event_listener_flags)duk_get_int(ctx, -1);
		duk_pop(ctx);
		/* ... copy handler callback */
		if (((phase == DOM_CAPTURING_PHASE) && !(flags & ELF_CAPTURE)) ||
		    ((phase != DOM_CAPTURING_PHASE) && (flags & ELF_CAPTURE))) {
			duk_pop_2(ctx);
			/* ... copy */
			continue;
		}
		/* ... copy handler callback */
		qjsky_push_target(ctx, targ);
		/* ... copy handler callback node */
		dukky_push_event(ctx, evt);
		/* ... copy handler callback node event */
		if (qjsky_pcall_method(ctx, 1) != 0) {
			/* Failed to run the method */
			/* ... copy handler err */
			NSLOG(qjsky, DEBUG,
			      "OH NOES! An error running a callback.  Meh.");
			exc = dom_event_stop_immediate_propagation(evt);
			if (exc != DOM_NO_ERR)
				NSLOG(qjsky, DEBUG,
				      "WORSE! could not stop propagation");
			qjsky_log_handler_error(ctx);
			duk_pop_2(ctx);
			/* ... copy */
			continue;
		}
		/* ... copy handler result */
		if (duk_is_boolean(ctx, -1) &&
		    duk_to_boolean(ctx, -1) == 0) {
			dom_event_prevent_default(evt);
		}
		duk_pop_2(ctx);
		/* ... copy */
	}
	duk_pop_2(ctx);
}

static void dukky_generic_event_handler(dom_event *evt, void *pw)
{
	duk_context *ctx = (duk_context *)pw;
	dom_string *name;
	dom_exception exc;
	dom_event_target *targ;
	dom_event_flow_phase phase;

	NSLOG(qjsky, DEBUG, "Handling an event in QuickJS interface...");
	exc = dom_event_get_type(evt, &name);
	if (exc != DOM_NO_ERR) {
		NSLOG(qjsky, DEBUG, "Unable to find the event name");
		return;
	}
	NSLOG(qjsky, DEBUG, "Event's name is %*s", (int)dom_string_length(name),
	      dom_string_data(name));
	exc = dom_event_get_event_phase(evt, &phase);
	if (exc != DOM_NO_ERR) {
		NSLOG(qjsky, WARNING, "Unable to get event phase");
		return;
	}
	NSLOG(qjsky, DEBUG, "Event phase is: %s (%d)",
	      phase == DOM_CAPTURING_PHASE ? "capturing" : phase == DOM_AT_TARGET ? "at-target" : phase == DOM_BUBBLING_PHASE ? "bubbling" : "unknown",
	      (int)phase);

	exc = dom_event_get_current_target(evt, &targ);
	if (exc != DOM_NO_ERR) {
		dom_string_unref(name);
		NSLOG(qjsky, DEBUG, "Unable to find the event target");
		return;
	}

	/* If we're capturing right now, we skip the 'event handler'
	 * and go straight to the extras
	 */
	if (phase == DOM_CAPTURING_PHASE)
		goto handle_extras;

	/* ... */
	if (dukky_push_node(ctx, (dom_node *)targ) == false) {
		dom_string_unref(name);
		dom_node_unref(targ);
		NSLOG(qjsky, DEBUG,
		      "Unable to push JS node representation?!");
		duk_pop(ctx);
		return;
	}
	/* ... node */
	if (dukky_get_current_value_of_event_handler(
		    ctx, name, (dom_event_target *)targ) == false) {
		/* ... */
		goto handle_extras;
	}
	/* ... handler node */
	dukky_push_event(ctx, evt);
	/* ... handler node event */
	if (qjsky_pcall_method(ctx, 1) != 0) {
		/* Failed to run the method */
		/* ... err */
		NSLOG(qjsky, DEBUG,
		      "OH NOES! An error running a callback.  Meh.");
		exc = dom_event_stop_immediate_propagation(evt);
		if (exc != DOM_NO_ERR)
			NSLOG(qjsky, DEBUG,
			      "WORSE! could not stop propagation");
		qjsky_log_handler_error(ctx);
		duk_pop(ctx);
		/* ... */
		goto handle_extras;
	}
	/* ... result */
	if (duk_is_boolean(ctx, -1) &&
	    duk_to_boolean(ctx, -1) == 0) {
		dom_event_prevent_default(evt);
	}
	duk_pop(ctx);
handle_extras:
	qjsky_call_listeners(ctx, name, evt, phase, targ);

	dom_node_unref(targ);
	dom_string_unref(name);
}

/* exported interface documented in qjsky.h */
void dukky_register_event_listener_for(duk_context *ctx,
				       struct dom_element *ele,
				       dom_string *name,
				       bool capture)
{
	dom_event_listener *listen = NULL;
	dom_exception exc;

	/* ... */
	if (ele == NULL) {
		/* A null element is the Window object */
		duk_push_global_object(ctx);
	} else {
		/* Non null elements must be pushed as a node object */
		if (dukky_push_node(ctx, (struct dom_node *)ele) == false) {
			duk_pop(ctx);
			return;
		}
	}
	/* ... node */
	duk_get_prop_string(ctx, -1, HANDLER_LISTENER_MAGIC);
	/* ... node handlers */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	/* ... node handlers name */
	if (duk_has_prop(ctx, -2)) {
		/* ... node handlers */
		duk_pop_2(ctx);
		/* ... */
		return;
	}
	/* ... node handlers */
	duk_push_lstring(ctx, dom_string_data(name), dom_string_length(name));
	/* ... node handlers name */
	duk_push_boolean(ctx, true);
	/* ... node handlers name true */
	duk_put_prop(ctx, -3);
	/* ... node handlers */
	duk_pop_2(ctx);
	/* ... */
	if (ele == NULL) {
		/* Nothing more to do, Window doesn't register in the
		 * normal event listener flow
		 */
		return;
	}

	/* Otherwise add an event listener to the element */
	exc = dom_event_listener_create(dukky_generic_event_handler, ctx,
					&listen);
	if (exc != DOM_NO_ERR) return;
	exc = dom_event_target_add_event_listener(
		ele, name, listen, capture);
	if (exc != DOM_NO_ERR) {
		NSLOG(qjsky, DEBUG,
		      "Unable to register listener for %p.%*s", ele,
		      (int)dom_string_length(name), dom_string_data(name));
	} else {
		NSLOG(qjsky, DEBUG, "have registered listener for %p.%*s",
		      ele, (int)dom_string_length(name), dom_string_data(name));
	}
	dom_event_listener_unref(listen);
}

/* The sub-listeners are a list of {callback,flags} tuples */
/* We return true if we created a new sublistener table */
/* If we're told to not create, but we want to, we still return true */
bool dukky_event_target_push_listeners(duk_context *ctx, bool dont_create)
{
	bool ret = false;
	/* ... type this */
	duk_get_prop_string(ctx, -1, EVENT_LISTENER_JS_MAGIC);
	if (duk_is_undefined(ctx, -1)) {
		/* ... type this null */
		duk_pop(ctx);
		duk_push_object(ctx);
		duk_dup(ctx, -1);
		/* ... type this listeners listeners */
		duk_put_prop_string(ctx, -3, EVENT_LISTENER_JS_MAGIC);
		/* ... type this listeners */
	}
	/* ... type this listeners */
	duk_insert(ctx, -3);
	/* ... listeners type this */
	duk_pop(ctx);
	/* ... listeners type */
	duk_dup(ctx, -1);
	/* ... listeners type type */
	duk_get_prop(ctx, -3);
	/* ... listeners type ??? */
	if (duk_is_undefined(ctx, -1)) {
		/* ... listeners type ??? */
		if (dont_create == true) {
			duk_pop_3(ctx);
			duk_push_undefined(ctx);
			return true;
		}
		duk_pop(ctx);
		duk_push_array(ctx);
		duk_dup(ctx, -2);
		duk_dup(ctx, -2);
		/* ... listeners type sublisteners type sublisteners */
		duk_put_prop(ctx, -5);
		/* ... listeners type sublisteners */
		ret = true;
	}
	duk_insert(ctx, -3);
	/* ... sublisteners listeners type */
	duk_pop_2(ctx);
	/* ... sublisteners */
	return ret;
}

/* Shuffle an array "down" one.  This involves iterating from
 * the index provided, shuffling elements down, until we reach an
 * undefined
 */
void dukky_shuffle_array(duk_context *ctx, duk_uarridx_t idx)
{
	/* ... somearr */
	while (duk_get_prop_index(ctx, -1, idx + 1)) {
		duk_put_prop_index(ctx, -2, idx);
		idx++;
	}
	/* ... somearr undefined */
	duk_del_prop_index(ctx, -2, idx + 1);
	duk_pop(ctx);
}


/* exported interface documented in js.h */
void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	assert(thread);
	assert(node);
	dom_namednodemap *map;
	dom_exception exc;
	dom_ulong idx;
	dom_ulong siz;
	dom_attr *attr = NULL;
	dom_string *key = NULL;
	dom_string *nodename;
	duk_bool_t is_body = false;

	exc = dom_node_get_node_name(node, &nodename);
	if (exc != DOM_NO_ERR) return;

	if (nodename == corestring_dom_BODY)
		is_body = true;

	dom_string_unref(nodename);

	exc = dom_node_get_attributes(node, &map);
	if (exc != DOM_NO_ERR) return;
	if (map == NULL) return;

	qjsky_enter_thread(thread);

	exc = dom_namednodemap_get_length(map, &siz);
	if (exc != DOM_NO_ERR) goto out;

	for (idx = 0; idx < siz; idx++) {
		exc = dom_namednodemap_item(map, idx, &attr);
		if (exc != DOM_NO_ERR) goto out;
		exc = dom_attr_get_name(attr, &key);
		if (exc != DOM_NO_ERR) goto out;
		if (is_body && (
			    key == corestring_dom_onblur ||
			    key == corestring_dom_onerror ||
			    key == corestring_dom_onfocus ||
			    key == corestring_dom_onload ||
			    key == corestring_dom_onresize ||
			    key == corestring_dom_onscroll)) {
			/* This is a forwarded event, it doesn't matter,
			 * we should skip registering for it and later
			 * we will register it for Window itself
			 */
			goto skip_register;
		}
		if (dom_string_length(key) > 2) {
			/* Can be on* */
			const uint8_t *data = (const uint8_t *)dom_string_data(key);
			if (data[0] == 'o' && data[1] == 'n') {
				dom_string *sub = NULL;
				exc = dom_string_substr(
					key, 2, dom_string_length(key),
					&sub);
				if (exc == DOM_NO_ERR) {
					dukky_register_event_listener_for(
						CTX, node, sub, false);
					dom_string_unref(sub);
				}
			}
		}
	skip_register:
		dom_string_unref(key); key = NULL;
		dom_node_unref(attr); attr = NULL;
	}

out:
	if (key != NULL)
		dom_string_unref(key);

	if (attr != NULL)
		dom_node_unref(attr);

	dom_namednodemap_unref(map);

	qjsky_leave_thread(thread);
}


/* exported interface documented in js.h */
void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	assert(thread);
	qjsky_enter_thread(thread);
	/* ... */
	duk_get_global_string(CTX, EVENT_MAGIC);
	/* ... EVENT_MAP */
	duk_push_pointer(CTX, evt);
	/* ... EVENT_MAP eventptr */
	duk_del_prop(CTX, -2);
	/* ... EVENT_MAP */
	duk_pop(CTX);
	/* ... */
	qjsky_leave_thread(thread);
}


/**
 * Run the Window's part of an event.
 *
 * Calls the Window's on<type> handler and then its listeners.  The load
 * handler may come from the body's onload attribute, which is forwarded
 * to the Window.
 *
 * \param thread The thread to run in
 * \param evt The event
 * \param type The event type
 * \param doc The document, for the body load handler
 * \param phase The event phase at the Window
 */
static void
qjsky_window_event(jsthread *thread,
		   dom_event *evt,
		   dom_string *type,
		   struct dom_document *doc,
		   dom_event_flow_phase phase)
{
	dom_event_target *body;
	dom_exception exc;
	bool have_handler = true;

	/* ... */
	duk_get_global_string(CTX, HANDLER_MAGIC);
	/* ... handlers */
	duk_push_lstring(CTX, dom_string_data(type), dom_string_length(type));
	/* ... handlers type */
	duk_get_prop(CTX, -2);
	/* ... handlers handler? */
	if (duk_is_undefined(CTX, -1) &&
	    dom_string_isequal(type, corestring_dom_load)) {
		/* No handler here, *try* and retrieve a handler from
		 * the body
		 */
		duk_pop(CTX);
		/* ... handlers */
		exc = dom_html_document_get_body(doc, &body);
		if ((exc != DOM_NO_ERR) || (body == NULL)) {
			duk_push_undefined(CTX);
		} else {
			dukky_push_node(CTX, (struct dom_node *)body);
			/* ... handlers bodynode */
			if (dukky_get_current_value_of_event_handler(
				    CTX, corestring_dom_load, body) == false) {
				/* ... handlers */
				duk_push_undefined(CTX);
			} else {
				/* ... handlers handler bodynode */
				duk_pop(CTX);
			}
			dom_node_unref(body);
		}
	}
	/* ... handlers handler */
	duk_remove(CTX, -2);
	/* ... handler */
	if (duk_is_undefined(CTX, -1)) {
		duk_pop(CTX);
		have_handler = false;
	}

	if (have_handler) {
		/* ... handler */
		duk_push_global_object(CTX);
		/* ... handler Window */
		dukky_push_event(CTX, evt);
		/* ... handler Window event */
		if (qjsky_pcall_method(CTX, 1) != 0) {
			/* ... err */
			qjsky_log_handler_error(CTX);
		} else if (duk_is_boolean(CTX, -1) &&
			   duk_to_boolean(CTX, -1) == 0) {
			dom_event_prevent_default(evt);
		}
		/* ... result */
		duk_pop(CTX);
	}

	/* ... */
	qjsky_call_listeners(CTX, type, evt, phase, NULL);
}


/* exported interface documented in js.h */
bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target)
{
	dom_exception exc;
	dom_event *evt;
	dom_string *type_str;
	bool bubbles;
	bool success;

	NSLOG(qjsky, DEBUG, "Event: %s (doc=%p, target=%p)", type, doc,
	      target);

	if (thread->pending_destroy) {
		return true;
	}

	/* Of the events fired by the browser, DOMContentLoaded bubbles from
	 * the Document to the Window.  The others are fired at the Window.
	 */
	bubbles = (strcmp(type, "DOMContentLoaded") == 0);

	exc = dom_string_create((const uint8_t *)type, strlen(type), &type_str);
	if (exc != DOM_NO_ERR) {
		return true;
	}

	exc = dom_event_create(&evt);
	if (exc != DOM_NO_ERR) {
		dom_string_unref(type_str);
		return true;
	}
	exc = dom_event_init(evt, type_str, bubbles, false);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		dom_string_unref(type_str);
		return true;
	}

	qjsky_enter_thread(thread);

	if (target != NULL) {
		/* listeners on nodes are called through libdom */
		exc = dom_event_target_dispatch_event(target, evt, &success);
		if (exc != DOM_NO_ERR) {
			NSLOG(qjsky, DEBUG, "Unable to dispatch %s", type);
		}
	}

	if ((target == NULL) || bubbles) {
		qjsky_window_event(thread, evt, type_str, doc,
				   (target == NULL) ? DOM_AT_TARGET :
				   DOM_BUBBLING_PHASE);
	}

	js_event_cleanup(thread, evt);
	dom_event_unref(evt);
	dom_string_unref(type_str);
	qjsky_leave_thread(thread);

	return true;
}
