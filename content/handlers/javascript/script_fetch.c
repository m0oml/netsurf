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
 * Requests go straight to the fetchers rather than through the cache,
 * as scripts need every response status, any method and their own
 * headers.  Redirects are followed here so the security checks can be
 * repeated for each URL.
 *
 * Fetch messages arrive while the fetchers are being polled, so each
 * step of a request (following a redirect, sending the request after a
 * preflight, reporting the response) is taken from the scheduler.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libwapcaplet/libwapcaplet.h>

#include "utils/utils.h"
#include "utils/ascii.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/nsoption.h"
#include "netsurf/misc.h"
#include "netsurf/url_db.h"
#include "desktop/gui_internal.h"
#include "content/fetch.h"
#include "content/urldb.h"

#include "javascript/script_fetch.h"

/** Largest response body kept for a script */
#define SCRIPT_FETCH_MAX_BODY (64 * 1024 * 1024)

/** Largest set of response headers kept for a script */
#define SCRIPT_FETCH_MAX_HEADERS (256 * 1024)

/**
 * A growable byte buffer.
 */
struct script_fetch_buf {
	uint8_t *data; /**< contents, NUL terminated when not empty */
	size_t len; /**< length of contents */
	size_t alloc; /**< allocated size */
};

/**
 * The stage a request has reached.
 */
enum script_fetch_phase {
	SCRIPT_FETCH_PREFLIGHT, /**< making a CORS preflight request */
	SCRIPT_FETCH_MAIN /**< making the request itself */
};

/**
 * What the last fetch for a request did.
 */
enum script_fetch_outcome {
	SCRIPT_FETCH_PENDING, /**< still fetching */
	SCRIPT_FETCH_FINISHED, /**< got a response */
	SCRIPT_FETCH_REDIRECTED, /**< got a redirect */
	SCRIPT_FETCH_FAILED /**< network error */
};

/**
 * A request made by a script.
 */
struct script_fetch {
	struct fetch *fetch; /**< the active fetch, or NULL */
	enum script_fetch_phase phase; /**< current stage */
	enum script_fetch_outcome outcome; /**< result of the last fetch */

	nsurl *document; /**< URL of the requesting document */
	char *origin; /**< serialised origin of the document */
	nsurl *url; /**< URL currently being fetched */
	char *method; /**< request method */
	char **headers; /**< request header lines, NULL terminated */
	bool has_content_type; /**< the script gave a Content-Type */
	bool simple; /**< request can be made cross origin without a preflight */
	char *preflight_headers; /**< lower case names of unsafe headers */
	uint8_t *body; /**< request body, or NULL */
	size_t body_len; /**< byte length of body */
	enum script_fetch_credentials credentials; /**< credentials mode */
	bool redirect_error; /**< redirects are errors */

	bool cors; /**< request has gone to another origin */
	bool tainted; /**< redirected between other origins; origin is null */
	unsigned int redirects; /**< number of redirects followed */

	int status; /**< status of the last response */
	char *status_text; /**< status message of the last response */
	char *location; /**< redirect target */
	struct script_fetch_buf header_buf; /**< raw response header lines */
	struct script_fetch_buf body_buf; /**< response body */
	const char *error; /**< reason for a network error */

	script_fetch_callback callback; /**< completion callback */
	void *pw; /**< private data for callback */
};


/**
 * Methods scripts may not use.
 */
static const char *script_fetch_forbidden_methods[] = {
	"CONNECT", "TRACE", "TRACK", NULL
};

/**
 * Request headers scripts may not set.
 */
static const char *script_fetch_forbidden_headers[] = {
	"Accept-Charset", "Accept-Encoding",
	"Access-Control-Request-Headers", "Access-Control-Request-Method",
	"Connection", "Content-Length", "Cookie", "Cookie2", "Date", "DNT",
	"Expect", "Host", "Keep-Alive", "Origin", "Referer", "Set-Cookie",
	"TE", "Trailer", "Transfer-Encoding", "Upgrade", "Via", NULL
};

/**
 * Response headers scripts may always see on a cross origin response.
 */
static const char *script_fetch_cors_safe_response_headers[] = {
	"cache-control", "content-language", "content-length",
	"content-type", "expires", "last-modified", "pragma", NULL
};


/**
 * Append to a buffer.
 *
 * \return false if out of memory or the limit is exceeded
 */
static bool
script_fetch_buf_append(struct script_fetch_buf *buf,
			const uint8_t *data, size_t len, size_t limit)
{
	if (len > limit || buf->len > limit - len) {
		return false;
	}
	if (buf->len + len + 1 > buf->alloc) {
		size_t alloc = buf->alloc == 0 ? 1024 : buf->alloc;
		uint8_t *n;

		while (alloc < buf->len + len + 1) {
			alloc *= 2;
		}
		n = realloc(buf->data, alloc);
		if (n == NULL) {
			return false;
		}
		buf->data = n;
		buf->alloc = alloc;
	}
	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return true;
}


/**
 * Empty a buffer.
 */
static void script_fetch_buf_reset(struct script_fetch_buf *buf)
{
	free(buf->data);
	buf->data = NULL;
	buf->len = 0;
	buf->alloc = 0;
}


