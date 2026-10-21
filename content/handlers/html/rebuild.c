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
 * Rebuilding the box tree of an HTML content when its document changes.
 *
 * The box tree is built from the document once it has loaded.  Changes
 * made to the document after that, usually by scripts, schedule a
 * rebuild of the whole box tree.  Changes made before the rebuild runs
 * are coalesced into it, so a script making many changes causes one
 * rebuild once it returns.
 *
 * The new tree is built completely before the old one is freed.  State
 * which refers to boxes is moved to the new tree where it can be: form
 * controls, iframes and the focused textarea.  Selections, drags and
 * text searches are cleared.
 */

#include <stdbool.h>
#include <dom/dom.h>
#include <nsutils/time.h>

#include "utils/talloc.h"
#include "utils/log.h"
#include "utils/corestrings.h"
#include "netsurf/misc.h"
#include "content/content_protected.h"
#include "content/textsearch.h"
#include "css/select.h"
#include "desktop/gui_internal.h"
#include "desktop/selection.h"
#include "desktop/textarea.h"
#include "desktop/frames.h"

#include "html/private.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/form_internal.h"
#include "html/imagemap.h"
#include "html/interaction.h"
#include "html/object.h"

/** Delay before a rebuild, so changes in quick succession are coalesced */
#define HTML_REBUILD_DELAY_MS 10

/** Result of the synchronous box construction */
static bool html_rebuild_success;


/**
 * Visit a node and every node below it.
 *
 * \param root The node to start from
 * \param visit Function called for each node
 */
static void
html_rebuild_walk_dom(dom_node *root, void (*visit)(dom_node *node))
{
	dom_node *node = dom_node_ref(root);

	for (;;) {
		dom_node *next = NULL;
		dom_exception exc;

		visit(node);

		exc = dom_node_get_first_child(node, &next);
		if ((exc == DOM_NO_ERR) && (next != NULL)) {
			dom_node_unref(node);
			node = next;
			continue;
		}

		/* no children, move to the next sibling of the node or of
		 * its nearest ancestor which has one
		 */
		while (node != root) {
			dom_node *parent = NULL;

			exc = dom_node_get_next_sibling(node, &next);
			if ((exc == DOM_NO_ERR) && (next != NULL)) {
				break;
			}
			next = NULL;

			exc = dom_node_get_parent_node(node, &parent);
			dom_node_unref(node);
			if ((exc != DOM_NO_ERR) || (parent == NULL)) {
				return;
			}
			node = parent;
		}

		dom_node_unref(node);
		if (next == NULL) {
			return;
		}
		node = next;
	}
}


/**
 * Forget the box of a node.
 */
static void html_rebuild_clear_node_box(dom_node *node)
{
	struct box *old_box;

	dom_node_set_user_data(node, corestring_dom___ns_key_box_node_data,
			       NULL, NULL, (void *)&old_box);
}


/**
 * Unlink a box tree from the document and form controls.
 *
 * Nodes are only unlinked from boxes of this tree, as nodes which have
 * been moved may already refer to a box of another tree.
 *
 * \param box The root of the box tree
 */
static void html_rebuild_unlink_boxes(struct box *box)
{
	for (; box != NULL; box = box->next) {
		if (box->node != NULL) {
			struct box *mapped = NULL;
			dom_exception exc;

			exc = dom_node_get_user_data(box->node,
					corestring_dom___ns_key_box_node_data,
					(void *)&mapped);
			if ((exc == DOM_NO_ERR) && (mapped == box)) {
				html_rebuild_clear_node_box(box->node);
			}
		}

		if ((box->gadget != NULL) && (box->gadget->box == box)) {
			box->gadget->box = NULL;
		}

		html_rebuild_unlink_boxes(box->children);
	}
}


/**
 * Link a box tree to the document and form controls.
 *
 * Used to reinstate a box tree which could not be replaced.
 *
 * \param box The root of the box tree
 */
static void html_rebuild_link_boxes(struct box *box)
{
	for (; box != NULL; box = box->next) {
		if (box->node != NULL) {
			struct box *old_box;
			dom_node_set_user_data(box->node,
					corestring_dom___ns_key_box_node_data,
					box, NULL, (void *)&old_box);
		}

		if (box->gadget != NULL) {
			box->gadget->box = box;
		}

		html_rebuild_link_boxes(box->children);
	}
}


/* exported interface documented in html/private.h */
void html_rebuild_detach(html_content *htmlc, struct box *layout)
{
	dom_node *html;
	dom_exception exc;

	html_rebuild_unlink_boxes(layout);

	exc = dom_document_get_document_element(htmlc->document, &html);
	if ((exc == DOM_NO_ERR) && (html != NULL)) {
		html_rebuild_walk_dom(html, nscss_invalidate_node_data);
		dom_node_unref(html);
	}
}


/**
 * Callback for completion of the box tree construction.
 */
static void html_rebuild_box_done(html_content *htmlc, bool success)
{
	html_rebuild_success = success;
}


/**
 * Clear state which refers to boxes of the box tree.
 *
 * \param htmlc HTML content whose box tree is to be replaced
 * \return The control of a focused textarea, which can keep the focus
 *         in the new tree, or NULL
 */
