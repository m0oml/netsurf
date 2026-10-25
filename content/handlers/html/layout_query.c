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
 * Layout information for scripts.
 *
 * Scripts can ask for the position and size of elements, the viewport
 * and the computed style of elements.  Any pending rebuild of the box
 * tree is done first so the answers reflect changes the script has made.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dom/dom.h>

#include "utils/utils.h"
#include "netsurf/browser_window.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "css/utils.h"

#include "html/private.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/box_inspect.h"

/**
 * Get an up to date box tree.
 *
 * \return true if the content has been laid out
 */
static bool html_layout_query_prepare(html_content *htmlc)
{
	html_rebuild_flush(htmlc);

	if (htmlc->had_initial_layout == false || htmlc->early_layout) {
		/* scripts can ask before the document has loaded */
		html_early_layout(htmlc);
	}

	return (htmlc->layout != NULL) && htmlc->had_initial_layout;
}


/**
 * Extend a rectangle to include the border box of a box.
 */
static void html_layout_query_add_box(struct box *box, struct rect *r, bool *any)
{
	int x;
	int y;
	struct rect b;

	box_coords(box, &x, &y);

	b.x0 = x - box->border[LEFT].width;
	b.y0 = y - box->border[TOP].width;
	b.x1 = x + box->padding[LEFT] + box->width + box->padding[RIGHT] +
			box->border[RIGHT].width;
	b.y1 = y + box->padding[TOP] + box->height + box->padding[BOTTOM] +
			box->border[BOTTOM].width;

	if (*any == false) {
		*r = b;
		*any = true;
		return;
	}

	if (b.x0 < r->x0) r->x0 = b.x0;
	if (b.y0 < r->y0) r->y0 = b.y0;
	if (b.x1 > r->x1) r->x1 = b.x1;
	if (b.y1 > r->y1) r->y1 = b.y1;
}


/* exported interface documented in html/private.h */
bool
html_layout_node_geometry(html_content *htmlc,
			  dom_node *node,
			  struct html_layout_geometry *geometry)
{
	struct box *box;
	struct rect r;
	bool any = false;

	if (html_layout_query_prepare(htmlc) == false) {
		return false;
	}

	box = box_for_node(node);
	if ((box == NULL) || (box->style == NULL)) {
		return false;
	}

	if ((box->type == BOX_INLINE) && (box->inline_end != NULL)) {
		/* An inline element's content follows its box up to the
		 * end of the inline, possibly across several lines.
		 */
		struct box *b;

		for (b = box; b != NULL; b = b->next) {
			html_layout_query_add_box(b, &r, &any);
			if (b == box->inline_end) {
				break;
			}
		}
	} else {
		html_layout_query_add_box(box, &r, &any);
	}

	geometry->x = r.x0;
	geometry->y = r.y0;
	geometry->width = r.x1 - r.x0;
	geometry->height = r.y1 - r.y0;
	geometry->client_width = box->padding[LEFT] + box->width +
			box->padding[RIGHT];
	geometry->client_height = box->padding[TOP] + box->height +
			box->padding[BOTTOM];
	geometry->scroll_width = max(geometry->client_width,
			box->descendant_x1 - box->descendant_x0);
	geometry->scroll_height = max(geometry->client_height,
			box->descendant_y1 - box->descendant_y0);

	return true;
}


/* exported interface documented in html/private.h */
void
html_layout_viewport(html_content *htmlc,
		     int *width, int *height,
		     int *scroll_x, int *scroll_y)
{
	*width = 0;
	*height = 0;
	*scroll_x = 0;
	*scroll_y = 0;

	if (html_layout_query_prepare(htmlc) == false) {
		/* before layout the size used for media queries is the
		 * best available
		 */
		unsigned w = 0;
		unsigned h = 0;
		union content_msg_data msg_data = {
			.getdims = {
				.viewport_width = &w,
				.viewport_height = &h,
			},
		};

		if (htmlc->media.width != 0) {
			*width = FIXTOINT(htmlc->media.width);
			*height = FIXTOINT(htmlc->media.height);
			return;
		}

		/* scripts can run before the content has asked for the
		 * viewport size, so ask the browser window directly
		 */
		content_broadcast(&htmlc->base, CONTENT_MSG_GETDIMS, &msg_data);
		*width = FIXTOINT(css_unit_device2css_px(INTTOFIX(w),
				nscss_screen_dpi));
		*height = FIXTOINT(css_unit_device2css_px(INTTOFIX(h),
				nscss_screen_dpi));
		return;
	}

	*width = FIXTOINT(htmlc->unit_len_ctx.viewport_width);
	*height = FIXTOINT(htmlc->unit_len_ctx.viewport_height);

	if (htmlc->bw != NULL) {
		if (browser_window_get_scroll(htmlc->bw,
					      scroll_x, scroll_y) != NSERROR_OK) {
			*scroll_x = 0;
			*scroll_y = 0;
		}
	}
}