/**
 * Test whether a string is in a NULL terminated list, ignoring case.
 */
static bool script_fetch_in_list(const char *s, const char **list)
{
	for (; *list != NULL; list++) {
		if (strcasecmp(s, *list) == 0) {
			return true;
		}
	}
	return false;
}


/**
 * Test whether a string is an HTTP token.
 */
static bool script_fetch_is_token(const char *s)
{
	if (*s == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		if (!(ascii_is_alphanumerical(*s) ||
		      strchr("!#$%&'*+-.^_`|~", *s) != NULL)) {
			return false;
		}
	}
	return true;
}


/**
 * Test whether a request header may be set by a script.
 */
static bool script_fetch_header_allowed(const char *name)
{
	if (script_fetch_in_list(name, script_fetch_forbidden_headers)) {
		return false;
	}
	if (strncasecmp(name, "Proxy-", 6) == 0 ||
	    strncasecmp(name, "Sec-", 4) == 0) {
		return false;
	}
	return true;
}


/**
 * Test whether a header value has a byte which is not CORS safe.
 */
static bool script_fetch_cors_unsafe_bytes(const char *value)
{
	const unsigned char *p;

	for (p = (const unsigned char *)value; *p != '\0'; p++) {
		if ((*p < 0x20 && *p != '\t') || *p == 0x7f ||
		    strchr("\"():<>?@[\\]{}", *p) != NULL) {
			return true;
		}
	}
	return false;
}


/**
 * Test whether a request header can be sent cross origin without a
 * preflight request.
 */
static bool script_fetch_header_cors_safe(const char *name, const char *value)
{
	const char *p;

	if (strlen(value) > 128) {
		return false;
	}

	if (strcasecmp(name, "Accept") == 0) {
		return !script_fetch_cors_unsafe_bytes(value);
	}

	if (strcasecmp(name, "Accept-Language") == 0 ||
	    strcasecmp(name, "Content-Language") == 0) {
		for (p = value; *p != '\0'; p++) {
			if (!(ascii_is_alphanumerical(*p) ||
			      strchr(" *,-.;=", *p) != NULL)) {
				return false;
			}
		}
		return true;
	}

	if (strcasecmp(name, "Content-Type") == 0) {
		size_t len;

		if (script_fetch_cors_unsafe_bytes(value)) {
			return false;
		}
		while (*value == ' ' || *value == '\t') {
			value++;
		}
		len = strcspn(value, ";");
		while (len > 0 && (value[len - 1] == ' ' ||
				   value[len - 1] == '\t')) {
			len--;
		}
		return (len == 33 && strncasecmp(value,
				"application/x-www-form-urlencoded", len) == 0) ||
			(len == 19 && strncasecmp(value,
				"multipart/form-data", len) == 0) ||
			(len == 10 && strncasecmp(value,
				"text/plain", len) == 0);
	}

	return false;
}


/**
 * Get the serialised origin of a URL.
 *
 * \return The origin, which the caller must free, or NULL on error
 */
static char *script_fetch_origin(nsurl *url)
{
	enum nsurl_scheme_type type = nsurl_get_scheme_type(url);
	char *origin;
	size_t len;

	if (type != NSURL_SCHEME_HTTP && type != NSURL_SCHEME_HTTPS) {
		return strdup("null");
	}
	if (nsurl_get(url, NSURL_SCHEME | NSURL_HOST | NSURL_PORT,
		      &origin, &len) != NSERROR_OK) {
		return NULL;
	}
	/* an origin has no path */
	while (len > 0 && origin[len - 1] == '/') {
		origin[--len] = '\0';
	}
	return origin;
}


/**
 * Test whether two URLs have the same origin.
 */
static bool script_fetch_same_origin(nsurl *a, nsurl *b)
{
	enum nsurl_scheme_type type = nsurl_get_scheme_type(a);

	if (type != NSURL_SCHEME_HTTP && type != NSURL_SCHEME_HTTPS) {
		/* other schemes have opaque origins */
		return false;
	}
	return nsurl_compare(a, b, NSURL_SCHEME | NSURL_HOST | NSURL_PORT);
}


/**
 * Check a URL may be fetched by the document, upgrading it for HSTS.
 *
 * \param sf The request
 * \param url The URL, which is replaced if upgraded
 * \return NULL if the URL may be fetched, otherwise the reason not
 */
static const char *script_fetch_check_url(struct script_fetch *sf, nsurl **url)
{
	enum nsurl_scheme_type type = nsurl_get_scheme_type(*url);

	if (type == NSURL_SCHEME_HTTP && urldb_get_hsts_enabled(*url)) {
		nsurl *upgraded;

		if (nsurl_replace_scheme(*url, corestring_lwc_https,
					 &upgraded) == NSERROR_OK) {
			nsurl_unref(*url);
			*url = upgraded;
			type = NSURL_SCHEME_HTTPS;
		}
	}

	switch (type) {
	case NSURL_SCHEME_HTTP:
		if (nsurl_get_scheme_type(sf->document) ==
		    NSURL_SCHEME_HTTPS) {
			return "mixed content: http request from an https page";
		}
		break;
	case NSURL_SCHEME_HTTPS:
	case NSURL_SCHEME_DATA:
		break;
	default:
		return "URL scheme not available to scripts";
	}

	if (nsurl_has_component(*url, NSURL_USERNAME) ||
	    nsurl_has_component(*url, NSURL_PASSWORD)) {
		if (!script_fetch_same_origin(sf->document, *url)) {
			return "cross origin URL with credentials";
		}
	}

	return NULL;
}


