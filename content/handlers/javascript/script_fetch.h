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
 * Network requests made by scripts (fetch and XMLHttpRequest).
 *
 * The security checks of the fetch standard are made here, outside the
 * reach of page scripts: forbidden methods and headers, mixed content,
 * CORS (including preflight requests) and the credentials mode.
 */

#ifndef NETSURF_JAVASCRIPT_SCRIPT_FETCH_H
#define NETSURF_JAVASCRIPT_SCRIPT_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/errors.h"

struct nsurl;
struct script_fetch;

/**
 * When a request includes cookies and HTTP authentication.
 */
enum script_fetch_credentials {
	SCRIPT_FETCH_CREDENTIALS_OMIT, /**< never */
	SCRIPT_FETCH_CREDENTIALS_SAME_ORIGIN, /**< for same origin requests */
	SCRIPT_FETCH_CREDENTIALS_INCLUDE /**< always */
};

/**
 * A request made by a script.
 */
struct script_fetch_request {
	/** URL of the document making the request, giving its origin */
	struct nsurl *document;
	/** Request method */
	const char *method;
	/** URL to fetch */
	struct nsurl *url;
	/** Request header names and values, alternating, NULL terminated */
	const char **headers;
	/** Request body, or NULL for none */
	const uint8_t *body;
	/** Byte length of body */
	size_t body_len;
	/** Credentials mode */
	enum script_fetch_credentials credentials;
	/** Fail if the response is a redirect, instead of following it */
	bool redirect_error;
};

/**
 * The response to a request made by a script.
 *
 * The contents are only valid for the duration of the callback.
 */
struct script_fetch_response {
	/** Reason for a network error, or NULL on success */
	const char *error;
	/** Response status */
	int status;
	/** Response status message */
	const char *status_text;
	/** Final URL, after redirects */
	struct nsurl *url;
	/** Whether any redirects were followed */
	bool redirected;
	/** Response headers the script may see, as "name: value\r\n" lines */
	const char *headers;
	/** Response body */
	const uint8_t *body;
	/** Byte length of body */
	size_t body_len;
};

/**
 * Callback for completion of a request made by a script.
 *
 * Called once, from the scheduler, after which the request is freed.
 *
 * \param response The response, or the error if the request failed
 * \param pw The private data passed to script_fetch_start()
 */
typedef void (*script_fetch_callback)(const struct script_fetch_response *response, void *pw);

/**
 * Start a request made by a script.
 *
 * Requests which are refused immediately, for example for a forbidden
 * method, still complete through the callback with an error.
 *
 * \param request The request
 * \param callback Called when the request completes
 * \param pw Private data for callback
 * \param fetch_out Updated with the request, which may be aborted
 *                  until the callback is called
 * \return NSERROR_OK on success, otherwise an error and no callback
 *         will be made
 */
nserror script_fetch_start(const struct script_fetch_request *request, script_fetch_callback callback, void *pw, struct script_fetch **fetch_out);

/**
 * Abort a request made by a script.
 *
 * The callback is not called and the request is freed.
 *
 * \param sf The request
 */
void script_fetch_abort(struct script_fetch *sf);

#endif
