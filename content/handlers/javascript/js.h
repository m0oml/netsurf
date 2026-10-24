/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
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

/** \file
 * Interface to javascript engine functions.
 */

#ifndef NETSURF_JAVASCRIPT_JS_H_
#define NETSURF_JAVASCRIPT_JS_H_

#include "utils/errors.h"

struct dom_event;
struct dom_document;
struct dom_node;
struct dom_element;
struct dom_string;

/**
 * JavaScript interpreter heap
 *
 * In order to try and be moderately performant, we create a heap
 * per browser window.  This heap is shared by all browsing contexts
 * we end up creating in that window.
 */
typedef struct jsheap jsheap;

/**
 * JavaScript interpreter thread
 *
 * When we create a browsing context itself (window+content) we have
 * to create a JS thread to attach to the browsing context.
 *
 * JS threads are associated with heaps and will be destroyed when
 * the heap is destroyed.  They can be shut down manually though
 * and should be for object lifetime safety reasons.
 */
typedef struct jsthread jsthread;

/**
 * Initialise javascript interpreter
 */
void js_initialise(void);

/**
 * finalise javascript interpreter
 */
void js_finalise(void);

/**
 * Create a new javascript heap.
 *
 * There is usually one heap per browser window.
 *
 * \param timeout elapsed wallclock time (in seconds) before \a callback is called
 * \param heap Updated to the created JS heap
 * \return NSERROR_OK on success, appropriate error otherwise.
 */
nserror js_newheap(int timeout, jsheap **heap);

/**
 * Destroy a previously created heap.
 *
 * \param heap The heap to destroy
 */
void js_destroyheap(jsheap *heap);

/**
 * Create a new javascript thread
 *
 * This is called once for a page with javascript script tags on
 * it. It constructs a fresh global window object and prepares the JS
 * browsing context.  It's important that threads are shut down cleanly
 * when the browsing context is going to be cleaned up.
 *
 * \param heap The heap to create the thread within
 * \param win_priv The value to give to the Window constructor as the window
 * \param doc_priv The value to give to the Document constructor as the document
 * \param thread Updated to the created thread
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread);

/**
 * Close a javascript thread
 *
 * This should be called when the HTML content which owns the thread is
 * being closed.  This is a separate process from destroying the thread
 * and merely disconnects any callbacks and thus hopefully stops
 * additional JS things from triggering.  If any code runs and attempts to
 * register callbacks after closedown, they will fail.
 *
 * \param thread The thread to close down
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror js_closethread(jsthread *thread);

/**
 * Destroy a javascript thread
 *
 * This should be called when the browsing context is done with the thread.
 *
 * This will be called when the HTML content associated with the browsing
 * context is being destroyed.  The thread should have already been closed
 * down during the HTML content close.
 *
 * \param thread The thread to be destroyed
 */
void js_destroythread(jsthread *thread);

/**
 * execute some javascript in a context
 */
bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name);

/**
 * A module script and the modules it imports.
 */
struct js_module;

/**
 * Status of loading a module script.
 */
enum js_module_status {
	JS_MODULE_READY, /**< the module can be executed */
	JS_MODULE_PENDING, /**< imported modules are being fetched */
	JS_MODULE_FAILED /**< the module or one it imports failed to load */
};

/**
 * Callback for a module script whose imported modules have been fetched.
 *
 * \param pw The private data passed to js_module_load()
 * \param module The module, which can be executed if the status is
 *               JS_MODULE_READY and is otherwise freed after the callback
 * \param status JS_MODULE_READY or JS_MODULE_FAILED
 */
typedef void (js_module_cb)(void *pw, struct js_module *module, enum js_module_status status);

/**
 * Check if the javascript engine runs module scripts.
 *
 * \return true if module scripts are supported
 */
bool js_supports_modules(void);

/**
 * Add an import map, which says where module specifiers are to be found.
 *
 * \param thread The thread the map is for
 * \param txt The JSON text of the map
 * \param txtlen The length of the text
 */
void js_importmap(jsthread *thread, const uint8_t *txt, size_t txtlen);

/**
 * Load a module script, fetching the modules it imports.
 *
 * \param thread The thread to load the module in
 * \param txt The source of the module
 * \param txtlen The length of the source
 * \param url The URL of the module, or the base URL of the document for
 *            a module in the document
 * \param is_inline Whether the module is in the document
 * \param cb Callback for a module whose imports are being fetched
 * \param pw Private data for cb
 * \param module_out Updated with the module unless loading failed
 * \return JS_MODULE_READY if the module can be executed now,
 *         JS_MODULE_PENDING if cb will be called once its imports have
 *         been fetched, or JS_MODULE_FAILED
 */
enum js_module_status js_module_load(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *url, bool is_inline, js_module_cb *cb, void *pw, struct js_module **module_out);

/**
 * Execute a module script which has loaded, and free it.
 *
 * \param thread The thread the module was loaded in
 * \param module The module
 * \return true if the module was executed
 */
bool js_module_exec(jsthread *thread, struct js_module *module);

/**
 * fire an event at a dom node
 */
bool js_fire_event(jsthread *thread, const char *type, struct dom_document *doc, struct dom_node *target);

bool
js_dom_event_add_listener(jsthread *thread,
			  struct dom_document *document,
			  struct dom_node *node,
			  struct dom_string *event_type_dom,
			  void *js_funcval);

/*** New Events ***/

/**
 * Handle a new element being created.
 *
 * This is called once an element is inserted into the DOM document handled
 * by the context provided.  The JS implementation must then scan the element
 * for on* attributes and register appropriate listeners for those handlers.
 */
void js_handle_new_element(jsthread *thread, struct dom_element *node);

/**
 * Handle an event propagation finished callback.
 *
 * This is called once an event finishes propagating, no matter how it
 * finishes.  The intent here is that the JS context can perform any cleanups
 * it may need to perform before the DOM finishes and the event may end up
 * freed.
 */
void js_event_cleanup(jsthread *thread, struct dom_event *evt);

#endif /* NETSURF_JAVASCRIPT_JS_H_ */