/**
 * Get a response header of the last fetch.
 *
 * Repeated headers are combined with ", ".
 *
 * \param sf The request
 * \param name The header name
 * \return The value, which the caller must free, or NULL if not present
 */
static char *script_fetch_get_header(struct script_fetch *sf, const char *name)
{
	const char *line = (const char *)sf->header_buf.data;
	size_t name_len = strlen(name);
	struct script_fetch_buf value = { NULL, 0, 0 };

	if (line == NULL) {
		return NULL;
	}

	while (*line != '\0') {
		size_t len = strcspn(line, "\r\n");

		if (len > name_len && line[name_len] == ':' &&
		    strncasecmp(line, name, name_len) == 0) {
			const char *v = line + name_len + 1;
			size_t vlen = len - name_len - 1;

			while (vlen > 0 && (*v == ' ' || *v == '\t')) {
				v++;
				vlen--;
			}
			while (vlen > 0 && (v[vlen - 1] == ' ' ||
					    v[vlen - 1] == '\t')) {
				vlen--;
			}
			if (value.len > 0) {
				script_fetch_buf_append(&value,
						(const uint8_t *)", ", 2,
						SCRIPT_FETCH_MAX_HEADERS);
			}
			script_fetch_buf_append(&value, (const uint8_t *)v,
					vlen, SCRIPT_FETCH_MAX_HEADERS);
			if (value.data == NULL) {
				/* present but empty */
				script_fetch_buf_append(&value,
						(const uint8_t *)"", 0,
						SCRIPT_FETCH_MAX_HEADERS);
			}
		}
		line += len;
		line += strspn(line, "\r\n");
	}

	return (char *)value.data;
}


/**
 * Test whether a token is in a comma separated header value.
 *
 * \param list The header value, or NULL
 * \param token The token to find
 * \param caseless Whether to ignore case
 */
static bool
script_fetch_list_has(const char *list, const char *token, bool caseless)
{
	size_t tlen = strlen(token);

	if (list == NULL) {
		return false;
	}
	while (*list != '\0') {
		size_t len;

		list += strspn(list, " \t,");
		len = strcspn(list, ",");
		while (len > 0 && (list[len - 1] == ' ' ||
				   list[len - 1] == '\t')) {
			len--;
		}
		if (len == tlen && (caseless ?
				    strncasecmp(list, token, len) == 0 :
				    strncmp(list, token, len) == 0)) {
			return true;
		}
		list += strcspn(list, ",");
	}
	return false;
}


/**
 * Whether credentials are sent with the current fetch of a request.
 */
static bool script_fetch_send_credentials(struct script_fetch *sf)
{
	return (sf->credentials == SCRIPT_FETCH_CREDENTIALS_INCLUDE) ||
		(sf->credentials == SCRIPT_FETCH_CREDENTIALS_SAME_ORIGIN &&
		 sf->cors == false);
}


/**
 * The origin sent with a request.
 */
static const char *script_fetch_request_origin(struct script_fetch *sf)
{
	return sf->tainted ? "null" : sf->origin;
}


/**
 * Make the CORS check of the last response.
 *
 * \return NULL if the response may be used, otherwise the reason not
 */
static const char *script_fetch_cors_check(struct script_fetch *sf)
{
	const char *origin = script_fetch_request_origin(sf);
	const char *error = NULL;
	char *acao;

	acao = script_fetch_get_header(sf, "Access-Control-Allow-Origin");
	if (acao == NULL) {
		return "CORS: no Access-Control-Allow-Origin";
	}

	if (strcmp(acao, "*") == 0 &&
	    sf->credentials != SCRIPT_FETCH_CREDENTIALS_INCLUDE) {
		free(acao);
		return NULL;
	}

	if (strcmp(acao, origin) != 0) {
		error = "CORS: Access-Control-Allow-Origin does not match";
	} else if (sf->credentials == SCRIPT_FETCH_CREDENTIALS_INCLUDE) {
		char *acac;

		acac = script_fetch_get_header(sf,
				"Access-Control-Allow-Credentials");
		if (acac == NULL || strcmp(acac, "true") != 0) {
			error = "CORS: credentials not allowed";
		}
		free(acac);
	}

	free(acao);
	return error;
}


/**
 * Check the response to a preflight request allows the request.
 *
 * \return NULL if the request may be made, otherwise the reason not
 */