/**
 * Format a length in px.
 */
static char *html_layout_px(int px)
{
	char buf[32];

	snprintf(buf, sizeof buf, "%dpx", px);
	return strdup(buf);
}


/**
 * Format a css colour.
 */
static char *html_layout_colour(css_color colour)
{
	char buf[48];
	unsigned int a = (colour >> 24) & 0xff;
	unsigned int r = (colour >> 16) & 0xff;
	unsigned int g = (colour >> 8) & 0xff;
	unsigned int b = colour & 0xff;

	if (a == 0xff) {
		snprintf(buf, sizeof buf, "rgb(%u, %u, %u)", r, g, b);
	} else {
		snprintf(buf, sizeof buf, "rgba(%u, %u, %u, %g)",
			 r, g, b, a / 255.0);
	}
	return strdup(buf);
}


/**
 * Get the css name of a display value.
 */
static const char *html_layout_display_name(uint8_t display)
{
	switch (display) {
	case CSS_DISPLAY_INLINE: return "inline";
	case CSS_DISPLAY_BLOCK: return "block";
	case CSS_DISPLAY_LIST_ITEM: return "list-item";
	case CSS_DISPLAY_RUN_IN: return "run-in";
	case CSS_DISPLAY_INLINE_BLOCK: return "inline-block";
	case CSS_DISPLAY_TABLE: return "table";
	case CSS_DISPLAY_INLINE_TABLE: return "inline-table";
	case CSS_DISPLAY_TABLE_ROW_GROUP: return "table-row-group";
	case CSS_DISPLAY_TABLE_HEADER_GROUP: return "table-header-group";
	case CSS_DISPLAY_TABLE_FOOTER_GROUP: return "table-footer-group";
	case CSS_DISPLAY_TABLE_ROW: return "table-row";
	case CSS_DISPLAY_TABLE_COLUMN_GROUP: return "table-column-group";
	case CSS_DISPLAY_TABLE_COLUMN: return "table-column";
	case CSS_DISPLAY_TABLE_CELL: return "table-cell";
	case CSS_DISPLAY_TABLE_CAPTION: return "table-caption";
	case CSS_DISPLAY_NONE: return "none";
	case CSS_DISPLAY_FLEX: return "flex";
	case CSS_DISPLAY_INLINE_FLEX: return "inline-flex";
	case CSS_DISPLAY_GRID: return "grid";
	case CSS_DISPLAY_INLINE_GRID: return "inline-grid";
	}
	return "";
}


/**
 * Get the css name of an overflow value.
 */
static const char *html_layout_overflow_name(uint8_t overflow)
{
	switch (overflow) {
	case CSS_OVERFLOW_VISIBLE: return "visible";
	case CSS_OVERFLOW_HIDDEN: return "hidden";
	case CSS_OVERFLOW_SCROLL: return "scroll";
	case CSS_OVERFLOW_AUTO: return "auto";
	}
	return "";
}


/**
 * Get a computed style value which needs no box.
 *
 * \return the value, or NULL if the property is not one of these
 */
