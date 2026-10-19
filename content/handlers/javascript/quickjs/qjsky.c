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
 * QuickJS-ng implementation of javascript engine functions.
 *
 * A jsheap is a QuickJS runtime and each jsthread is a context within
 * that runtime.  Contexts in one runtime share the garbage collector
 * but each has its own global object.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <nsutils/time.h>

#include "netsurf/inttypes.h"
#include "utils/utils.h"
#include "utils/log.h"
#include "content/content.h"

#include "javascript/js.h"
#include "javascript/content.h"

#include "engine/quickjs.h"
#include "duk_compat.h"

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
};

/**
 * qjsky javascript thread
 */
struct jsthread {
	bool pending_destroy; /**< Whether this thread is pending destruction */
	unsigned int in_use; /**< The number of times this thread is in use */
	jsheap *heap; /**< The heap this thread belongs to */
	JSContext *ctx; /**< The QuickJS context */
};


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
 * Prepare a heap for a new piece of execution.
 *
 * Resets the timeout start time and records the current C stack
 * position as the base for QuickJS stack overflow checks, as entry
 * points are called from different stack depths.
 *
 * \param heap The heap about to execute code
 */
static void qjsky_reset_start_time(jsheap *heap)
{
	(void) nsu_getmonotonic_ms(&heap->exec_start_time);
	JS_UpdateStackTop(heap->rt);
}


/**
 * Log an uncaught javascript error value.
 *
 * \param ctx The context the error belongs to
 * \param what Description of how the error was raised
 * \param err The thrown value, usually an Error object
 */
static void
qjsky_log_error(JSContext *ctx, const char *what, JSValueConst err)
{
	JSValue stack;
	const char *msg;
	const char *trace = NULL;

	msg = JS_ToCString(ctx, err);
	if (msg == NULL) {
		/* converting the value threw, discard that exception */
		JS_FreeValue(ctx, JS_GetException(ctx));
	}

	if (JS_IsError(err)) {
		stack = JS_GetPropertyStr(ctx, err, "stack");
		if (JS_IsString(stack)) {
			trace = JS_ToCString(ctx, stack);
		}
		JS_FreeValue(ctx, stack);
	}

	NSLOG(jserrors, WARNING, "%s: %s%s%s",
	      what,
	      msg != NULL ? msg : "[unprintable]",
	      trace != NULL ? "\n" : "",
	      trace != NULL ? trace : "");

	JS_FreeCString(ctx, trace);
	JS_FreeCString(ctx, msg);
}


/**
 * Log the pending exception of a context and clear it.
 *
 * \param ctx The context with a pending exception
 */
static void qjsky_dump_exception(JSContext *ctx)
{
	JSValue exc;

	exc = JS_GetException(ctx);
	qjsky_log_error(ctx, "Uncaught error in JS", exc);
	JS_FreeValue(ctx, exc);
}


/**
 * Promise rejection tracker which logs unhandled rejections.
 *
 * \todo Browsers only report a rejection which is still unhandled at
 *       the end of the microtask checkpoint; this logs immediately so
 *       a handler attached later still produces a log entry.
 *
 * \param ctx The context the promise belongs to
 * \param promise The rejected promise
 * \param reason The rejection value
 * \param is_handled true when a handler has been attached
 * \param opaque unused
 */
static void
qjsky_promise_rejection(JSContext *ctx,
			JSValueConst promise,
			JSValueConst reason,
			bool is_handled,
			void *opaque)
{
	if (!is_handled) {
		qjsky_log_error(ctx, "Uncaught (in promise)", reason);
	}
}


/**
 * Run all pending jobs (promise reactions) in a heap.
 *
 * This is the microtask checkpoint which must be performed after any
 * script or callback has run.
 *
 * \param heap The heap whose jobs should be run
 */
static void qjsky_run_jobs(jsheap *heap)
{
	JSContext *job_ctx;
	int res;

	while ((res = JS_ExecutePendingJob(heap->rt, &job_ctx)) != 0) {
		if (res < 0) {
			qjsky_dump_exception(job_ctx);
		}
	}
}


/* exported interface documented in duk_compat.h */
void *qjsduk_lookup_private(duk_context *ctx, JSValueConst obj)
{
	/** \todo Resolve binding objects once the bindings exist */
	return NULL;
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

	*heap = ret;
	return NSERROR_OK;
}