static const char *script_fetch_preflight_check(struct script_fetch *sf)
{
	bool wild = sf->credentials != SCRIPT_FETCH_CREDENTIALS_INCLUDE;
	const char *error;
	char *allowed;
	const char *name;

	if (sf->status < 200 || sf->status > 299) {
		return "CORS: preflight request failed";
	}

	error = script_fetch_cors_check(sf);
	if (error != NULL) {
		return error;
	}

	if (strcmp(sf->method, "GET") != 0 &&
	    strcmp(sf->method, "HEAD") != 0 &&
	    strcmp(sf->method, "POST") != 0) {
		allowed = script_fetch_get_header(sf,
				"Access-Control-Allow-Methods");
		if (!script_fetch_list_has(allowed, sf->method, false) &&
		    !(wild && script_fetch_list_has(allowed, "*", false))) {
			error = "CORS: method not allowed";
		}
		free(allowed);
		if (error != NULL) {
			return error;
		}
	}

	if (sf->preflight_headers == NULL) {
		return NULL;
	}

	allowed = script_fetch_get_header(sf, "Access-Control-Allow-Headers");
	if (!(wild && script_fetch_list_has(allowed, "*", false))) {
		name = sf->preflight_headers;
		while (*name != '\0' && error == NULL) {
			size_t len = strcspn(name, ",");
			char *one = strndup(name, len);

			if (one == NULL ||
			    !script_fetch_list_has(allowed, one, true)) {
				error = "CORS: header not allowed";
			}
			free(one);
			name += len;
			name += strspn(name, ",");
		}
	}
	free(allowed);

	return error;
}


/**
 * Build the response headers a script may see.
 *
 * Names are made lower case.  Cookies are never exposed, and cross
 * origin responses only expose safe headers and those the server
 * lists in Access-Control-Expose-Headers.
 *
 * \return The headers, which the caller must free, or NULL on error
 */
static char *script_fetch_exposed_headers(struct script_fetch *sf)
{
	struct script_fetch_buf out = { NULL, 0, 0 };
	const char *line = (const char *)sf->header_buf.data;
	char *expose = NULL;
	bool expose_all = false;

	if (sf->cors) {
		expose = script_fetch_get_header(sf,
				"Access-Control-Expose-Headers");
		expose_all = (sf->credentials !=
			      SCRIPT_FETCH_CREDENTIALS_INCLUDE) &&
			script_fetch_list_has(expose, "*", false);
	}

	while (line != NULL && *line != '\0') {
		size_t len = strcspn(line, "\r\n");
		size_t name_len = strcspn(line, ":");

		if (name_len < len && name_len > 0 &&
		    strncmp(line, "HTTP/", 5) != 0) {
			char *name = strndup(line, name_len);
			bool show;

			if (name == NULL) {
				break;
			}
			show = strcasecmp(name, "Set-Cookie") != 0 &&
				strcasecmp(name, "Set-Cookie2") != 0;
			if (show && sf->cors && !expose_all) {
				show = script_fetch_in_list(name,
					script_fetch_cors_safe_response_headers) ||
					script_fetch_list_has(expose, name, true);
			}
			if (show) {
				const char *v = line + name_len + 1;
				size_t vlen = len - name_len - 1;
				size_t i;

				while (vlen > 0 && (*v == ' ' || *v == '\t')) {
					v++;
					vlen--;
				}
				for (i = 0; i < name_len; i++) {
					name[i] = ascii_to_lower(name[i]);
				}
				script_fetch_buf_append(&out,
						(const uint8_t *)name, name_len,
						SCRIPT_FETCH_MAX_HEADERS);
				script_fetch_buf_append(&out,
						(const uint8_t *)": ", 2,
						SCRIPT_FETCH_MAX_HEADERS);
				script_fetch_buf_append(&out,
						(const uint8_t *)v, vlen,
						SCRIPT_FETCH_MAX_HEADERS);
				script_fetch_buf_append(&out,
						(const uint8_t *)"\r\n", 2,
						SCRIPT_FETCH_MAX_HEADERS);
			}
			free(name);
		}
		line += len;
		line += strspn(line, "\r\n");
	}

	free(expose);

	if (out.data == NULL) {
		return strdup("");
	}
	return (char *)out.data;
}


/**
 * Free a request.
 */
static void script_fetch_free(struct script_fetch *sf)
{
	char **h;

	if (sf->headers != NULL) {
		for (h = sf->headers; *h != NULL; h++) {
			free(*h);
		}
		free(sf->headers);
	}
	if (sf->document != NULL) {
		nsurl_unref(sf->document);
	}
	if (sf->url != NULL) {
		nsurl_unref(sf->url);
	}
	free(sf->origin);
	free(sf->method);
	free(sf->preflight_headers);
	free(sf->body);
	free(sf->status_text);
	free(sf->location);
	script_fetch_buf_reset(&sf->header_buf);
	script_fetch_buf_reset(&sf->body_buf);
	free(sf);
}


static void script_fetch_step(void *p);


/**
 * Note the outcome of a fetch and continue from the scheduler.
 */
static void
script_fetch_done(struct script_fetch *sf, enum script_fetch_outcome outcome)
{
	sf->status = fetch_http_code(sf->fetch);
	sf->fetch = NULL; /* the fetchers free it */
	sf->outcome = outcome;
	guit->misc->schedule(0, script_fetch_step, sf);
}


/**
 * Note the status message of a response from its status line.
 */
