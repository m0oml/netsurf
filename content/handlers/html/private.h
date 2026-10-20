/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
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
 * Private data for text/html content.
 */

#ifndef NETSURF_HTML_PRIVATE_H
#define NETSURF_HTML_PRIVATE_H

#include <dom/bindings/hubbub/parser.h>

#include "netsurf/types.h"
#include "content/content_protected.h"
#include "content/handlers/css/utils.h"


struct gui_layout_table;
struct scrollbar_msg_data;
struct content_redraw_data;
struct selection;
struct hlcache_handle;

typedef enum {
	HTML_DRAG_NONE,			/** No drag */
	HTML_DRAG_SELECTION,		/** Own; Text selection */
	HTML_DRAG_SCROLLBAR,		/** Not own; drag in scrollbar widget */
	HTML_DRAG_TEXTAREA_SELECTION,	/** Not own; drag in textarea widget */
	HTML_DRAG_TEXTAREA_SCROLLBAR,	/** Not own; drag in textarea widget */
	HTML_DRAG_CONTENT_SELECTION,	/** Not own; drag in child content */
	HTML_DRAG_CONTENT_SCROLL	/** Not own; drag in child content */
} html_drag_type;

/**
 * For drags we don't own
 */
union html_drag_owner {
	bool no_owner;
	struct box *content;
	struct scrollbar *scrollbar;
	struct box *textarea;
};

typedef enum {
	HTML_SELECTION_NONE,		/** No selection */
	HTML_SELECTION_TEXTAREA,	/** Selection in one of our textareas */
	HTML_SELECTION_SELF,		/** Selection in this html content */
	HTML_SELECTION_CONTENT		/** Selection in child content */
} html_selection_type;

/**
 * For getting at selections in this content or things in this content
 */
union html_selection_owner {
	bool none;
	struct box *textarea;
	struct box *content;
};

typedef enum {
	HTML_FOCUS_SELF,		/**< Focus is our own */
	HTML_FOCUS_CONTENT,		/**< Focus belongs to child content */
	HTML_FOCUS_TEXTAREA		/**< Focus belongs to textarea */
} html_focus_type;

/**
 * For directing input
 */
union html_focus_owner {
	bool self;
	struct box *textarea;
	struct box *content;
};

/**
 * Data specific to CONTENT_HTML.
 */
typedef struct html_content {
	struct content base;

	dom_hubbub_parser *parser; /**< Parser object handle */
	bool parse_completed; /**< Whether the parse has been completed */
	bool conversion_begun; /**< Whether or not the conversion has begun */

	/** Document tree */
	dom_document *document;
	/** Quirkyness of document */
	dom_document_quirks_mode quirks;

	/** Encoding of source, NULL if unknown. */
	char *encoding;
	/** Source of encoding information. */
	dom_hubbub_encoding_source encoding_source;

	/** Base URL (may be a copy of content->url). */
	struct nsurl *base_url;
	/** Base target */
	char *base_target;

	/** Content has been aborted in the LOADING state */
	bool aborted;

	/** Whether a meta refresh has been handled */
	bool refresh;

	/** Whether a layout (reflow) is in progress */
	bool reflowing;

	/** Whether an initial layout has been done */
	bool had_initial_layout;

	/** Whether scripts are enabled for this content */
	bool enable_scripting;

	/* Title element node */
	dom_node *title;

	/** A talloc context purely for the render box tree */
	int *bctx;
	/** A context pointer for the box conversion, NULL if no conversion
	 * is in progress.
	 */
	void *box_conversion_context;
	/** Box tree, or NULL. */
	struct box *layout;
	/** Whether a rebuild of the box tree is scheduled */
	bool rebuild_pending;
	/** Whether the box tree is being rebuilt */
	bool rebuilding;
	/**
	 * Whether the box tree is an early one, built before the document
	 * finished loading so scripts could ask for layout information.
	 */
	bool early_layout;
	/** Whether the document has changed since the early box tree */
	bool early_layout_stale;
	/** Whether the early box tree is being built */
	bool early_layout_building;
	/** Whether the forms have been retrieved from the document */
	bool forms_retrieved;
	/** Whether the load event has been fired */
	bool load_fired;
	/**
	 * Number of operations in progress which hold box pointers while
	 * running scripts.  The box tree is not rebuilt synchronously,
	 * for scripts asking for layout information, while non zero.
	 */
	unsigned int box_pins;
	/** Document background colour. */
	colour background_colour;

	/** Font callback table */
	const struct gui_layout_table *font_func;

	/** Number of entries in scripts */
	unsigned int scripts_count;
	/** Scripts */
	struct html_script *scripts;
	/** javascript thread in use */
	struct jsthread *jsthread;

	/** Number of entries in stylesheet_content. */
	unsigned int stylesheet_count;
	/** Stylesheets. Each may be NULL. */
	struct html_stylesheet *stylesheets;
	/**< Style selection context */
	css_select_ctx *select_ctx;
	/**< Style selection media specification */
	css_media media;
	/** CSS length conversion context for document. */
	css_unit_ctx unit_len_ctx;
	/**< Universal selector */
	lwc_string *universal;

	/** Number of entries in object_list. */
	unsigned int num_objects;
	/** List of objects. */
	struct content_html_object *object_list;
	/** Forms, in reverse order to document. */
	struct form *forms;
	/** Hash table of imagemaps. */
	struct imagemap **imagemaps;

	/** Browser window containing this document, or NULL if not open. */
	struct browser_window *bw;

	/** Frameset information */
	struct content_html_frames *frameset;

	/** Inline frame information */
	struct content_html_iframe *iframe;

	/** Content of type CONTENT_HTML containing this, or NULL if not an
	 * object within a page. */
	struct html_content *page;

	/** Current drag type */
	html_drag_type drag_type;
	/** Widget capturing all mouse events */
	union html_drag_owner drag_owner;

	/** Current selection state */
	html_selection_type selection_type;
	/** Current selection owner */
	union html_selection_owner selection_owner;

	/** Current input focus target type */
	html_focus_type focus_type;
	/** Current input focus target */
	union html_focus_owner focus_owner;

	/** HTML content's own text selection object */
	struct selection *sel;

	/**
	 * Open core-handled form SELECT menu, or NULL if none
	 *  currently open.
	 */
	struct form_control *visible_select_menu;

} html_content;

