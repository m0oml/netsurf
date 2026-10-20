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
 *
 * Redrawing CONTENT_HTML boxes with rounded corners.
 *
 * Plotters have no rounded rectangle, so these are drawn as paths, whose
 * corners are approximated with bezier curves.  Rings, such as the border
 * of a box, are one path made of an outer and an inner shape which wind
 * in opposite directions, so the inner one is a hole however the plotter
 * fills paths.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "netsurf/plotters.h"
#include "netsurf/css.h"
#include "css/utils.h"

#include "html/box.h"
#include "html/private.h"

/** Distance of a bezier control point from its end, as a part of the
 * radius, which makes the curve approximate a quarter ellipse */
#define KAPPA 0.5522847498f

/** Most floats a path of two rounded shapes needs */
#define PATH_MAX_FLOATS 96


/* exported interface documented in html/private.h */
bool html_radii_get(const css_computed_style *style,
		const css_unit_ctx *unit_len_ctx,
		int width, int height, float scale,
		struct html_radii *radii)
{
	uint8_t (*const get[4])(const css_computed_style *style,
			css_fixed *hlength, css_unit *hunit,
			css_fixed *vlength, css_unit *vunit) = {
		css_computed_border_top_left_radius,
		css_computed_border_top_right_radius,
		css_computed_border_bottom_right_radius,
		css_computed_border_bottom_left_radius
	};
	/* the corners at each end of the top, right, bottom and left sides */
	static const int side_a[4] = { HTML_TL, HTML_TR, HTML_BR, HTML_BL };
	static const int side_b[4] = { HTML_TR, HTML_BR, HTML_BL, HTML_TL };
	float factor = 1;
	bool any = false;
	int i;

	memset(radii, 0, sizeof(*radii));

	if (style == NULL || width <= 0 || height <= 0) {
		return false;
	}

	for (i = 0; i < 4; i++) {
		css_fixed hlength = 0, vlength = 0;
		css_unit hunit = CSS_UNIT_PX, vunit = CSS_UNIT_PX;

		get[i](style, &hlength, &hunit, &vlength, &vunit);

		if (hunit == CSS_UNIT_PCT) {
			radii->rx[i] = FIXTOFLT(hlength) * width / 100;
		} else {
			radii->rx[i] = FIXTOFLT(css_unit_len2device_px(style,
					unit_len_ctx, hlength, hunit)) * scale;
		}
		if (vunit == CSS_UNIT_PCT) {
			radii->ry[i] = FIXTOFLT(vlength) * height / 100;
		} else {
			radii->ry[i] = FIXTOFLT(css_unit_len2device_px(style,
					unit_len_ctx, vlength, vunit)) * scale;
		}

		if (radii->rx[i] < 0 || radii->ry[i] < 0 ||
				radii->rx[i] == 0 || radii->ry[i] == 0) {
			/* a corner with no radius in one direction is square */
			radii->rx[i] = 0;
			radii->ry[i] = 0;
		} else {
			any = true;
		}
	}

	if (any == false) {
		return false;
	}

	/* Radii which do not fit a side are all scaled down until they do */
	for (i = 0; i < 4; i++) {
		float sum;
		float length;

		if (i % 2 == 0) {
			sum = radii->rx[side_a[i]] + radii->rx[side_b[i]];
			length = width;
		} else {
			sum = radii->ry[side_a[i]] + radii->ry[side_b[i]];
			length = height;
		}
		if (sum > length && sum > 0 && length / sum < factor) {
			factor = length / sum;
		}
	}
	if (factor < 1) {
		for (i = 0; i < 4; i++) {
			radii->rx[i] *= factor;
			radii->ry[i] *= factor;
		}
	}

	return true;
}


/* exported interface documented in html/private.h */
void html_radii_adjust(struct html_radii *radii, float left, float top,
		float right, float bottom)
{
	/* each corner moves in by the sides either side of it */
	const float dx[4] = { left, right, right, left };
	const float dy[4] = { top, top, bottom, bottom };
	int i;

	for (i = 0; i < 4; i++) {
		if (radii->rx[i] <= 0 || radii->ry[i] <= 0) {
			continue;
		}
		radii->rx[i] -= dx[i];
		radii->ry[i] -= dy[i];
		if (radii->rx[i] <= 0 || radii->ry[i] <= 0) {
			radii->rx[i] = 0;
			radii->ry[i] = 0;
		}
	}
}