static char *
html_layout_style_value(const css_computed_style *style,
			bool root,
			const char *property)
{
	css_color colour;
	css_fixed length;
	css_unit unit;
	char buf[32];

	if (strcmp(property, "display") == 0) {
		return strdup(html_layout_display_name(
				ns_computed_display(style, root)));
	}

	if (strcmp(property, "visibility") == 0) {
		switch (css_computed_visibility(style)) {
		case CSS_VISIBILITY_HIDDEN: return strdup("hidden");
		case CSS_VISIBILITY_COLLAPSE: return strdup("collapse");
		default: return strdup("visible");
		}
	}

	if (strcmp(property, "position") == 0) {
		switch (css_computed_position(style)) {
		case CSS_POSITION_RELATIVE: return strdup("relative");
		case CSS_POSITION_ABSOLUTE: return strdup("absolute");
		case CSS_POSITION_FIXED: return strdup("fixed");
		case CSS_POSITION_STICKY: return strdup("sticky");
		default: return strdup("static");
		}
	}

	if (strcmp(property, "float") == 0 ||
	    strcmp(property, "cssFloat") == 0) {
		switch (css_computed_float(style)) {
		case CSS_FLOAT_LEFT: return strdup("left");
		case CSS_FLOAT_RIGHT: return strdup("right");
		default: return strdup("none");
		}
	}

	if (strcmp(property, "overflow-x") == 0) {
		return strdup(html_layout_overflow_name(
				css_computed_overflow_x(style)));
	}

	if (strcmp(property, "overflow-y") == 0 ||
	    strcmp(property, "overflow") == 0) {
		return strdup(html_layout_overflow_name(
				css_computed_overflow_y(style)));
	}

	if (strcmp(property, "overflow-wrap") == 0 ||
	    strcmp(property, "word-wrap") == 0) {
		switch (css_computed_overflow_wrap(style)) {
		case CSS_OVERFLOW_WRAP_BREAK_WORD: return strdup("break-word");
		case CSS_OVERFLOW_WRAP_ANYWHERE: return strdup("anywhere");
		default: return strdup("normal");
		}
	}

	if (strcmp(property, "word-break") == 0) {
		switch (css_computed_word_break(style)) {
		case CSS_WORD_BREAK_BREAK_ALL: return strdup("break-all");
		case CSS_WORD_BREAK_KEEP_ALL: return strdup("keep-all");
		case CSS_WORD_BREAK_BREAK_WORD: return strdup("break-word");
		default: return strdup("normal");
		}
	}

	if (strcmp(property, "white-space") == 0) {
		switch (css_computed_white_space(style)) {
		case CSS_WHITE_SPACE_PRE: return strdup("pre");
		case CSS_WHITE_SPACE_NOWRAP: return strdup("nowrap");
		case CSS_WHITE_SPACE_PRE_WRAP: return strdup("pre-wrap");
		case CSS_WHITE_SPACE_PRE_LINE: return strdup("pre-line");
		default: return strdup("normal");
		}
	}

	if (strcmp(property, "color") == 0) {
		css_computed_color(style, &colour);
		return html_layout_colour(colour);
	}

	if (strcmp(property, "background-color") == 0) {
		if (css_computed_background_color(style, &colour) ==
		    CSS_BACKGROUND_COLOR_CURRENT_COLOR) {
			css_computed_color(style, &colour);
		}
		return html_layout_colour(colour);
	}

	if (strcmp(property, "font-size") == 0) {
		css_computed_font_size(style, &length, &unit);
		snprintf(buf, sizeof buf, "%gpx", FIXTOFLT(length));
		return strdup(buf);
	}

	if (strcmp(property, "font-weight") == 0) {
		uint8_t weight = css_computed_font_weight(style);

		if (weight == CSS_FONT_WEIGHT_BOLD) {
			return strdup("700");
		}
		if (weight >= CSS_FONT_WEIGHT_100 &&
		    weight <= CSS_FONT_WEIGHT_900) {
			snprintf(buf, sizeof buf, "%d",
				 (weight - CSS_FONT_WEIGHT_100 + 1) * 100);
			return strdup(buf);
		}
		return strdup("400");
	}

	if (strcmp(property, "opacity") == 0) {
		if (css_computed_opacity(style, &length) == CSS_OPACITY_SET) {
			snprintf(buf, sizeof buf, "%g", FIXTOFLT(length));
			return strdup(buf);
		}
		return strdup("1");
	}

	return NULL;
}


/**
 * Format a radius of a corner, in px or %.
 */
static void
html_layout_radius_text(char *buf, size_t len, css_fixed length, css_unit unit,
			const css_computed_style *style,
			const css_unit_ctx *unit_len_ctx)
{
	if (unit == CSS_UNIT_PCT) {
		snprintf(buf, len, "%g%%", FIXTOFLT(length));
	} else {
		snprintf(buf, len, "%gpx", FIXTOFLT(css_unit_len2device_px(
				style, unit_len_ctx, length, unit)));
	}
}


/**
 * Get the computed value of a border radius property.
 *
 * \return the value, or NULL if the property is not one of these
 */
