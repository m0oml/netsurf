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

#include <math.h>
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


/** Most shapes which are kept at once */
#define CLIP_STACK_MAX 8

/** A shape which what is drawn is kept inside */
struct clip_shape {
	struct rect area;
	struct html_radii radii;
};

static struct clip_shape clip_stack[CLIP_STACK_MAX];
static int clip_depth = 0;


/* exported interface documented in html/private.h */
bool html_redraw_clip_push(const struct rect *area,
		const struct html_radii *radii)
{
	if (clip_depth >= CLIP_STACK_MAX || html_radii_any(radii) == false) {
		return false;
	}

	clip_stack[clip_depth].area = *area;
	clip_stack[clip_depth].radii = *radii;
	clip_depth++;

	return true;
}


/* exported interface documented in html/private.h */
void html_redraw_clip_pop(void)
{
	if (clip_depth > 0) {
		clip_depth--;
	}
}


/**
 * Find how far in from the side of a shape a corner is at a row.
 *
 * \param rx  horizontal radius of the corner
 * \param ry  vertical radius of the corner
 * \param dy  distance of the row from the side the corner is on
 * \return distance in from the edge, which is zero past the corner
 */
static float corner_inset(float rx, float ry, float dy)
{
	float t;

	if (rx <= 0 || ry <= 0 || dy >= ry) {
		return 0;
	}

	t = (ry - dy) / ry;
	return rx * (1 - sqrtf(1 - t * t));
}


/**
 * Check if a rectangle is over a corner of a shape.
 */
static bool clip_over_corner(const struct clip_shape *s, const struct rect *r)
{
	const struct html_radii *c = &s->radii;
	/* each corner's rectangle, as the extents of its box */
	const float x0[4] = { s->area.x0, s->area.x1 - c->rx[HTML_TR],
			s->area.x1 - c->rx[HTML_BR], s->area.x0 };
	const float y0[4] = { s->area.y0, s->area.y0,
			s->area.y1 - c->ry[HTML_BR], s->area.y1 - c->ry[HTML_BL] };
	const float x1[4] = { s->area.x0 + c->rx[HTML_TL], s->area.x1,
			s->area.x1, s->area.x0 + c->rx[HTML_BL] };
	const float y1[4] = { s->area.y0 + c->ry[HTML_TL],
			s->area.y0 + c->ry[HTML_TR], s->area.y1,
			s->area.y1 };
	const int corner[4] = { HTML_TL, HTML_TR, HTML_BR, HTML_BL };
	int i;

	for (i = 0; i < 4; i++) {
		if (c->rx[corner[i]] <= 0 || c->ry[corner[i]] <= 0) {
			continue;
		}
		if (r->x0 < x1[i] && r->x1 > x0[i] &&
		    r->y0 < y1[i] && r->y1 > y0[i]) {
			return true;
		}
	}

	return false;
}


/**
 * Draw in a rectangle inside the shape at a depth of the stack and those
 * under it.
 */
static bool clipped_at(int depth, const struct rect *r,
		bool (*fn)(const struct rect *band, void *pw), void *pw)
{
	const struct clip_shape *s;
	struct rect band;
	int y, y0, y1;
	int band_x0 = 0, band_x1 = 0, band_y0 = 0;
	bool open = false;

	if (depth < 0) {
		return fn(r, pw);
	}

	s = &clip_stack[depth];
	if (clip_over_corner(s, r) == false) {
		return clipped_at(depth - 1, r, fn, pw);
	}

	y0 = r->y0;
	y1 = r->y1;

	for (y = y0; y <= y1; y++) {
		int x0 = 0, x1 = 0;
		bool empty = true;

		if (y < y1) {
			float row = y + 0.5f;
			float left = 0, right = 0, inset;

			/* the corners which this row is in */
			inset = corner_inset(s->radii.rx[HTML_TL],
					s->radii.ry[HTML_TL], row - s->area.y0);
			if (inset > left) left = inset;
			inset = corner_inset(s->radii.rx[HTML_BL],
					s->radii.ry[HTML_BL], s->area.y1 - row);
			if (inset > left) left = inset;
			inset = corner_inset(s->radii.rx[HTML_TR],
					s->radii.ry[HTML_TR], row - s->area.y0);
			if (inset > right) right = inset;
			inset = corner_inset(s->radii.rx[HTML_BR],
					s->radii.ry[HTML_BR], s->area.y1 - row);
			if (inset > right) right = inset;

			x0 = (int)ceilf(s->area.x0 + left);
			x1 = (int)floorf(s->area.x1 - right);
			if (x0 < r->x0) x0 = r->x0;
			if (x1 > r->x1) x1 = r->x1;
			empty = x0 >= x1;
		}

		/* rows with the same extents are drawn as one band */
		if (open && (y == y1 || empty ||
				x0 != band_x0 || x1 != band_x1)) {
			band.x0 = band_x0;
			band.x1 = band_x1;
			band.y0 = band_y0;
			band.y1 = y;
			if (clipped_at(depth - 1, &band, fn, pw) == false) {
				return false;
			}
			open = false;
		}
		if (open == false && empty == false) {
			open = true;
			band_x0 = x0;
			band_x1 = x1;
			band_y0 = y;
		}
	}

	return true;
}