/* exported interface documented in html/private.h */
bool html_radii_any(const struct html_radii *radii)
{
	int i;

	for (i = 0; i < 4; i++) {
		if (radii->rx[i] > 0 && radii->ry[i] > 0) {
			return true;
		}
	}
	return false;
}


/**
 * Add a corner to a path as a curve, or as a line if it is square.
 *
 * The path is at the point where the corner starts.
 *
 * \param p       the path
 * \param n       number of floats in the path
 * \param ex      where the corner ends
 * \param ey      where the corner ends
 * \param c1x     first control point
 * \param c1y     first control point
 * \param c2x     second control point
 * \param c2y     second control point
 * \param round   whether the corner is rounded
 */
static void path_corner(float *p, unsigned int *n, float ex, float ey,
		float c1x, float c1y, float c2x, float c2y, bool round)
{
	if (round) {
		p[(*n)++] = PLOTTER_PATH_BEZIER;
		p[(*n)++] = c1x;
		p[(*n)++] = c1y;
		p[(*n)++] = c2x;
		p[(*n)++] = c2y;
		p[(*n)++] = ex;
		p[(*n)++] = ey;
	} else {
		p[(*n)++] = PLOTTER_PATH_LINE;
		p[(*n)++] = ex;
		p[(*n)++] = ey;
	}
}


static void path_line(float *p, unsigned int *n, float x, float y)
{
	p[(*n)++] = PLOTTER_PATH_LINE;
	p[(*n)++] = x;
	p[(*n)++] = y;
}


/**
 * Add a rounded rectangle to a path.
 *
 * \param p        the path
 * \param n        number of floats in the path
 * \param r        the rectangle
 * \param radii    the radii of its corners
 * \param reverse  wind counterclockwise, so the shape is a hole in a path
 *                 with a clockwise one around it
 */
static void path_add_shape(float *p, unsigned int *n, const struct rect *r,
		const struct html_radii *radii, bool reverse)
{
	const float x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
	const float tlx = radii->rx[HTML_TL], tly = radii->ry[HTML_TL];
	const float trx = radii->rx[HTML_TR], try = radii->ry[HTML_TR];
	const float brx = radii->rx[HTML_BR], bry = radii->ry[HTML_BR];
	const float blx = radii->rx[HTML_BL], bly = radii->ry[HTML_BL];
	const bool tl = tlx > 0 && tly > 0;
	const bool tr = trx > 0 && try > 0;
	const bool br = brx > 0 && bry > 0;
	const bool bl = blx > 0 && bly > 0;

	p[(*n)++] = PLOTTER_PATH_MOVE;
	p[(*n)++] = x0 + (tl ? tlx : 0);
	p[(*n)++] = y0;

	if (reverse == false) {
		path_line(p, n, x1 - (tr ? trx : 0), y0);
		path_corner(p, n, x1, y0 + (tr ? try : 0),
				x1 - trx + KAPPA * trx, y0,
				x1, y0 + try - KAPPA * try, tr);
		path_line(p, n, x1, y1 - (br ? bry : 0));
		path_corner(p, n, x1 - (br ? brx : 0), y1,
				x1, y1 - bry + KAPPA * bry,
				x1 - brx + KAPPA * brx, y1, br);
		path_line(p, n, x0 + (bl ? blx : 0), y1);
		path_corner(p, n, x0, y1 - (bl ? bly : 0),
				x0 + blx - KAPPA * blx, y1,
				x0, y1 - bly + KAPPA * bly, bl);
		path_line(p, n, x0, y0 + (tl ? tly : 0));
		path_corner(p, n, x0 + (tl ? tlx : 0), y0,
				x0, y0 + tly - KAPPA * tly,
				x0 + tlx - KAPPA * tlx, y0, tl);
	} else {
		path_corner(p, n, x0, y0 + (tl ? tly : 0),
				x0 + tlx - KAPPA * tlx, y0,
				x0, y0 + tly - KAPPA * tly, tl);
		path_line(p, n, x0, y1 - (bl ? bly : 0));
		path_corner(p, n, x0 + (bl ? blx : 0), y1,
				x0, y1 - bly + KAPPA * bly,
				x0 + blx - KAPPA * blx, y1, bl);
		path_line(p, n, x1 - (br ? brx : 0), y1);
		path_corner(p, n, x1, y1 - (br ? bry : 0),
				x1 - brx + KAPPA * brx, y1,
				x1, y1 - bry + KAPPA * bry, br);
		path_line(p, n, x1, y0 + (tr ? try : 0));
		path_corner(p, n, x1 - (tr ? trx : 0), y0,
				x1, y0 + try - KAPPA * try,
				x1 - trx + KAPPA * trx, y0, tr);
		path_line(p, n, x0 + (tl ? tlx : 0), y0);
	}

	p[(*n)++] = PLOTTER_PATH_CLOSE;
}