/**
 * Render padding and margin box outlines in html_redraw().
 */
extern bool html_redraw_debug;


/* in html/html.c */

/**
 * redraw a box
 *
 * \param htmlc HTML content
 * \param box The box to redraw.
 */
void html__redraw_a_box(html_content *htmlc, struct box *box);


/**
 * Complete conversion of an HTML document
 *
 * \param htmlc Content to convert
 */
void html_finish_conversion(html_content *htmlc);


/**
 * Lay out a document before it has finished loading.
 *
 * Scripts which run once the document has been parsed, such as deferred
 * scripts and DOMContentLoaded handlers, can ask for layout information
 * before the box tree is built.  An early box tree is built and laid out
 * for them, without fetching objects.  It is replaced by the box tree
 * built when the document has loaded.
 *
 * \param htmlc Content to lay out
 * \return true if the content has a box tree which has been laid out
 */
bool html_early_layout(html_content *htmlc);


/**
 * Mark a document as done, firing its load event first.
 *
 * \param htmlc Content which has finished loading
 */
void html_set_done(html_content *htmlc);


/* in html/rebuild.c */

/**
 * Detach a box tree from the document so it can be freed.
 *
 * Nodes and form controls no longer refer to the boxes, and styles
 * libcss has cached for the nodes are discarded.
 *
 * \param htmlc HTML content the box tree belongs to
 * \param layout The root of the box tree
 */
void html_rebuild_detach(html_content *htmlc, struct box *layout);


/**
 * Note a change to the document which may need the box tree rebuilt.
 *
 * Changes after the box tree has been built schedule a rebuild of the
 * box tree, which is coalesced with any other changes made before it
 * runs.
 *
 * \param htmlc HTML content whose document changed
 * \param node The node which changed
 */
void html_dom_changed(html_content *htmlc, struct dom_node *node);


/**
 * Rebuild the box tree now if a rebuild is pending.
 *
 * Used when up to date layout is needed, such as when a script asks for
 * the position of an element.
 *
 * \param htmlc HTML content to update
 */
void html_rebuild_flush(html_content *htmlc);


/**
 * Cancel any pending rebuild of the box tree.
 *
 * \param htmlc HTML content being destroyed
 */
void html_rebuild_cancel(html_content *htmlc);


/* in html/layout_query.c */

/**
 * Position and size of an element, in css px.
 */
struct html_layout_geometry {
	int x; /**< left of the border box in the document */
	int y; /**< top of the border box in the document */
	int width; /**< width of the border box */
	int height; /**< height of the border box */
	int client_width; /**< width of the padding box */
	int client_height; /**< height of the padding box */
	int scroll_width; /**< width of the content and its overflow */
	int scroll_height; /**< height of the content and its overflow */
};

/**
 * Get the position and size of an element.
 *
 * \param htmlc HTML content the element belongs to
 * \param node The element
 * \param geometry Updated with the element's geometry
 * \return true on success, false if the element is not displayed
 */
bool html_layout_node_geometry(html_content *htmlc, struct dom_node *node, struct html_layout_geometry *geometry);

/**
 * Get the size and scroll position of the viewport, in css px.
 *
 * Before the content has been laid out the size is that of the
 * browser window and the scroll position is zero.
 */
void html_layout_viewport(html_content *htmlc, int *width, int *height, int *scroll_x, int *scroll_y);