static void
script_fetch_status_line(struct script_fetch *sf, const uint8_t *buf, size_t len)
{
	size_t i = 0;
	size_t end;

	/* HTTP/1.1 200 OK */
	while (i < len && buf[i] != ' ') {
		i++;
	}
	while (i < len && buf[i] == ' ') {
		i++;
	}
	while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') {
		i++;
	}
	while (i < len && buf[i] == ' ') {
		i++;
	}
	end = i;
	while (end < len && buf[end] != '\r' && buf[end] != '\n') {
		end++;
	}

	free(sf->status_text);
	sf->status_text = strndup((const char *)buf + i, end - i);
}


/**
 * Ignore messages from an aborted fetch.
 */
static void script_fetch_callback_ignore(const fetch_msg *msg, void *p)
{
}


/**
 * Handle a message from the fetchers.
 */
static void script_fetch_callback_fetch(const fetch_msg *msg, void *p)
{
	struct script_fetch *sf = p;

	switch (msg->type) {
	case FETCH_HEADER:
		if (msg->data.header_or_data.len > 5 &&
		    memcmp(msg->data.header_or_data.buf, "HTTP/", 5) == 0) {
			/* a new response, perhaps after 100 Continue */
			script_fetch_buf_reset(&sf->header_buf);
			script_fetch_status_line(sf,
					msg->data.header_or_data.buf,
					msg->data.header_or_data.len);
		}
		if (!script_fetch_buf_append(&sf->header_buf,
				msg->data.header_or_data.buf,
				msg->data.header_or_data.len,
				SCRIPT_FETCH_MAX_HEADERS)) {
			sf->error = "response headers too large";
			fetch_change_callback(sf->fetch,
					      script_fetch_callback_ignore, NULL);
			fetch_abort(sf->fetch);
			script_fetch_done(sf, SCRIPT_FETCH_FAILED);
		}
		break;

	case FETCH_DATA:
		if (sf->phase == SCRIPT_FETCH_MAIN &&
		    !script_fetch_buf_append(&sf->body_buf,
				msg->data.header_or_data.buf,
				msg->data.header_or_data.len,
				SCRIPT_FETCH_MAX_BODY)) {
			sf->error = "response too large";
			fetch_change_callback(sf->fetch,
					      script_fetch_callback_ignore, NULL);
			fetch_abort(sf->fetch);
			script_fetch_done(sf, SCRIPT_FETCH_FAILED);
		}
		break;

	case FETCH_FINISHED:
	case FETCH_NOTMODIFIED:
		script_fetch_done(sf, SCRIPT_FETCH_FINISHED);
		break;

	case FETCH_REDIRECT:
		free(sf->location);
		sf->location = strdup(msg->data.redirect);
		if (sf->location == NULL) {
			sf->error = "out of memory";
			script_fetch_done(sf, SCRIPT_FETCH_FAILED);
		} else {
			script_fetch_done(sf, SCRIPT_FETCH_REDIRECTED);
		}
		break;

	case FETCH_ERROR:
	case FETCH_TIMEDOUT:
	case FETCH_AUTH:
	case FETCH_CERT_ERR:
	case FETCH_SSL_ERR:
		NSLOG(netsurf, INFO, "script fetch of %s failed: %d",
		      nsurl_access(sf->url), msg->type);
		sf->error = "network error";
		script_fetch_done(sf, SCRIPT_FETCH_FAILED);
		break;

	case FETCH_PROGRESS:
	case FETCH_CERTS:
		break;
	}
}


/**
 * Make a Referer header line for the current fetch.
 *
 * \return The line, which the caller must free, or NULL for none
 */
static char *script_fetch_referer(struct script_fetch *sf)
{
	enum nsurl_scheme_type doc = nsurl_get_scheme_type(sf->document);
	enum nsurl_scheme_type url = nsurl_get_scheme_type(sf->url);
	char *referer;
	char *line;
	size_t len;

	if (!nsoption_bool(send_referer)) {
		return NULL;
	}
	/* never send an https referer over http */
	if (doc == NSURL_SCHEME_HTTPS && url != NSURL_SCHEME_HTTPS) {
		return NULL;
	}
	if (doc != NSURL_SCHEME_HTTP && doc != NSURL_SCHEME_HTTPS) {
		return NULL;
	}
	if (nsurl_get(sf->document, NSURL_SCHEME | NSURL_HOST | NSURL_PORT |
		      NSURL_PATH | NSURL_QUERY, &referer, &len) != NSERROR_OK) {
		return NULL;
	}
	line = malloc(len + sizeof "Referer: ");
	if (line != NULL) {
		snprintf(line, len + sizeof "Referer: ", "Referer: %s", referer);
	}
	free(referer);
	return line;
}


/**
 * Start the fetch for the current phase of a request.
 *
 * \return NSERROR_OK on success, or an error
 */