static struct form_control *html_rebuild_release_state(html_content *htmlc)
{
	struct form_control *focus = NULL;

	if (htmlc->drag_type != HTML_DRAG_NONE) {
		union html_drag_owner drag_owner;
		drag_owner.no_owner = true;
		html_set_drag_type(htmlc, HTML_DRAG_NONE, drag_owner, NULL);
	}

	if (htmlc->selection_type != HTML_SELECTION_NONE) {
		union html_selection_owner sel_owner;
		sel_owner.none = true;
		html_set_selection(htmlc, HTML_SELECTION_NONE, sel_owner, true);
	}

	if (htmlc->focus_type != HTML_FOCUS_SELF) {
		union html_focus_owner focus_owner;

		if (htmlc->focus_type == HTML_FOCUS_TEXTAREA) {
			focus = htmlc->focus_owner.textarea->gadget;
		}
		focus_owner.self = true;
		html_set_focus(htmlc, HTML_FOCUS_SELF, focus_owner,
			       true, 0, 0, 0, NULL);
	}

	if (htmlc->visible_select_menu != NULL) {
		form_free_select_menu(htmlc->visible_select_menu);
		htmlc->visible_select_menu = NULL;
	}

	if (htmlc->base.textsearch.context != NULL) {
		content_textsearch_destroy(htmlc->base.textsearch.context);
		htmlc->base.textsearch.context = NULL;
	}

	return focus;
}


/**
 * Rebuild the box tree of an HTML content from its document.
 *
 * \param htmlc HTML content to rebuild
 * \return NSERROR_OK on success, or the box tree is unchanged
 */
static nserror html_rebuild_box_tree(html_content *htmlc)
{
	struct content *c = &htmlc->base;
	struct content_html_object *old_objects;
	struct content_html_iframe *old_iframe;
	unsigned int old_num_objects;
	uint64_t ms_start;
	uint64_t ms_end;
	uint64_t ms_invalidated;
	uint64_t ms_built;
	uint64_t ms_freed;
	struct box *old_layout;
	struct form_control *focus;
	int *old_bctx;
	dom_node *html;
	dom_exception exc;
	nserror err;

	if ((htmlc->layout == NULL) ||
	    (htmlc->box_conversion_context != NULL) ||
	    (htmlc->frameset != NULL) ||
	    htmlc->aborted ||
	    c->locked ||
	    htmlc->reflowing ||
	    ((c->status != CONTENT_STATUS_READY) &&
	     (c->status != CONTENT_STATUS_DONE))) {
		/* not in a state where the box tree can be replaced */
		return NSERROR_OK;
	}

	exc = dom_document_get_document_element(htmlc->document, &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		return NSERROR_DOM;
	}

	old_bctx = htmlc->bctx;
	htmlc->bctx = talloc_zero(0, int);
	if (htmlc->bctx == NULL) {
		htmlc->bctx = old_bctx;
		dom_node_unref(html);
		return NSERROR_NOMEM;
	}

	NSLOG(netsurf, INFO, "Rebuilding box tree (content %p)", htmlc);
	nsu_getmonotonic_ms(&ms_start);

	htmlc->rebuilding = true;

	focus = html_rebuild_release_state(htmlc);

	/* Detach the old box tree and the lists built with it */
	old_layout = htmlc->layout;
	old_objects = htmlc->object_list;
	old_num_objects = htmlc->num_objects;
	old_iframe = htmlc->iframe;
	htmlc->layout = NULL;
	htmlc->object_list = NULL;
	htmlc->num_objects = 0;
	htmlc->iframe = NULL;

	html_rebuild_unlink_boxes(old_layout);

	/* The document may have changed in ways libcss has cached */
	html_rebuild_walk_dom(html, nscss_invalidate_node_data);

	nsu_getmonotonic_ms(&ms_invalidated);

	html_rebuild_success = false;
	err = dom_to_box_sync(html, htmlc, html_rebuild_box_done);
	if ((err != NSERROR_OK) || (html_rebuild_success == false)) {
		NSLOG(netsurf, WARNING, "Box tree rebuild failed (content %p)",
		      htmlc);

		/* Reinstate the old box tree.  Nodes may refer to boxes of
		 * the partial new tree so all node boxes are cleared first.
		 */
		html_rebuild_walk_dom(html, html_rebuild_clear_node_box);
		html_object_free_list(htmlc, htmlc->object_list);
		talloc_free(htmlc->bctx);

		htmlc->bctx = old_bctx;
		htmlc->layout = old_layout;
		htmlc->object_list = old_objects;
		htmlc->num_objects = old_num_objects;
		htmlc->iframe = old_iframe;
		html_rebuild_link_boxes(old_layout);

		htmlc->rebuilding = false;
		dom_node_unref(html);
		return (err != NSERROR_OK) ? err : NSERROR_BOX_CONVERT;
	}

	nsu_getmonotonic_ms(&ms_built);

	/* Move iframes to their new boxes while the old ones are valid */
	if (htmlc->bw != NULL) {
		browser_window_rebind_iframes(htmlc->bw);
	}

	/* Free the old box tree, and the iframe descriptors allocated in
	 * it, and release its objects.  The new tree has fetched them
	 * again, which the cache satisfies.
	 */
	html_object_free_list(htmlc, old_objects);
	talloc_free(old_bctx);

	/* Image maps refer to the document, which may have changed */
	imagemap_destroy(htmlc);
	err = imagemap_extract(htmlc);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, INFO, "imagemap extraction failed");
	}

	htmlc->rebuilding = false;
	dom_node_unref(html);

	nsu_getmonotonic_ms(&ms_freed);

	/* Lay out the new tree, which redraws it */
	if (htmlc->had_initial_layout) {
		content__reformat(c, false, c->available_width,
				  c->available_height);
	}

	nsu_getmonotonic_ms(&ms_end);
	NSLOG(netsurf, INFO, "Rebuilt and laid out box tree in %u ms (content %p): invalidate %u, styles and boxes %u, free and imagemaps %u, layout %u",
	      (unsigned int)(ms_end - ms_start), htmlc,
	      (unsigned int)(ms_invalidated - ms_start),
	      (unsigned int)(ms_built - ms_invalidated),
	      (unsigned int)(ms_freed - ms_built),
	      (unsigned int)(ms_end - ms_freed));

	/* Give the focus back to a textarea which is still displayed */
	if ((focus != NULL) &&
	    (focus->box != NULL) &&
	    (focus->data.text.ta != NULL)) {
		textarea_refresh_caret(focus->data.text.ta);
	}

	return NSERROR_OK;
}