/**
 * Get the computed value of a css property of an element.
 *
 * Only commonly used properties are supported.  Sizes, margins,
 * padding and borders are used values from the layout.
 *
 * \param htmlc HTML content the element belongs to
 * \param node The element
 * \param property The css property name
 * \return The value, which the caller must free, or NULL if unknown
 */
char *html_layout_computed_style(html_content *htmlc, struct dom_node *node, const char *property);

/**
 * Get the size to draw a background image at, from background-size.
 *
 * \param style        Style giving the background-size
 * \param unit_len_ctx Length conversion context
 * \param area_width   Width of the background positioning area
 * \param area_height  Height of the background positioning area
 * \param image        The background image
 * \param width        Updated to the width to draw the image at
 * \param height       Updated to the height to draw the image at
 */
void html_background_image_size(const css_computed_style *style, const css_unit_ctx *unit_len_ctx, int area_width, int area_height, struct hlcache_handle *image, int *width, int *height);


/**
 * Test if an HTML content conversion can begin
 *
 * \param htmlc		html content to test
 * \return true iff the html content conversion can begin
 */
bool html_can_begin_conversion(html_content *htmlc);


/**
 * Begin conversion of an HTML document
 *
 * \param htmlc Content to convert
 */
bool html_begin_conversion(html_content *htmlc);


/**
 * execute some text as a script element
 */
bool html_exec(struct content *c, const char *src, size_t srclen);


/**
 * Attempt script execution for defer and async scripts
 *
 * execute scripts using algorithm found in:
 * http://www.whatwg.org/specs/web-apps/current-work/multipage/scripting-1.html#the-script-element
 *
 * \param htmlc html content.
 * \param allow_defer allow deferred execution, if not, only async scripts.
 * \return NSERROR_OK error code.
 */
nserror html_script_exec(html_content *htmlc, bool allow_defer);


/**
 * Free all script resources and references for a html content.
 *
 * \param htmlc html content.
 * \return NSERROR_OK or error code.
 */
nserror html_script_free(html_content *htmlc);


/**
 * Check if any of the scripts loaded were insecure
 */
bool html_saw_insecure_scripts(html_content *htmlc);


/**
 * Complete the HTML content state machine *iff* all scripts are finished
 */
nserror html_proceed_to_done(html_content *html);


/* in html/redraw.c */
bool html_redraw(struct content *c, struct content_redraw_data *data,
		const struct rect *clip, const struct redraw_context *ctx);


/* in html/redraw_border.c */
bool html_redraw_borders(struct box *box, int x_parent, int y_parent,
		int p_width, int p_height, const struct rect *clip, float scale,
		const struct redraw_context *ctx);


bool html_redraw_inline_borders(struct box *box, struct rect b,
		const struct rect *clip, float scale, bool first, bool last,
		const struct redraw_context *ctx);


/* in html/script.c */
dom_hubbub_error html_process_script(void *ctx, dom_node *node);


/* in html/forms.c */
struct form *html_forms_get_forms(const char *docenc, dom_html_document *doc);
struct form_control *html_forms_get_control_for_node(struct form *forms,
		dom_node *node);


/* in html/css_fetcher.c */
/**
 * Register the fetcher for the pseudo x-ns-css scheme.
 *
 * \return NSERROR_OK on successful registration or error code on failure.
 */
nserror html_css_fetcher_register(void);
nserror html_css_fetcher_add_item(dom_string *data, struct nsurl *base_url,
		uint32_t *key);


/* Events */
/**
 * Construct an event and fire it at the DOM
 *
 */
bool fire_generic_dom_event(dom_string *type, dom_node *target,
		    bool bubbles, bool cancelable);

/**
 * Construct a keyboard event and fire it at the DOM
 */
bool fire_dom_keyboard_event(dom_string *type, dom_node *target,
		bool bubbles, bool cancelable, uint32_t key);

/* Useful dom_string pointers */
struct dom_string;

extern struct dom_string *html_dom_string_map;
extern struct dom_string *html_dom_string_id;
extern struct dom_string *html_dom_string_name;
extern struct dom_string *html_dom_string_area;
extern struct dom_string *html_dom_string_a;
extern struct dom_string *html_dom_string_nohref;
extern struct dom_string *html_dom_string_href;
extern struct dom_string *html_dom_string_target;
extern struct dom_string *html_dom_string_shape;
extern struct dom_string *html_dom_string_default;
extern struct dom_string *html_dom_string_rect;
extern struct dom_string *html_dom_string_rectangle;
extern struct dom_string *html_dom_string_coords;
extern struct dom_string *html_dom_string_circle;
extern struct dom_string *html_dom_string_poly;
extern struct dom_string *html_dom_string_polygon;
extern struct dom_string *html_dom_string_text_javascript;
extern struct dom_string *html_dom_string_type;
extern struct dom_string *html_dom_string_src;

#endif