/**
 * Fill a path.
 */
static bool path_fill(const struct redraw_context *ctx, colour fill,
		const struct rect *clip, const float *p, unsigned int n)
{
	static const float transform[6] = { 1, 0, 0, 1, 0, 0 };
	/* plotters stroke if the stroke colour is not transparent, whatever
	 * the type of stroke */
	plot_style_t style = {
		.stroke_type = PLOT_OP_TYPE_NONE,
		.stroke_colour = NS_TRANSPARENT,
		.fill_type = PLOT_OP_TYPE_SOLID,
		.fill_colour = fill,
	};

	if (ctx->plot->clip(ctx, clip) != NSERROR_OK) {
		return false;
	}

	return ctx->plot->path(ctx, &style, p, n, transform) == NSERROR_OK;
}


/* exported interface documented in html/private.h */
bool html_redraw_rounded_fill(const struct redraw_context *ctx,
		const struct rect *r, const struct html_radii *radii,
		colour fill, const struct rect *clip)
{
	float p[PATH_MAX_FLOATS];
	unsigned int n = 0;

	if (r->x0 >= r->x1 || r->y0 >= r->y1) {
		return true;
	}

	path_add_shape(p, &n, r, radii, false);

	return path_fill(ctx, fill, clip, p, n);
}


/* exported interface documented in html/private.h */
bool html_redraw_rounded_ring(const struct redraw_context *ctx,
		const struct rect *outer, const struct html_radii *outer_radii,
		const struct rect *hole, const struct html_radii *hole_radii,
		colour fill, const struct rect *clip)
{
	float p[PATH_MAX_FLOATS];
	unsigned int n = 0;
	struct rect h = *hole;
	struct html_radii hr = *hole_radii;
	int clamped = 0;

	if (outer->x0 >= outer->x1 || outer->y0 >= outer->y1) {
		return true;
	}

	/* A part of the hole outside the shape would be filled, as it
	 * winds the other way round, so the hole is kept inside it.  Where
	 * it meets a side its corners are those of the shape. */
	if (h.x0 <= outer->x0) {
		h.x0 = outer->x0;
		hr.rx[HTML_TL] = outer_radii->rx[HTML_TL];
		hr.ry[HTML_TL] = outer_radii->ry[HTML_TL];
		hr.rx[HTML_BL] = outer_radii->rx[HTML_BL];
		hr.ry[HTML_BL] = outer_radii->ry[HTML_BL];
		clamped++;
	}
	if (h.x1 >= outer->x1) {
		h.x1 = outer->x1;
		hr.rx[HTML_TR] = outer_radii->rx[HTML_TR];
		hr.ry[HTML_TR] = outer_radii->ry[HTML_TR];
		hr.rx[HTML_BR] = outer_radii->rx[HTML_BR];
		hr.ry[HTML_BR] = outer_radii->ry[HTML_BR];
		clamped++;
	}
	if (h.y0 <= outer->y0) {
		h.y0 = outer->y0;
		hr.rx[HTML_TL] = outer_radii->rx[HTML_TL];
		hr.ry[HTML_TL] = outer_radii->ry[HTML_TL];
		hr.rx[HTML_TR] = outer_radii->rx[HTML_TR];
		hr.ry[HTML_TR] = outer_radii->ry[HTML_TR];
		clamped++;
	}
	if (h.y1 >= outer->y1) {
		h.y1 = outer->y1;
		hr.rx[HTML_BL] = outer_radii->rx[HTML_BL];
		hr.ry[HTML_BL] = outer_radii->ry[HTML_BL];
		hr.rx[HTML_BR] = outer_radii->rx[HTML_BR];
		hr.ry[HTML_BR] = outer_radii->ry[HTML_BR];
		clamped++;
	}
	if (clamped == 4) {
		/* the hole is all of the shape */
		return true;
	}

	path_add_shape(p, &n, outer, outer_radii, false);
	if (h.x0 < h.x1 && h.y0 < h.y1) {
		path_add_shape(p, &n, &h, &hr, true);
	}

	return path_fill(ctx, fill, clip, p, n);
}