/**
 * Scheduler callback which rebuilds the box tree.
 */
static void html_rebuild_box_tree_cb(void *p)
{
	html_content *htmlc = p;
	nserror err;

	htmlc->rebuild_pending = false;

	err = html_rebuild_box_tree(htmlc);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, WARNING, "Unable to rebuild box tree: %d", err);
	}
}


/**
 * Check if a change to a node can affect the box tree.
 *
 * \param htmlc HTML content the node belongs to
 * \param node The node which changed
 * \return true if the box tree should be rebuilt
 */
static bool html_rebuild_node_matters(html_content *htmlc, dom_node *node)
{
	dom_node *n = dom_node_ref(node);
	bool matters = true;

	while (n != NULL) {
		dom_node *parent = NULL;
		dom_node_type type;
		dom_exception exc;

		exc = dom_node_get_node_type(n, &type);
		if ((exc == DOM_NO_ERR) && (type == DOM_ELEMENT_NODE)) {
			dom_html_element_type tag_type;
			struct form_control *control = NULL;

			exc = dom_node_get_user_data(n,
				corestring_dom___ns_key_form_control_node_data,
				(void *)&control);
			if ((exc == DOM_NO_ERR) &&
			    (control != NULL) &&
			    control->syncing) {
				/* the browser is updating the document from
				 * a form control, which shows the change
				 */
				matters = false;
				break;
			}

			exc = dom_html_element_get_tag_type(n, &tag_type);
			if (exc == DOM_NO_ERR &&
			    (tag_type == DOM_HTML_ELEMENT_TYPE_HEAD ||
			     tag_type == DOM_HTML_ELEMENT_TYPE_TEXTAREA)) {
				/* the head is not displayed and a textarea's
				 * text is shown by its control
				 */
				matters = false;
				break;
			}
		}

		exc = dom_node_get_parent_node(n, &parent);
		dom_node_unref(n);
		n = (exc == DOM_NO_ERR) ? parent : NULL;
	}

	if (n != NULL) {
		dom_node_unref(n);
	}

	return matters;
}


/* exported interface documented in html/private.h */
void html_dom_changed(html_content *htmlc, dom_node *node)
{
	if (htmlc->early_layout) {
		/* the early box tree is rebuilt when next needed */
		if (htmlc->early_layout_building == false) {
			htmlc->early_layout_stale = true;
		}
		return;
	}

	if (htmlc->rebuild_pending ||
	    htmlc->rebuilding ||
	    (htmlc->layout == NULL) ||
	    (htmlc->box_conversion_context != NULL)) {
		/* already scheduled, or there is no box tree yet so the
		 * conversion will see the change
		 */
		return;
	}

	if (html_rebuild_node_matters(htmlc, node) == false) {
		return;
	}

	htmlc->rebuild_pending = true;
	guit->misc->schedule(HTML_REBUILD_DELAY_MS,
			     html_rebuild_box_tree_cb, htmlc);
}


/* exported interface documented in html/private.h */
void html_rebuild_flush(html_content *htmlc)
{
	if (htmlc->rebuild_pending && (htmlc->box_pins == 0)) {
		guit->misc->schedule(-1, html_rebuild_box_tree_cb, htmlc);
		html_rebuild_box_tree_cb(htmlc);
	}
}


/* exported interface documented in html/private.h */
void html_rebuild_cancel(html_content *htmlc)
{
	if (htmlc->rebuild_pending) {
		guit->misc->schedule(-1, html_rebuild_box_tree_cb, htmlc);
		htmlc->rebuild_pending = false;
	}
}