/* exported interface documented in html/private.h */
bool html_redraw_clipped(const struct rect *r,
		bool (*fn)(const struct rect *band, void *pw), void *pw)
{
	if (r->x0 >= r->x1 || r->y0 >= r->y1) {
		return true;
	}

	return clipped_at(clip_depth - 1, r, fn, pw);
}


/** A point of a path */
struct pt {
	float x, y;
};

/** A cubic curve, which is a line where the corner is square */
struct arc {
	struct pt p0, c1, c2, p3;
};


static struct pt pt_mid(struct pt a, struct pt b)
{
	struct pt m = { (a.x + b.x) / 2, (a.y + b.y) / 2 };

	return m;
}


/**
 * Get the curve of a corner of a shape, going clockwise.
 *
 * \param r       the rectangle
 * \param radii   the radii of its corners
 * \param corner  the corner, HTML_TL, HTML_TR, HTML_BR or HTML_BL
 * \return the curve, which starts and ends at the corner if it is square
 */
static struct arc corner_arc(const struct rect *r,
		const struct html_radii *radii, int corner)
{
	const float x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
	const float rx = radii->rx[corner], ry = radii->ry[corner];
	struct arc a;

	if (rx <= 0 || ry <= 0) {
		struct pt p;

		switch (corner) {
		case HTML_TL: p.x = x0; p.y = y0; break;
		case HTML_TR: p.x = x1; p.y = y0; break;
		case HTML_BR: p.x = x1; p.y = y1; break;
		default: p.x = x0; p.y = y1; break;
		}
		a.p0 = a.c1 = a.c2 = a.p3 = p;
		return a;
	}

	switch (corner) {
	case HTML_TL:
		a.p0.x = x0;		a.p0.y = y0 + ry;
		a.c1.x = x0;		a.c1.y = y0 + ry - KAPPA * ry;
		a.c2.x = x0 + rx - KAPPA * rx; a.c2.y = y0;
		a.p3.x = x0 + rx;	a.p3.y = y0;
		break;
	case HTML_TR:
		a.p0.x = x1 - rx;	a.p0.y = y0;
		a.c1.x = x1 - rx + KAPPA * rx; a.c1.y = y0;
		a.c2.x = x1;		a.c2.y = y0 + ry - KAPPA * ry;
		a.p3.x = x1;		a.p3.y = y0 + ry;
		break;
	case HTML_BR:
		a.p0.x = x1;		a.p0.y = y1 - ry;
		a.c1.x = x1;		a.c1.y = y1 - ry + KAPPA * ry;
		a.c2.x = x1 - rx + KAPPA * rx; a.c2.y = y1;
		a.p3.x = x1 - rx;	a.p3.y = y1;
		break;
	default:
		a.p0.x = x0 + rx;	a.p0.y = y1;
		a.c1.x = x0 + rx - KAPPA * rx; a.c1.y = y1;
		a.c2.x = x0;		a.c2.y = y1 - ry + KAPPA * ry;
		a.p3.x = x0;		a.p3.y = y1 - ry;
		break;
	}

	return a;
}


/**
 * Cut a curve half way, giving the part before and after.
 */