static char *
html_layout_radius_value(const css_computed_style *style,
			 const css_unit_ctx *unit_len_ctx,
			 const char *property)
{
	static const struct {
		const char *name;
		uint8_t (*get)(const css_computed_style *style,
				css_fixed *hlength, css_unit *hunit,
				css_fixed *vlength, css_unit *vunit);
	} corners[4] = {
		{ "border-top-left-radius", css_computed_border_top_left_radius },
		{ "border-top-right-radius", css_computed_border_top_right_radius },
		{ "border-bottom-right-radius", css_computed_border_bottom_right_radius },
		{ "border-bottom-left-radius", css_computed_border_bottom_left_radius },
	};
	char h[4][32], v[4][32];
	char buf[512];
	bool same_h = true, same_v = true, h_eq_v = true;
	unsigned int i;

	for (i = 0; i < 4; i++) {
		css_fixed hl = 0, vl = 0;
		css_unit hu = CSS_UNIT_PX, vu = CSS_UNIT_PX;

		corners[i].get(style, &hl, &hu, &vl, &vu);
		html_layout_radius_text(h[i], sizeof h[i], hl, hu, style,
					unit_len_ctx);
		html_layout_radius_text(v[i], sizeof v[i], vl, vu, style,
					unit_len_ctx);

		if (strcmp(property, corners[i].name) == 0) {
			if (strcmp(h[i], v[i]) == 0) {
				return strdup(h[i]);
			}
			snprintf(buf, sizeof buf, "%s %s", h[i], v[i]);
			return strdup(buf);
		}
	}

	if (strcmp(property, "border-radius") != 0 &&
	    strcmp(property, "borderRadius") != 0) {
		return NULL;
	}

	for (i = 0; i < 4; i++) {
		same_h = same_h && (strcmp(h[i], h[0]) == 0);
		same_v = same_v && (strcmp(v[i], v[0]) == 0);
		h_eq_v = h_eq_v && (strcmp(h[i], v[i]) == 0);
	}

	if (same_h && same_v) {
		if (h_eq_v) {
			return strdup(h[0]);
		}
		snprintf(buf, sizeof buf, "%s / %s", h[0], v[0]);
	} else if (h_eq_v) {
		snprintf(buf, sizeof buf, "%s %s %s %s", h[0], h[1], h[2], h[3]);
	} else {
		snprintf(buf, sizeof buf, "%s %s %s %s / %s %s %s %s",
			 h[0], h[1], h[2], h[3], v[0], v[1], v[2], v[3]);
	}

	return strdup(buf);
}


/**
 * Get a used value from a box, such as a margin or size.
 *
 * \return the value, or NULL if the property is not one of these
 */
static char *html_layout_box_value(struct box *box, const char *property)
{
	static const char *sides[] = { "top", "right", "bottom", "left" };
	static const int side_index[] = { TOP, RIGHT, BOTTOM, LEFT };
	unsigned int i;
	char name[32];

	if (strcmp(property, "width") == 0) {
		return html_layout_px(box->width);
	}
	if (strcmp(property, "height") == 0) {
		return html_layout_px(box->height);
	}

	for (i = 0; i < 4; i++) {
		snprintf(name, sizeof name, "margin-%s", sides[i]);
		if (strcmp(property, name) == 0) {
			return html_layout_px(box->margin[side_index[i]]);
		}
		snprintf(name, sizeof name, "padding-%s", sides[i]);
		if (strcmp(property, name) == 0) {
			return html_layout_px(box->padding[side_index[i]]);
		}
		snprintf(name, sizeof name, "border-%s-width", sides[i]);
		if (strcmp(property, name) == 0) {
			return html_layout_px(box->border[side_index[i]].width);
		}
	}

	return NULL;
}


/* exported interface documented in html/private.h */
char *
html_layout_computed_style(html_content *htmlc,
			   dom_node *node,
			   const char *property)
{
	struct box *box;
	char *value;

	if (html_layout_query_prepare(htmlc) == false) {
		return NULL;
	}

	box = box_for_node(node);
	if ((box == NULL) || (box->style == NULL)) {
		/* elements without a box are not displayed */
		if (strcmp(property, "display") == 0) {
			return strdup("none");
		}
		return NULL;
	}

	value = html_layout_style_value(box->style, box->parent == NULL,
					property);
	if (value != NULL) {
		return value;
	}

	value = html_layout_radius_value(box->style, &htmlc->unit_len_ctx,
					 property);
	if (value != NULL) {
		return value;
	}

	return html_layout_box_value(box, property);
}