static nserror script_fetch_send(struct script_fetch *sf)
{
	struct fetch_request request;
	const char *headers[64];
	char *owned[4] = { NULL, NULL, NULL, NULL };
	unsigned int n = 0;
	unsigned int o = 0;
	nserror err = NSERROR_NOMEM;
	size_t len;
	char **h;

	script_fetch_buf_reset(&sf->header_buf);
	script_fetch_buf_reset(&sf->body_buf);
	free(sf->status_text);
	sf->status_text = NULL;
	sf->status = 0;
	sf->outcome = SCRIPT_FETCH_PENDING;

	memset(&request, 0, sizeof request);

	if (sf->phase == SCRIPT_FETCH_PREFLIGHT) {
		request.method = "OPTIONS";
		request.omit_credentials = true;

		len = strlen(sf->method) + 40;
		owned[o] = malloc(len);
		if (owned[o] == NULL) {
			goto out;
		}
		snprintf(owned[o], len, "Access-Control-Request-Method: %s",
			 sf->method);
		headers[n++] = owned[o++];

		if (sf->preflight_headers != NULL) {
			len = strlen(sf->preflight_headers) + 40;
			owned[o] = malloc(len);
			if (owned[o] == NULL) {
				goto out;
			}
			snprintf(owned[o], len,
				 "Access-Control-Request-Headers: %s",
				 sf->preflight_headers);
			headers[n++] = owned[o++];
		}
		headers[n++] = "Accept: */*";
	} else {
		request.method = sf->method;
		request.body = sf->body;
		request.body_len = sf->body_len;
		request.omit_credentials = !script_fetch_send_credentials(sf);

		for (h = sf->headers; *h != NULL; h++) {
			if (n >= NOF_ELEMENTS(headers) - 4) {
				err = NSERROR_INVALID;
				goto out;
			}
			headers[n++] = *h;
		}
		if (sf->body != NULL && !sf->has_content_type) {
			/* stop the fetcher adding a form content type */
			headers[n++] = "Content-Type:";
		}
	}

	if (sf->cors || sf->phase == SCRIPT_FETCH_PREFLIGHT ||
	    (strcmp(sf->method, "GET") != 0 &&
	     strcmp(sf->method, "HEAD") != 0)) {
		const char *origin = script_fetch_request_origin(sf);

		len = strlen(origin) + 10;
		owned[o] = malloc(len);
		if (owned[o] == NULL) {
			goto out;
		}
		snprintf(owned[o], len, "Origin: %s", origin);
		headers[n++] = owned[o++];
	}

	owned[o] = script_fetch_referer(sf);
	if (owned[o] != NULL) {
		headers[n++] = owned[o++];
	}

	headers[n] = NULL;

	NSLOG(netsurf, INFO, "script %s %s%s",
	      request.method, nsurl_access(sf->url),
	      sf->cors ? " (cross origin)" : "");

	err = fetch_start_request(sf->url, sf->document,
				  script_fetch_callback_fetch, sf,
				  &request, headers, &sf->fetch);

out:
	for (n = 0; n < NOF_ELEMENTS(owned); n++) {
		free(owned[n]);
	}
	return err;
}


/**
 * Report the result of a request to its owner and free it.
 */
static void script_fetch_finish(struct script_fetch *sf)
{
	struct script_fetch_response response;
	char *headers = NULL;

	memset(&response, 0, sizeof response);
	response.url = sf->url;
	response.redirected = sf->redirects > 0;

	if (sf->error == NULL) {
		headers = script_fetch_exposed_headers(sf);
		if (headers == NULL) {
			sf->error = "out of memory";
		}
	}

	if (sf->error != NULL) {
		NSLOG(netsurf, INFO, "script fetch of %s: %s",
		      nsurl_access(sf->url), sf->error);
		response.error = sf->error;
		response.headers = "";
	} else {
		response.status = sf->status;
		if (response.status == 0) {
			/* not http */
			response.status = 200;
		}
		response.status_text = sf->status_text;
		response.headers = headers;
		response.body = sf->body_buf.data;
		response.body_len = sf->body_buf.len;
	}
	if (response.status_text == NULL) {
		response.status_text = response.status == 200 ? "OK" : "";
	}

	sf->callback(&response, sf->pw);

	free(headers);
	script_fetch_free(sf);
}


/**
 * Follow a redirect.
 *
 * \return NULL on success, otherwise the reason for failure
 */