static void arc_halve(const struct arc *a, struct arc *first,
		struct arc *second)
{
	const struct pt q0 = pt_mid(a->p0, a->c1);
	const struct pt q1 = pt_mid(a->c1, a->c2);
	const struct pt q2 = pt_mid(a->c2, a->p3);
	const struct pt r0 = pt_mid(q0, q1);
	const struct pt r1 = pt_mid(q1, q2);
	const struct pt m = pt_mid(r0, r1);

	first->p0 = a->p0; first->c1 = q0; first->c2 = r0; first->p3 = m;
	second->p0 = m; second->c1 = r1; second->c2 = q2; second->p3 = a->p3;
}


/**
 * Add a curve, or a line where it is straight, to a path, from where the
 * path is.
 */
static void path_arc(float *p, unsigned int *n, const struct arc *a)
{
	if (a->p0.x == a->p3.x && a->p0.y == a->p3.y) {
		path_line(p, n, a->p3.x, a->p3.y);
		return;
	}

	p[(*n)++] = PLOTTER_PATH_BEZIER;
	p[(*n)++] = a->c1.x;
	p[(*n)++] = a->c1.y;
	p[(*n)++] = a->c2.x;
	p[(*n)++] = a->c2.y;
	p[(*n)++] = a->p3.x;
	p[(*n)++] = a->p3.y;
}


/**
 * Reverse a curve.
 */
static struct arc arc_reverse(const struct arc *a)
{
	struct arc r;

	r.p0 = a->p3;
	r.c1 = a->c2;
	r.c2 = a->c1;
	r.p3 = a->p0;

	return r;
}


/* exported interface documented in html/private.h */
bool html_redraw_rounded_side(const struct redraw_context *ctx,
		int side,
		const struct rect *outer, const struct html_radii *outer_radii,
		const struct rect *hole, const struct html_radii *hole_radii,
		colour fill, const struct rect *clip)
{
	/* the corners at the start and end of a side, going clockwise */
	static const int start_corner[4] = { HTML_TL, HTML_TR, HTML_BR, HTML_BL };
	static const int end_corner[4] = { HTML_TR, HTML_BR, HTML_BL, HTML_TL };
	float p[PATH_MAX_FLOATS];
	unsigned int n = 0;
	struct rect h = *hole;
	struct arc oa, ob, ia, ib;
	struct arc o_before, o_after, i_before, i_after;
	struct arc unused;

	if (outer->x0 >= outer->x1 || outer->y0 >= outer->y1) {
		return true;
	}

	if (h.x0 >= h.x1 || h.y0 >= h.y1) {
		/* nothing is left of the hole, so it is all border */
		h.x0 = h.x1 = (h.x0 + h.x1) / 2;
		h.y0 = h.y1 = (h.y0 + h.y1) / 2;
	}

	oa = corner_arc(outer, outer_radii, start_corner[side]);
	ob = corner_arc(outer, outer_radii, end_corner[side]);
	ia = corner_arc(&h, hole_radii, start_corner[side]);
	ib = corner_arc(&h, hole_radii, end_corner[side]);

	/* the side runs from half way round the corner it starts at, to half
	 * way round the one it ends at */
	arc_halve(&oa, &unused, &o_after);
	arc_halve(&ob, &o_before, &unused);
	arc_halve(&ia, &unused, &i_after);
	arc_halve(&ib, &i_before, &unused);

	p[n++] = PLOTTER_PATH_MOVE;
	p[n++] = o_after.p0.x;
	p[n++] = o_after.p0.y;
	path_arc(p, &n, &o_after);
	path_line(p, &n, o_before.p0.x, o_before.p0.y);
	path_arc(p, &n, &o_before);

	/* across to the hole, and back along it */
	path_line(p, &n, i_before.p3.x, i_before.p3.y);
	{
		struct arc back = arc_reverse(&i_before);

		path_arc(p, &n, &back);
	}
	path_line(p, &n, i_after.p3.x, i_after.p3.y);
	{
		struct arc back = arc_reverse(&i_after);

		path_arc(p, &n, &back);
	}
	p[n++] = PLOTTER_PATH_CLOSE;

	return path_fill(ctx, fill, clip, p, n);
}