/**
 * Destroy a heap once it has no threads left.
 *
 * \param heap The heap to destroy
 */
static void qjsky_destroyheap(jsheap *heap)
{
	assert(heap->pending_destroy == true);
	assert(heap->live_threads == 0);

	NSLOG(qjsky, DEBUG, "Destroying QuickJS javascript heap");

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


/* exported interface documented in js.h */
nserror
js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread)
{
	jsthread *ret;

	assert(heap != NULL);
	assert(heap->pending_destroy == false);

	*thread = NULL;

	ret = calloc(1, sizeof(*ret));
	if (ret == NULL) {
		NSLOG(qjsky, ERROR, "Unable to allocate new JS thread structure");
		return NSERROR_NOMEM;
	}

	NSLOG(qjsky, DEBUG,
	      "New javascript/QuickJS thread, win_priv=%p, doc_priv=%p",
	      win_priv, doc_priv);

	ret->ctx = JS_NewContext(heap->rt);
	if (ret->ctx == NULL) {
		free(ret);
		return NSERROR_NOMEM;
	}
	JS_SetContextOpaque(ret->ctx, ret);

	ret->heap = heap;
	heap->live_threads++;

	/** \todo Construct the Window global and load polyfill.js and
	 * generics.js once the bindings exist.
	 */

	NSLOG(qjsky, DEBUG, "New thread is %p in heap %p", ret, heap);
	*thread = ret;

	return NSERROR_OK;
}


/* exported interface documented in js.h */
nserror js_closethread(jsthread *thread)
{
	NSLOG(qjsky, DEBUG, "Closing down extant thread %p in heap %p",
	      thread, thread->heap);

	/** \todo Call the closedownThread generic once generics.js is
	 * loaded.
	 */

	return NSERROR_OK;
}


/**
 * Destroy a thread once it is no longer in use.
 *
 * \param thread The thread to destroy
 */
static void qjsky_destroythread(jsthread *thread)
{
	jsheap *heap = thread->heap;

	assert(thread->in_use == 0);
	assert(thread->pending_destroy == true);

	NSLOG(qjsky, DEBUG, "Destroying thread %p in heap %p", thread, heap);

	JS_FreeContext(thread->ctx);
	free(thread);

	/* Give the heap a chance to collect any cycles */
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


/**
 * Mark a thread as in use so it is not destroyed under us.
 *
 * \param thread The thread being entered
 */
static void qjsky_enter_thread(jsthread *thread)
{
	assert(thread != NULL);
	thread->in_use++;
}


/**
 * Release a thread, destroying it if destruction was requested while
 * it was in use.
 *
 * \param thread The thread being left
 */
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
	char *src;
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

	/* JS_Eval requires the source to be NUL terminated, which the
	 * caller does not promise, so take a terminated copy.
	 */
	src = malloc(txtlen + 1);
	if (src == NULL) {
		goto out;
	}
	memcpy(src, txt, txtlen);
	src[txtlen] = '\0';

	qjsky_reset_start_time(thread->heap);
	res = JS_Eval(thread->ctx, src, txtlen,
		      name != NULL ? name : "?unknown source?",
		      JS_EVAL_TYPE_GLOBAL);
	free(src);

	if (JS_IsException(res)) {
		NSLOG(qjsky, DEBUG, "Failed to execute JavaScript");
		qjsky_dump_exception(thread->ctx);
	} else {
		ret = JS_IsBool(res) && JS_ToBool(thread->ctx, res);
		JS_FreeValue(thread->ctx, res);
	}

	qjsky_run_jobs(thread->heap);
	thread->heap->exec_start_time = 0;

out:
	qjsky_leave_thread(thread);
	return ret;
}


/* exported interface documented in js.h */
bool
js_fire_event(jsthread *thread,
	      const char *type,
	      struct dom_document *doc,
	      struct dom_node *target)
{
	/** \todo Dispatch to Window load handler once bindings exist */
	return true;
}


/* exported interface documented in js.h */
void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	/** \todo Register on* attribute handlers once bindings exist */
}


/* exported interface documented in js.h */
void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	/** \todo Drop the cached event object once bindings exist */
}