static const char *script_fetch_redirect(struct script_fetch *sf)
{
	const char *error;
	nsurl *next;
	bool cross;

	if (sf->redirect_error) {
		return "redirect not allowed";
	}
	if (++sf->redirects > 20) {
		return "too many redirects";
	}
	if (sf->cors && (error = script_fetch_cors_check(sf)) != NULL) {
		return error;
	}

	if (nsurl_join(sf->url, sf->location, &next) != NSERROR_OK) {
		return "bad redirect URL";
	}
	error = script_fetch_check_url(sf, &next);
	if (error != NULL) {
		nsurl_unref(next);
		return error;
	}

	/* a redirect between two other origins hides the origin */
	if (sf->cors && !script_fetch_same_origin(sf->url, next)) {
		sf->tainted = true;
	}

	cross = !script_fetch_same_origin(sf->document, next) &&
		nsurl_get_scheme_type(next) != NSURL_SCHEME_DATA;

	/* POST becomes GET after 301 and 302, anything but HEAD after 303 */
	if (((sf->status == 301 || sf->status == 302) &&
	     strcmp(sf->method, "POST") == 0) ||
	    (sf->status == 303 && strcmp(sf->method, "HEAD") != 0)) {
		char **h;
		char **keep;

		free(sf->method);
		sf->method = strdup("GET");
		free(sf->body);
		sf->body = NULL;
		sf->body_len = 0;
		sf->has_content_type = false;

		/* drop the headers describing the body */
		for (h = keep = sf->headers; *h != NULL; h++) {
			if (strncasecmp(*h, "Content-", 8) == 0) {
				free(*h);
			} else {
				*keep++ = *h;
			}
		}
		*keep = NULL;
		if (sf->method == NULL) {
			nsurl_unref(next);
			return "out of memory";
		}
	}

	if (cross && !sf->cors) {
		/* now going cross origin, which needs a simple request
		 * since there is no preflight after a redirect
		 */
		if (!sf->simple) {
			nsurl_unref(next);
			return "CORS: redirected to another origin";
		}
		sf->cors = true;
	}

	nsurl_unref(sf->url);
	sf->url = next;

	if (script_fetch_send(sf) != NSERROR_OK) {
		return "network error";
	}
	return NULL;
}


/**
 * Take the next step of a request after a fetch ends.
 */
static void script_fetch_step(void *p)
{
	struct script_fetch *sf = p;
	const char *error = NULL;

	switch (sf->outcome) {
	case SCRIPT_FETCH_PENDING:
		return;

	case SCRIPT_FETCH_FAILED:
		break;

	case SCRIPT_FETCH_REDIRECTED:
		if (sf->phase == SCRIPT_FETCH_PREFLIGHT) {
			error = "CORS: preflight request redirected";
		} else {
			error = script_fetch_redirect(sf);
			if (error == NULL) {
				return;
			}
		}
		break;

	case SCRIPT_FETCH_FINISHED:
		if (sf->phase == SCRIPT_FETCH_PREFLIGHT) {
			error = script_fetch_preflight_check(sf);
			if (error == NULL) {
				sf->phase = SCRIPT_FETCH_MAIN;
				if (script_fetch_send(sf) == NSERROR_OK) {
					return;
				}
				error = "network error";
			}
		} else if (sf->cors) {
			error = script_fetch_cors_check(sf);
		}
		break;
	}

	if (error != NULL) {
		sf->error = error;
	}
	script_fetch_finish(sf);
}


/**
 * Fail a request from the scheduler.
 */
static nserror script_fetch_fail(struct script_fetch *sf, const char *error)
{
	sf->error = error;
	sf->outcome = SCRIPT_FETCH_FAILED;
	guit->misc->schedule(0, script_fetch_step, sf);
	return NSERROR_OK;
}


/**
 * Add the unsafe header names to the list sent in a preflight request.
 *
 * The list is lower case, sorted and without duplicates.
 */
static bool script_fetch_add_preflight_header(struct script_fetch *sf, const char *name)
{
	char *lower = strdup(name);
	const char *list = sf->preflight_headers;
	struct script_fetch_buf out = { NULL, 0, 0 };
	bool added = false;
	size_t i;

	if (lower == NULL) {
		return false;
	}
	for (i = 0; lower[i] != '\0'; i++) {
		lower[i] = ascii_to_lower(lower[i]);
	}

	while (list != NULL && *list != '\0') {
		size_t len = strcspn(list, ",");
		int cmp = strncmp(list, lower, len);

		if (cmp == 0 && lower[len] == '\0') {
			/* already listed */
			free(lower);
			script_fetch_buf_reset(&out);
			return true;
		}
		if (!added && (cmp > 0 || (cmp == 0 && lower[len] < list[len]))) {
			if (out.len > 0) {
				script_fetch_buf_append(&out, (const uint8_t *)",", 1, SIZE_MAX);
			}
			script_fetch_buf_append(&out, (const uint8_t *)lower,
						strlen(lower), SIZE_MAX);
			added = true;
		}
		if (out.len > 0) {
			script_fetch_buf_append(&out, (const uint8_t *)",", 1, SIZE_MAX);
		}
		script_fetch_buf_append(&out, (const uint8_t *)list, len, SIZE_MAX);
		list += len;
		list += strspn(list, ",");
	}
	if (!added) {
		if (out.len > 0) {
			script_fetch_buf_append(&out, (const uint8_t *)",", 1, SIZE_MAX);
		}
		script_fetch_buf_append(&out, (const uint8_t *)lower,
					strlen(lower), SIZE_MAX);
	}
	free(lower);

	if (out.data == NULL) {
		return false;
	}
	free(sf->preflight_headers);
	sf->preflight_headers = (char *)out.data;
	return true;
}


/**
 * Copy and check the request headers given by a script.
 *
 * Forbidden headers are dropped.
 *
 * \return NULL on success, otherwise the reason for failure
 */
static const char *
script_fetch_set_headers(struct script_fetch *sf, const char **headers)
{
	size_t count = 0;
	size_t n = 0;
	size_t i;

	while (headers != NULL && headers[count] != NULL &&
	       headers[count + 1] != NULL) {
		count += 2;
	}

	sf->headers = calloc(count / 2 + 1, sizeof(char *));
	if (sf->headers == NULL) {
		return "out of memory";
	}

	for (i = 0; i < count; i += 2) {
		const char *name = headers[i];
		const char *value = headers[i + 1];
		size_t vlen;
		size_t len;

		if (!script_fetch_is_token(name) ||
		    strpbrk(value, "\r\n") != NULL) {
			return "invalid header";
		}
		if (!script_fetch_header_allowed(name)) {
			NSLOG(netsurf, INFO, "dropping forbidden header %s",
			      name);
			continue;
		}

		while (*value == ' ' || *value == '\t') {
			value++;
		}
		vlen = strlen(value);
		while (vlen > 0 && (value[vlen - 1] == ' ' ||
				    value[vlen - 1] == '\t')) {
			vlen--;
		}

		len = strlen(name) + vlen + 3;
		sf->headers[n] = malloc(len);
		if (sf->headers[n] == NULL) {
			return "out of memory";
		}
		if (vlen == 0) {
			/* "Name:" would remove a header so the fetcher
			 * takes "Name;" as an empty header
			 */
			snprintf(sf->headers[n], len, "%s;", name);
		} else {
			snprintf(sf->headers[n], len, "%s: %.*s",
				 name, (int)vlen, value);
		}
		n++;

		if (strcasecmp(name, "Content-Type") == 0) {
			sf->has_content_type = true;
		}
		if (!script_fetch_header_cors_safe(name, value)) {
			sf->simple = false;
			if (!script_fetch_add_preflight_header(sf, name)) {
				return "out of memory";
			}
		}
	}

	return NULL;
}


/* exported interface documented in javascript/script_fetch.h */
nserror
script_fetch_start(const struct script_fetch_request *request,
		   script_fetch_callback callback,
		   void *pw,
		   struct script_fetch **fetch_out)
{
	struct script_fetch *sf;
	const char *error;
	size_t i;

	sf = calloc(1, sizeof *sf);
	if (sf == NULL) {
		return NSERROR_NOMEM;
	}
	sf->callback = callback;
	sf->pw = pw;
	sf->document = nsurl_ref(request->document);
	sf->url = nsurl_ref(request->url);
	sf->credentials = request->credentials;
	sf->redirect_error = request->redirect_error;
	sf->simple = true;
	sf->origin = script_fetch_origin(request->document);
	sf->method = strdup(request->method);
	if (sf->origin == NULL || sf->method == NULL) {
		script_fetch_free(sf);
		return NSERROR_NOMEM;
	}

	if (request->body != NULL) {
		sf->body = malloc(request->body_len + 1);
		if (sf->body == NULL) {
			script_fetch_free(sf);
			return NSERROR_NOMEM;
		}
		memcpy(sf->body, request->body, request->body_len);
		sf->body_len = request->body_len;
	}

	*fetch_out = sf;

	/* methods are case sensitive apart from the standard ones */
	if (!script_fetch_is_token(sf->method)) {
		return script_fetch_fail(sf, "invalid method");
	}
	if (script_fetch_in_list(sf->method, script_fetch_forbidden_methods)) {
		return script_fetch_fail(sf, "forbidden method");
	}
	if (script_fetch_in_list(sf->method, (const char *[]) {
			"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT",
			"PATCH", NULL })) {
		for (i = 0; sf->method[i] != '\0'; i++) {
			sf->method[i] = ascii_to_upper(sf->method[i]);
		}
	}
	if (strcmp(sf->method, "GET") != 0 &&
	    strcmp(sf->method, "HEAD") != 0 &&
	    strcmp(sf->method, "POST") != 0) {
		sf->simple = false;
	}
	if (sf->body != NULL && (strcmp(sf->method, "GET") == 0 ||
				 strcmp(sf->method, "HEAD") == 0)) {
		return script_fetch_fail(sf, "GET and HEAD requests have no body");
	}

	error = script_fetch_set_headers(sf, request->headers);
	if (error != NULL) {
		return script_fetch_fail(sf, error);
	}

	error = script_fetch_check_url(sf, &sf->url);
	if (error != NULL) {
		return script_fetch_fail(sf, error);
	}

	sf->cors = !script_fetch_same_origin(sf->document, sf->url) &&
		nsurl_get_scheme_type(sf->url) != NSURL_SCHEME_DATA;

	sf->phase = (sf->cors && !sf->simple) ?
		SCRIPT_FETCH_PREFLIGHT : SCRIPT_FETCH_MAIN;

	if (script_fetch_send(sf) != NSERROR_OK) {
		return script_fetch_fail(sf, "network error");
	}

	return NSERROR_OK;
}


/* exported interface documented in javascript/script_fetch.h */
void script_fetch_abort(struct script_fetch *sf)
{
	guit->misc->schedule(-1, script_fetch_step, sf);
	if (sf->fetch != NULL) {
		fetch_change_callback(sf->fetch,
				      script_fetch_callback_ignore, NULL);
		fetch_abort(sf->fetch);
		sf->fetch = NULL;
	}
	script_fetch_free(sf);
}
