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
 * HTML layout implementation: display: grid.
 *
 * Grid containers use the flex container box types, as their children
 * are made into blocks in the same way, and layout_flex() passes them
 * here.
 *
 * Items are placed by their lines, names and spans, and then by the
 * auto-placement algorithm.  Tracks are sized by a simplified form of
 * the CSS Grid track sizing algorithm: fixed, intrinsic and flexible
 * sizes, items spanning several tracks, and stretching of auto tracks.
 * Subgrids, baseline alignment and masonry are not supported.
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "css/utils.h"

#include "html/box.h"
#include "html/html.h"
#include "html/private.h"
#include "html/layout_internal.h"

/** The most tracks an axis may have; lines beyond are clamped */
#define GRID_MAX_TRACKS 1000

/** A growth limit which is not yet known */
#define GRID_UNSET -1

/** Sizing functions of a track, resolved for layout */
enum grid_sizing {
	GRID_FIXED,
	GRID_AUTO,
	GRID_MIN_CONTENT,
	GRID_MAX_CONTENT,
	GRID_FLEX,
	GRID_FIT_CONTENT
};

/** A track being sized */
struct grid_track {
	const css_computed_grid_track *size;	/**< Sizing functions */
	uint8_t min_type;	/**< grid_sizing of the minimum */
	uint8_t max_type;	/**< grid_sizing of the maximum */
	int min;		/**< Fixed minimum in pixels */
	int max;		/**< Fixed maximum or fit-content limit */
	float flex;		/**< Flex factor of a flexible maximum */
	int base;		/**< Base size in pixels */
	int limit;		/**< Growth limit in pixels, or GRID_UNSET */
	int pos;		/**< Offset from the content edge */
	bool fit;		/**< In a repeat(auto-fit, ...) */
	bool collapsed;		/**< An empty auto-fit track */
};

/** A line name */
struct grid_name {
	lwc_string *name;
	const char *suffix;	/**< "-start" or "-end" for an area, or "" */
	int line;		/**< Line number in the explicit grid, from 1 */
};

/** One axis of a grid */
struct grid_axis {
	struct grid_track *tracks;
	int count;		/**< Number of tracks */
	int explicit_count;	/**< Number of tracks of the explicit grid */
	int before;		/**< Implicit tracks before the explicit grid */
	int template_count;	/**< Explicit tracks the template sizes */

	/** The template's sizes of the explicit tracks */
	const css_computed_grid_track **sizes;
	bool *fit;		/**< Whether each explicit track is auto-fit */

	/** Sizes of implicit tracks, from grid-auto-rows or -columns */
	const css_computed_grid_track *auto_sizes;
	int auto_count;

	struct grid_name *names;
	int name_count;
	int name_alloc;

	int gap;		/**< Gap between tracks in pixels */
	int available;		/**< Size of the content box, or AUTO */
};

/** An item of a grid */
struct grid_item {
	struct box *box;
	int start[2];		/**< First track of each axis, or -1 */
	int span[2];		/**< Tracks spanned in each axis */
	int min_contribution[2];
	int max_contribution[2];
	int32_t order;		/**< The order property */
};

/** Grid layout context */
struct grid_ctx {
	struct box *grid;
	html_content *content;		/**< For laying out items */
	const css_unit_ctx *unit_len_ctx;

	struct grid_item *items;
	size_t count;

	struct grid_axis axis[2];	/**< Columns, then rows */
	bool column_flow;
	bool dense;
};

/** The sizing of a track with nothing set */
static const css_computed_grid_track grid_auto_track = {
	.kind = CSS_COMPUTED_GRID_TRACK_SIZE,
	.min_type = CSS_COMPUTED_GRID_SIZING_AUTO,
	.max_type = CSS_COMPUTED_GRID_SIZING_AUTO,
};


/**
 * Check whether a box is a grid container.
 *
 * \param[in] b  Box to check
 * \return true if the box is laid out as a grid
 */
bool layout_grid_is_grid(const struct box *b)
{
	enum css_display_e display;

	if (lh__box_is_flex_container(b) == false || b->style == NULL)
		return false;

	display = ns_computed_display_static(b->style);

	return display == CSS_DISPLAY_GRID ||
	       display == CSS_DISPLAY_INLINE_GRID;
}


/**
 * Convert a length or percentage to pixels.
 *
 * \param[in]  ctx        Grid layout context
 * \param[in]  value      The length
 * \param[in]  unit       The unit of the length
 * \param[in]  available  The size percentages are of, or AUTO
 * \param[out] px         Updated to the length in pixels
 * \return true if the length is definite
 */
static bool grid_length(const struct grid_ctx *ctx, css_fixed value,
		css_unit unit, int available, int *px)
{
	if (unit == CSS_UNIT_PCT) {
		if (available == AUTO)
			return false;

		*px = FPCT_OF_INT_TOINT(value, available);
		return true;
	}

	*px = FIXTOINT(css_unit_len2device_px(ctx->grid->style,
			ctx->unit_len_ctx, value, unit));
	return true;
}


/**
 * Set up a track from its sizing functions.
 *
 * \param[in]  ctx        Grid layout context
 * \param[in]  size       The sizing functions
 * \param[in]  available  The size of the content box, or AUTO
 * \param[out] t          The track
 */
static void grid_track_init(const struct grid_ctx *ctx,
		const css_computed_grid_track *size, int available,
		struct grid_track *t)
{
	memset(t, 0, sizeof(*t));
	t->size = size;

	switch (size->min_type) {
	case CSS_COMPUTED_GRID_SIZING_LENGTH:
		t->min_type = grid_length(ctx, size->min, size->min_unit,
				available, &t->min) ? GRID_FIXED : GRID_AUTO;
		break;
	case CSS_COMPUTED_GRID_SIZING_MIN_CONTENT:
		t->min_type = GRID_MIN_CONTENT;
		break;
	case CSS_COMPUTED_GRID_SIZING_MAX_CONTENT:
		t->min_type = GRID_MAX_CONTENT;
		break;
	default:
		t->min_type = GRID_AUTO;
		break;
	}

	switch (size->max_type) {
	case CSS_COMPUTED_GRID_SIZING_LENGTH:
		t->max_type = grid_length(ctx, size->max, size->max_unit,
				available, &t->max) ? GRID_FIXED : GRID_AUTO;
		break;
	case CSS_COMPUTED_GRID_SIZING_FLEX:
		t->max_type = GRID_FLEX;
		t->flex = FIXTOFLT(size->max);
		break;
	case CSS_COMPUTED_GRID_SIZING_MIN_CONTENT:
		t->max_type = GRID_MIN_CONTENT;
		break;
	case CSS_COMPUTED_GRID_SIZING_MAX_CONTENT:
		t->max_type = GRID_MAX_CONTENT;
		break;
	case CSS_COMPUTED_GRID_SIZING_FIT_CONTENT:
		t->max_type = grid_length(ctx, size->max, size->max_unit,
				available, &t->max) ?
				GRID_FIT_CONTENT : GRID_MAX_CONTENT;
		break;
	default:
		t->max_type = GRID_AUTO;
		break;
	}
}


/**
 * Get the size of a track for working out how many tracks an automatic
 * repeat makes: its fixed maximum, or else its fixed minimum.
 */
static int grid_track_fixed_size(const struct grid_ctx *ctx,
		const css_computed_grid_track *size, int available)
{
	struct grid_track t;

	grid_track_init(ctx, size, available, &t);

	if (t.max_type == GRID_FIXED)
		return max(t.max, t.min_type == GRID_FIXED ? t.min : 0);
	if (t.min_type == GRID_FIXED)
		return t.min;

	return 0;
}


/**
 * Add a line name to an axis.
 *
 * \return true on success, false on memory exhaustion
 */
static bool grid_add_name(struct grid_axis *axis, lwc_string *name,
		const char *suffix, int line)
{
	if (axis->name_count == axis->name_alloc) {
		int alloc = axis->name_alloc == 0 ? 8 : axis->name_alloc * 2;
		struct grid_name *names;

		names = realloc(axis->names, alloc * sizeof(*names));
		if (names == NULL)
			return false;

		axis->names = names;
		axis->name_alloc = alloc;
	}

	axis->names[axis->name_count].name = name;
	axis->names[axis->name_count].suffix = suffix;
	axis->names[axis->name_count].line = line;
	axis->name_count++;

	return true;
}


/**
 * Add an explicit track to an axis.
 *
 * \return true on success, false on memory exhaustion
 */
static bool grid_add_explicit(struct grid_axis *axis,
		const css_computed_grid_track *size, bool fit, int *alloc)
{
	if (axis->explicit_count >= GRID_MAX_TRACKS)
		return true;

	if (axis->explicit_count == *alloc) {
		int n = *alloc == 0 ? 8 : *alloc * 2;
		const css_computed_grid_track **sizes;
		bool *fits;

		sizes = realloc(axis->sizes, n * sizeof(*sizes));
		if (sizes == NULL)
			return false;
		axis->sizes = sizes;

		fits = realloc(axis->fit, n * sizeof(*fits));
		if (fits == NULL)
			return false;
		axis->fit = fits;

		*alloc = n;
	}

	axis->sizes[axis->explicit_count] = size;
	axis->fit[axis->explicit_count] = fit;
	axis->explicit_count++;

	return true;
}


/**
 * Find how many times an automatic repeat repeats.
 *
 * \param[in] ctx        Grid layout context
 * \param[in] axis       The axis
 * \param[in] tracks     The template
 * \return the number of repetitions
 */
static int grid_auto_repeat_count(const struct grid_ctx *ctx,
		const struct grid_axis *axis,
		const css_computed_grid_track *tracks)
{
	const css_computed_grid_track *t;
	int other = 0, other_count = 0;
	int repeat = 0, repeat_count = 0;
	int space, count;

	if (axis->available == AUTO)
		return 1;

	for (t = tracks; t->kind != CSS_COMPUTED_GRID_TRACK_END; t++) {
		int size;

		if (t->kind != CSS_COMPUTED_GRID_TRACK_SIZE)
			continue;

		size = grid_track_fixed_size(ctx, t, axis->available);
		if (t->flags & (CSS_COMPUTED_GRID_TRACK_AUTO_FILL |
				CSS_COMPUTED_GRID_TRACK_AUTO_FIT)) {
			repeat += size;
			repeat_count++;
		} else {
			other += size;
			other_count++;
		}
	}

	if (repeat_count == 0)
		return 0;

	/* Every track but the last is followed by a gap */
	space = axis->available - other - other_count * axis->gap +
			axis->gap;
	repeat += repeat_count * axis->gap;
	if (repeat <= 0 || space < repeat)
		return 1;

	count = space / repeat;
	if (count * repeat_count + other_count > GRID_MAX_TRACKS)
		count = (GRID_MAX_TRACKS - other_count) / repeat_count;

	return max(count, 1);
}


/**
 * Set up the explicit grid of an axis from its template and areas.
 *
 * \param[in] ctx     Grid layout context
 * \param[in] axis    The axis to set up
 * \param[in] tracks  The template, or NULL for none
 * \param[in] areas   The template areas, or NULL for none
 * \param[in] rows    Whether this is the row axis
 * \return true on success, false on memory exhaustion
 */
static bool grid_axis_explicit(const struct grid_ctx *ctx,
		struct grid_axis *axis,
		const css_computed_grid_track *tracks,
		const css_computed_grid_area *areas, bool rows)
{
	const css_computed_grid_track *t;
	int alloc = 0;
	int repeats;

	if (tracks != NULL) {
		repeats = grid_auto_repeat_count(ctx, axis, tracks);

		for (t = tracks; t->kind != CSS_COMPUTED_GRID_TRACK_END; ) {
			const css_computed_grid_track *group = t;
			int r;

			if ((t->flags & (CSS_COMPUTED_GRID_TRACK_AUTO_FILL |
					CSS_COMPUTED_GRID_TRACK_AUTO_FIT)) == 0) {
				if (t->kind == CSS_COMPUTED_GRID_TRACK_NAME) {
					if (!grid_add_name(axis, t->name, "",
						axis->explicit_count + 1))
						return false;
				} else if (!grid_add_explicit(axis, t, false,
						&alloc)) {
					return false;
				}
				t++;
				continue;
			}

			/* An automatic repeat: its entries are together */
			for (r = 0; r < repeats; r++) {
				for (t = group; t->flags != 0 && t->kind !=
						CSS_COMPUTED_GRID_TRACK_END;
						t++) {
					bool fit = t->flags &
						CSS_COMPUTED_GRID_TRACK_AUTO_FIT;

					if (t->kind ==
						CSS_COMPUTED_GRID_TRACK_NAME) {
						if (!grid_add_name(axis,
							t->name, "",
							axis->explicit_count +
							1))
							return false;
					} else if (!grid_add_explicit(axis, t,
							fit, &alloc)) {
						return false;
					}
				}
			}
			if (repeats == 0) {
				while (t->flags != 0 && t->kind !=
						CSS_COMPUTED_GRID_TRACK_END)
					t++;
			}
		}
	}

	axis->template_count = axis->explicit_count;

	if (areas != NULL) {
		const css_computed_grid_area *a;
		int count;

		for (a = areas; a->name != NULL; a++) {
			int start = rows ? a->row_start : a->column_start;
			int end = rows ? a->row_end : a->column_end;

			if (!grid_add_name(axis, a->name, "-start", start) ||
			    !grid_add_name(axis, a->name, "-end", end))
				return false;
		}

		/* The end entry gives the size of the template */
		count = rows ? a->row_end : a->column_end;
		while (axis->explicit_count < count &&
				axis->explicit_count < GRID_MAX_TRACKS) {
			if (!grid_add_explicit(axis, NULL, false, &alloc))
				return false;
		}
	}

	return true;
}


/**
 * Check if a line name matches a name with a suffix.
 *
 * A name with a suffix may have been given whole, as "a-start", or as
 * the implicit name of an area.
 */
static bool grid_name_matches(const struct grid_name *n, lwc_string *name,
		const char *suffix)
{
	const char *whole, *base, *with;
	size_t whole_len, base_len, suffix_len;
	lwc_string *full;

	if (strcmp(n->suffix, suffix) == 0)
		return n->name == name;

	/* One has a suffix the other's name carries in its text */
	if (n->suffix[0] == '\0') {
		full = n->name;
		with = suffix;
		base = lwc_string_data(name);
		base_len = lwc_string_length(name);
	} else if (suffix[0] == '\0') {
		full = name;
		with = n->suffix;
		base = lwc_string_data(n->name);
		base_len = lwc_string_length(n->name);
	} else {
		return false;
	}

	whole = lwc_string_data(full);
	whole_len = lwc_string_length(full);
	suffix_len = strlen(with);

	return whole_len == base_len + suffix_len &&
			memcmp(whole, base, base_len) == 0 &&
			memcmp(whole + base_len, with, suffix_len) == 0;
}


/**
 * Find the nth line with a name, in line order.
 *
 * \param[in]  axis    The axis
 * \param[in]  name    The name
 * \param[in]  suffix  Suffix of the name, or ""
 * \param[in]  from    Line to search after, going forwards, or before,
 *                     going backwards
 * \param[in]  n       Which line to find; negative to search backwards
 * \param[out] line    Updated to the line
 * \return true if found, or false with \a n updated to the lines missing
 */
static bool grid_find_name(const struct grid_axis *axis, lwc_string *name,
		const char *suffix, int from, int *n, int *line)
{
	int step = *n > 0 ? 1 : -1;
	int wanted = abs(*n);
	int last = axis->explicit_count + 1;
	int l;

	for (l = from + step; l >= 1 && l <= last; l += step) {
		int i;

		for (i = 0; i < axis->name_count; i++) {
			if (axis->names[i].line == l &&
			    grid_name_matches(&axis->names[i], name, suffix))
				break;
		}

		if (i < axis->name_count && --wanted == 0) {
			*line = l;
			return true;
		}
	}

	*n = wanted * step;
	return false;
}


/**
 * Resolve a line which is not a span.
 *
 * \param[in]  axis     The axis
 * \param[in]  type     css_grid_line_e of the line
 * \param[in]  integer  Its integer, or 0 for a name alone
 * \param[in]  name     Its name, or NULL
 * \param[in]  start    Whether it is a start line
 * \param[out] line     Updated to the line, numbered from 1 at the start
 *                      of the explicit grid
 * \return true if the line is definite
 */
static bool grid_resolve_line(const struct grid_axis *axis, uint8_t type,
		int32_t integer, lwc_string *name, bool start, int *line)
{
	int last = axis->explicit_count + 1;
	int n;

	if (type != CSS_GRID_LINE_SET)
		return false;

	if (name == NULL) {
		*line = integer > 0 ? integer : last + 1 + integer;
		return true;
	}

	if (integer == 0) {
		/* A name alone is first an area's edge */
		n = 1;
		if (grid_find_name(axis, name, start ? "-start" : "-end", 0,
				&n, line))
			return true;
		integer = 1;
	}

	n = integer;
	if (grid_find_name(axis, name, "", n > 0 ? 0 : last + 1, &n, line))
		return true;

	/* Lines beyond the explicit grid all have every name */
	*line = n > 0 ? last + n : 1 + n;
	return true;
}


/**
 * Resolve a span from a definite line.
 *
 * \param[in] axis     The axis
 * \param[in] integer  The span's integer
 * \param[in] name     The span's name, or NULL
 * \param[in] from     The definite line
 * \param[in] forward  Whether the span goes towards the end
 * \return the other line
 */
static int grid_resolve_span(const struct grid_axis *axis, int32_t integer,
		lwc_string *name, int from, bool forward)
{
	int last = axis->explicit_count + 1;
	int n = forward ? integer : -integer;
	int line;

	if (name == NULL)
		return from + n;

	if (grid_find_name(axis, name, "", from, &n, &line))
		return line;

	return n > 0 ? max(from, last) + n : min(from, 1) + n;
}


/**
 * Resolve the placement of an item along one axis.
 *
 * \param[in]  axis   The axis
 * \param[in]  style  The item's style
 * \param[in]  rows   Whether this is the row axis
 * \param[out] start  Updated to the start line, or INT_MIN if automatic
 * \param[out] span   Updated to the number of tracks spanned
 */
static void grid_resolve_item(const struct grid_axis *axis,
		const css_computed_style *style, bool rows,
		int *start, int *span)
{
	uint8_t (*get_start)(const css_computed_style *, int32_t *,
			lwc_string **);
	uint8_t (*get_end)(const css_computed_style *, int32_t *,
			lwc_string **);
	lwc_string *start_name = NULL, *end_name = NULL;
	int32_t start_int = 0, end_int = 0;
	uint8_t start_type, end_type;
	bool start_definite, end_definite;
	int s = 0, e = 0;

	get_start = rows ? css_computed_grid_row_start :
			css_computed_grid_column_start;
	get_end = rows ? css_computed_grid_row_end :
			css_computed_grid_column_end;

	start_type = get_start(style, &start_int, &start_name);
	end_type = get_end(style, &end_int, &end_name);

	start_definite = grid_resolve_line(axis, start_type, start_int,
			start_name, true, &s);
	end_definite = grid_resolve_line(axis, end_type, end_int,
			end_name, false, &e);

	if (start_definite && end_definite) {
		if (e < s) {
			int t = s;
			s = e;
			e = t;
		} else if (e == s) {
			e = s + 1;
		}
	} else if (start_definite) {
		if (end_type == CSS_GRID_LINE_SPAN)
			e = grid_resolve_span(axis, end_int, end_name, s, true);
		else
			e = s + 1;
	} else if (end_definite) {
		if (start_type == CSS_GRID_LINE_SPAN)
			s = grid_resolve_span(axis, start_int, start_name, e,
					false);
		else
			s = e - 1;
	} else {
		/* Automatic, with any span, which may not have a name */
		*start = INT_MIN;
		*span = 1;
		if (start_type == CSS_GRID_LINE_SPAN)
			*span = start_int;
		else if (end_type == CSS_GRID_LINE_SPAN)
			*span = end_int;
		*span = min(max(*span, 1), GRID_MAX_TRACKS);
		return;
	}

	s = min(max(s, -GRID_MAX_TRACKS), GRID_MAX_TRACKS);
	e = min(max(e, s + 1), s + GRID_MAX_TRACKS);
	*start = s;
	*span = e - s;
}


/**
 * Check whether an area of the occupancy grid is free.
 */
static bool grid_area_free(const bool *cells, int columns, int rows,
		int column, int row, int column_span, int row_span)
{
	int r, c;

	for (r = row; r < row + row_span; r++) {
		if (r >= rows)
			return true;
		for (c = column; c < column + column_span; c++) {
			if (cells[r * columns + c])
				return false;
		}
	}

	return true;
}


/**
 * Mark an area of the occupancy grid as used, growing it if needed.
 *
 * \return true on success, false on memory exhaustion
 */
static bool grid_area_occupy(bool **cells, int columns, int *rows,
		int column, int row, int column_span, int row_span)
{
	int r, c;

	if (row + row_span > *rows) {
		int n = row + row_span;
		bool *grown = realloc(*cells, (size_t) n * columns *
				sizeof(**cells));

		if (grown == NULL)
			return false;

		memset(grown + (size_t) *rows * columns, 0,
				(size_t) (n - *rows) * columns *
				sizeof(**cells));
		*cells = grown;
		*rows = n;
	}

	for (r = row; r < row + row_span; r++) {
		for (c = column; c < column + column_span; c++)
			(*cells)[r * columns + c] = true;
	}

	return true;
}


/**
 * Place the items of a grid.
 *
 * Items with definite lines are placed first, then items locked to a
 * line of the flow's cross axis, then the rest, by the auto-placement
 * algorithm.  The axes' track counts are updated to the implicit grid.
 *
 * \param[in] ctx  Grid layout context
 * \return true on success, false on memory exhaustion
 */
static bool grid_place_items(struct grid_ctx *ctx)
{
	/* The flow fills along the fixed axis, then grows the other */
	int fix = ctx->column_flow ? 1 : 0;
	int grow = 1 - fix;
	int lowest[2] = { 1, 1 };
	int counts[2];
	bool *cells = NULL;
	int grown = 0;
	int *cursors = NULL;
	int cursor_fix = 0, cursor_grow = 0;
	size_t i;
	int a;

	/* Resolve definite lines, in lines of the explicit grid */
	for (i = 0; i < ctx->count; i++) {
		struct grid_item *item = &ctx->items[i];

		for (a = 0; a < 2; a++) {
			grid_resolve_item(&ctx->axis[a], item->box->style,
					a == 1, &item->start[a],
					&item->span[a]);
			if (item->start[a] != INT_MIN &&
					item->start[a] < lowest[a])
				lowest[a] = item->start[a];
		}
	}

	/* Tracks before the explicit grid move it along */
	for (a = 0; a < 2; a++) {
		ctx->axis[a].before = 1 - lowest[a];
		counts[a] = ctx->axis[a].before +
				ctx->axis[a].explicit_count;
	}

	for (i = 0; i < ctx->count; i++) {
		struct grid_item *item = &ctx->items[i];

		for (a = 0; a < 2; a++) {
			if (item->start[a] == INT_MIN) {
				item->start[a] = -1;
				if (a == fix && item->span[a] > counts[a])
					counts[a] = item->span[a];
				continue;
			}

			item->start[a] += ctx->axis[a].before - 1;
			if (item->start[a] + item->span[a] > counts[a])
				counts[a] = item->start[a] + item->span[a];
		}
	}

	/* Lines are clamped when resolved, so counts[fix] is at most
	 * about three times GRID_MAX_TRACKS */
	cursors = calloc(1, sizeof(*cursors));
	if (cursors == NULL)
		return false;

	/* Items with both lines */
	for (i = 0; i < ctx->count; i++) {
		struct grid_item *item = &ctx->items[i];

		if (item->start[0] < 0 || item->start[1] < 0)
			continue;

		if (!grid_area_occupy(&cells, counts[fix], &grown,
				item->start[fix], item->start[grow],
				item->span[fix], item->span[grow]))
			goto error;
	}

	/* Items locked to a line of the growing axis */
	for (i = 0; i < ctx->count; i++) {
		struct grid_item *item = &ctx->items[i];
		int g = item->start[grow];
		int f;

		if (g < 0 || item->start[fix] >= 0)
			continue;

		if (g >= grown && !grid_area_occupy(&cells, counts[fix],
				&grown, 0, g, 0, 1))
			goto error;

		if (g >= ctx->axis[grow].count) {
			int *c = realloc(cursors, (g + 1) * sizeof(*c));
			if (c == NULL)
				goto error;
			memset(c + ctx->axis[grow].count, 0, (g + 1 -
				ctx->axis[grow].count) * sizeof(*c));
			cursors = c;
			ctx->axis[grow].count = g + 1;
		}

		for (f = ctx->dense ? 0 : cursors[g];
				f + item->span[fix] <= counts[fix]; f++) {
			if (grid_area_free(cells, counts[fix], grown, f, g,
					item->span[fix], item->span[grow]))
				break;
		}

		if (f + item->span[fix] > counts[fix]) {
			/* No room: the grid gains tracks along the fix axis
			 * only for items placed there, so overlap */
			f = 0;
		}

		item->start[fix] = f;
		cursors[g] = f + item->span[fix];
		if (!grid_area_occupy(&cells, counts[fix], &grown, f, g,
				item->span[fix], item->span[grow]))
			goto error;
	}

	/* The rest */
	for (i = 0; i < ctx->count; i++) {
		struct grid_item *item = &ctx->items[i];
		int f = item->start[fix];
		int g;

		if (item->start[grow] >= 0)
			continue;

		if (f >= 0) {
			/* A line in the fixed axis */
			if (ctx->dense) {
				g = 0;
			} else {
				g = cursor_grow;
				if (f < cursor_fix)
					g++;
			}

			while (!grid_area_free(cells, counts[fix], grown, f, g,
					item->span[fix], item->span[grow]))
				g++;

			cursor_fix = f;
		} else {
			if (ctx->dense) {
				f = 0;
				g = 0;
			} else {
				f = cursor_fix;
				g = cursor_grow;
			}

			while (true) {
				if (f + item->span[fix] > counts[fix]) {
					f = 0;
					g++;
					continue;
				}
				if (grid_area_free(cells, counts[fix], grown,
						f, g, item->span[fix],
						item->span[grow]))
					break;
				f++;
			}

			cursor_fix = f + item->span[fix];
		}

		cursor_grow = g;
		item->start[fix] = f;
		item->start[grow] = g;
		if (!grid_area_occupy(&cells, counts[fix], &grown, f, g,
				item->span[fix], item->span[grow]))
			goto error;
	}

	counts[grow] = max(counts[grow], grown);
	for (i = 0; i < ctx->count; i++) {
		struct grid_item *item = &ctx->items[i];

		if (item->start[grow] + item->span[grow] > counts[grow])
			counts[grow] = item->start[grow] + item->span[grow];
	}

	ctx->axis[0].count = counts[0];
	ctx->axis[1].count = counts[1];

	free(cells);
	free(cursors);
	return true;

error:
	free(cells);
	free(cursors);
	return false;
}


/**
 * Find the sizing functions of a track of the implicit grid.
 */
static const css_computed_grid_track *grid_track_size(
		const struct grid_axis *axis, int index)
{
	int j = index - axis->before;

	if (j >= 0 && j < axis->template_count)
		return axis->sizes[j];

	if (axis->auto_count == 0)
		return &grid_auto_track;

	if (j >= axis->template_count) {
		j -= axis->template_count;
		return &axis->auto_sizes[j % axis->auto_count];
	}

	/* Tracks before the explicit grid take the sizes backwards */
	j = -j;
	return &axis->auto_sizes[(axis->auto_count - j % axis->auto_count) %
			axis->auto_count];
}


/**
 * Set up the tracks of an axis of the implicit grid.
 *
 * \return true on success, false on memory exhaustion
 */
static bool grid_axis_tracks(struct grid_ctx *ctx, struct grid_axis *axis,
		int a)
{
	int i;
	size_t j;

	axis->tracks = calloc(max(axis->count, 1), sizeof(*axis->tracks));
	if (axis->tracks == NULL)
		return false;

	for (i = 0; i < axis->count; i++) {
		const css_computed_grid_track *size = grid_track_size(axis, i);
		int e = i - axis->before;

		grid_track_init(ctx, size != NULL ? size : &grid_auto_track,
				axis->available, &axis->tracks[i]);
		axis->tracks[i].fit = e >= 0 && e < axis->explicit_count &&
				axis->fit[e];
		axis->tracks[i].collapsed = axis->tracks[i].fit;
	}

	/* An auto-fit track with nothing in it collapses */
	for (j = 0; j < ctx->count; j++) {
		const struct grid_item *item = &ctx->items[j];

		for (i = item->start[a]; i < item->start[a] + item->span[a] &&
				i < axis->count; i++)
			axis->tracks[i].collapsed = false;
	}

	return true;
}


/**
 * Find the size of the gaps between some tracks.
 *
 * \param[in] axis   The axis
 * \param[in] start  First track
 * \param[in] span   Number of tracks
 * \return the size of the gaps between the tracks which are not collapsed
 */
static int grid_gaps(const struct grid_axis *axis, int start, int span)
{
	int count = 0;
	int i;

	for (i = start; i < start + span && i < axis->count; i++) {
		if (axis->tracks[i].collapsed == false)
			count++;
	}

	return count > 1 ? (count - 1) * axis->gap : 0;
}


/**
 * Check whether a track's sizing function is intrinsic.
 */
static inline bool grid_is_intrinsic(uint8_t type)
{
	return type == GRID_AUTO || type == GRID_MIN_CONTENT ||
	       type == GRID_MAX_CONTENT || type == GRID_FIT_CONTENT;
}


/**
 * Check whether an item spans a flexible track.
 */
static bool grid_item_spans_flex(const struct grid_axis *axis,
		const struct grid_item *item, int a)
{
	int i;

	for (i = item->start[a]; i < item->start[a] + item->span[a]; i++) {
		if (axis->tracks[i].max_type == GRID_FLEX)
			return true;
	}

	return false;
}


/**
 * Make tracks' base sizes or growth limits big enough for an item which
 * spans them.
 *
 * \param[in] axis    The axis
 * \param[in] item    The item
 * \param[in] a       Index of the axis
 * \param[in] size    The size the item needs
 * \param[in] limits  Whether to grow growth limits, rather than base sizes
 * \param[in] flex    Whether to grow only flexible tracks
 */
static void grid_distribute(struct grid_axis *axis,
		const struct grid_item *item, int a, int size, bool limits,
		bool flex)
{
	int start = item->start[a];
	int end = start + item->span[a];
	int have = grid_gaps(axis, start, item->span[a]);
	int count = 0;
	int extra, i;

	for (i = start; i < end; i++) {
		const struct grid_track *t = &axis->tracks[i];
		bool grows;

		if (t->collapsed)
			continue;

		if (limits) {
			have += t->limit == GRID_UNSET ? t->base : t->limit;
			grows = grid_is_intrinsic(t->max_type);
		} else {
			have += t->base;
			grows = grid_is_intrinsic(t->min_type) &&
					(!flex || t->max_type == GRID_FLEX);
		}

		if (grows)
			count++;
	}

	extra = size - have;
	if (extra <= 0 || count == 0)
		return;

	for (i = start; i < end; i++) {
		struct grid_track *t = &axis->tracks[i];
		int share;

		if (t->collapsed)
			continue;

		if (limits ? !grid_is_intrinsic(t->max_type) :
				!(grid_is_intrinsic(t->min_type) &&
				  (!flex || t->max_type == GRID_FLEX)))
			continue;

		share = extra / count;
		extra -= share;
		count--;

		if (limits) {
			if (t->limit == GRID_UNSET)
				t->limit = t->base;
			t->limit += share;
		} else {
			t->base += share;
		}
	}
}


/**
 * Find the size of the free space of an axis.
 *
 * \return the space left after the base sizes and gaps, or AUTO
 */
static int grid_free_space(const struct grid_axis *axis)
{
	int used = grid_gaps(axis, 0, axis->count);
	int i;

	if (axis->available == AUTO)
		return AUTO;

	for (i = 0; i < axis->count; i++)
		used += axis->tracks[i].base;

	return axis->available - used;
}


/**
 * Find the size of one fr of the flexible tracks of an axis.
 *
 * \param[in] ctx          Grid layout context
 * \param[in] axis         The axis
 * \param[in] a            Index of the axis
 * \param[in] min_content  Whether sizing for the min-content size
 * \return the size of 1fr in pixels
 */
static float grid_fr_size(const struct grid_ctx *ctx,
		const struct grid_axis *axis, int a, bool min_content)
{
	float fr = 0;
	size_t j;
	int i;

	if (axis->available != AUTO && !min_content) {
		/* Share the space, leaving out tracks which are bigger
		 * than their share */
		bool *fixed = calloc(axis->count, sizeof(*fixed));
		bool changed = true;

		if (fixed == NULL)
			return 0;

		while (changed) {
			int left = axis->available -
					grid_gaps(axis, 0, axis->count);
			float factors = 0;

			for (i = 0; i < axis->count; i++) {
				const struct grid_track *t = &axis->tracks[i];

				if (t->max_type == GRID_FLEX && !fixed[i])
					factors += t->flex;
				else
					left -= t->base;
			}

			fr = left / (factors < 1 ? 1 : factors);
			changed = false;

			for (i = 0; i < axis->count; i++) {
				const struct grid_track *t = &axis->tracks[i];

				if (t->max_type == GRID_FLEX && !fixed[i] &&
						t->base > fr * t->flex) {
					fixed[i] = true;
					changed = true;
				}
			}
		}

		free(fixed);
		return max(fr, 0);
	}

	/* Indefinite: 1fr is big enough for every track and item */
	for (i = 0; i < axis->count; i++) {
		const struct grid_track *t = &axis->tracks[i];

		if (t->max_type == GRID_FLEX && t->flex > 0)
			fr = max(fr, t->base / (t->flex < 1 ? 1 : t->flex));
	}

	for (j = 0; j < ctx->count; j++) {
		const struct grid_item *item = &ctx->items[j];
		int size = min_content ? item->min_contribution[a] :
				item->max_contribution[a];
		float factors = 0;

		if (!grid_item_spans_flex(axis, item, a))
			continue;

		size -= grid_gaps(axis, item->start[a], item->span[a]);
		for (i = item->start[a]; i < item->start[a] + item->span[a];
				i++) {
			const struct grid_track *t = &axis->tracks[i];

			if (t->max_type == GRID_FLEX)
				factors += t->flex;
			else
				size -= t->base;
		}

		if (factors > 0)
			fr = max(fr, size / (factors < 1 ? 1 : factors));
	}

	return fr;
}


/**
 * Size the tracks of an axis.
 *
 * \param[in] ctx          Grid layout context
 * \param[in] a            Index of the axis
 * \param[in] min_content  Whether sizing for the min-content size, where
 *                         items give only their min-content contributions
 */
static void grid_size_tracks(struct grid_ctx *ctx, int a, bool min_content)
{
	struct grid_axis *axis = &ctx->axis[a];
	int max_span = 1;
	int free_space;
	bool have_flex = false;
	size_t j;
	int i, span;

	for (i = 0; i < axis->count; i++) {
		struct grid_track *t = &axis->tracks[i];

		t->base = t->min_type == GRID_FIXED ? t->min : 0;
		t->limit = t->max_type == GRID_FIXED ? t->max : GRID_UNSET;
		if (t->limit != GRID_UNSET && t->limit < t->base)
			t->limit = t->base;

		if (t->max_type == GRID_FLEX)
			have_flex = true;

		if (t->collapsed) {
			t->base = 0;
			t->limit = 0;
		}
	}

	/* Items in one track */
	for (j = 0; j < ctx->count; j++) {
		const struct grid_item *item = &ctx->items[j];
		struct grid_track *t;
		int cmin = item->min_contribution[a];
		int cmax = min_content ? cmin : item->max_contribution[a];

		if (item->span[a] > max_span)
			max_span = item->span[a];

		if (item->span[a] != 1)
			continue;

		t = &axis->tracks[item->start[a]];
		if (t->collapsed)
			continue;

		if (t->min_type == GRID_AUTO || t->min_type == GRID_MIN_CONTENT)
			t->base = max(t->base, cmin);
		else if (t->min_type == GRID_MAX_CONTENT)
			t->base = max(t->base, cmax);

		switch (t->max_type) {
		case GRID_MIN_CONTENT:
			t->limit = max(t->limit, cmin);
			break;
		case GRID_AUTO:
		case GRID_MAX_CONTENT:
			t->limit = max(t->limit, cmax);
			break;
		case GRID_FIT_CONTENT:
			t->limit = max(t->limit, min(cmax, max(cmin, t->max)));
			break;
		default:
			break;
		}
	}

	/* Items spanning several tracks, smallest spans first */
	for (span = 2; span <= max_span; span++) {
		for (j = 0; j < ctx->count; j++) {
			const struct grid_item *item = &ctx->items[j];
			int cmin = item->min_contribution[a];
			int cmax = min_content ? cmin :
					item->max_contribution[a];

			if (item->span[a] != span ||
					grid_item_spans_flex(axis, item, a))
				continue;

			grid_distribute(axis, item, a, cmin, false, false);
			grid_distribute(axis, item, a, cmax, true, false);
		}
	}

	/* Items spanning several tracks, some flexible, need room in the
	 * flexible tracks' minimums */
	for (j = 0; j < ctx->count; j++) {
		const struct grid_item *item = &ctx->items[j];

		if (item->span[a] > 1 && grid_item_spans_flex(axis, item, a))
			grid_distribute(axis, item, a,
					item->min_contribution[a],
					false, true);
	}

	for (i = 0; i < axis->count; i++) {
		struct grid_track *t = &axis->tracks[i];

		if (t->limit == GRID_UNSET || t->limit < t->base)
			t->limit = t->base;
	}

	/* Grow the tracks to their limits */
	if (!min_content) {
		free_space = grid_free_space(axis);

		if (free_space == AUTO) {
			for (i = 0; i < axis->count; i++) {
				struct grid_track *t = &axis->tracks[i];

				if (t->max_type != GRID_FLEX)
					t->base = t->limit;
			}
		} else {
			while (free_space > 0) {
				int count = 0;
				int share;

				for (i = 0; i < axis->count; i++) {
					struct grid_track *t =
							&axis->tracks[i];
					if (t->max_type != GRID_FLEX &&
							t->base < t->limit)
						count++;
				}

				if (count == 0)
					break;

				share = max(free_space / count, 1);
				for (i = 0; i < axis->count &&
						free_space > 0; i++) {
					struct grid_track *t =
							&axis->tracks[i];
					int grow;

					if (t->max_type == GRID_FLEX ||
							t->base >= t->limit)
						continue;

					grow = min(min(share,
							t->limit - t->base),
							free_space);
					t->base += grow;
					free_space -= grow;
				}
			}
		}
	}

	/* Flexible tracks */
	if (have_flex) {
		float fr = grid_fr_size(ctx, axis, a, min_content);

		for (i = 0; i < axis->count; i++) {
			struct grid_track *t = &axis->tracks[i];

			if (t->max_type == GRID_FLEX && !t->collapsed &&
					t->base < fr * t->flex)
				t->base = fr * t->flex;
		}
	}
}


/**
 * Stretch auto tracks to fill the space, and place the tracks.
 *
 * \param[in] axis     The axis
 * \param[in] align    The container's justify-content or align-content, as
 *                     a css_align_content_e
 */
static void grid_place_tracks(struct grid_axis *axis,
		enum css_align_content_e align)
{
	int free_space = grid_free_space(axis);
	int offset = 0, spacing = 0, remainder = 0;
	int count = 0;
	int pos, i;

	for (i = 0; i < axis->count; i++) {
		if (axis->tracks[i].collapsed == false)
			count++;
	}

	if (free_space != AUTO && free_space > 0 &&
			align == CSS_ALIGN_CONTENT_STRETCH) {
		int autos = 0;

		for (i = 0; i < axis->count; i++) {
			if (axis->tracks[i].max_type == GRID_AUTO &&
					!axis->tracks[i].collapsed)
				autos++;
		}

		for (i = 0; i < axis->count && autos > 0; i++) {
			struct grid_track *t = &axis->tracks[i];
			int share;

			if (t->max_type != GRID_AUTO || t->collapsed)
				continue;

			share = free_space / autos;
			t->base += share;
			free_space -= share;
			autos--;
		}
	}

	if (free_space != AUTO && free_space > 0 && count > 0) {
		switch (align) {
		case CSS_ALIGN_CONTENT_CENTER:
			offset = free_space / 2;
			break;
		case CSS_ALIGN_CONTENT_FLEX_END:
			offset = free_space;
			break;
		case CSS_ALIGN_CONTENT_SPACE_BETWEEN:
			if (count > 1) {
				spacing = free_space / (count - 1);
				remainder = free_space % (count - 1);
			}
			break;
		case CSS_ALIGN_CONTENT_SPACE_AROUND:
			spacing = free_space / count;
			offset = spacing / 2;
			break;
		case CSS_ALIGN_CONTENT_SPACE_EVENLY:
			spacing = free_space / (count + 1);
			offset = spacing;
			break;
		default:
			break;
		}
	}

	pos = offset;
	for (i = 0; i < axis->count; i++) {
		struct grid_track *t = &axis->tracks[i];

		t->pos = pos;
		if (t->collapsed)
			continue;

		pos += t->base + axis->gap + spacing;
		if (remainder > 0) {
			pos++;
			remainder--;
		}
	}
}


/**
 * Find the size of a grid area along one axis.
 */
static int grid_area_size(const struct grid_axis *axis, int start, int span)
{
	int size = grid_gaps(axis, start, span);
	int i;

	for (i = start; i < start + span; i++) {
		if (axis->tracks[i].collapsed == false)
			size += axis->tracks[i].base;
	}

	return size;
}


/**
 * Find the size of all the tracks and gaps of an axis.
 */
static int grid_axis_size(const struct grid_axis *axis)
{
	return grid_area_size(axis, 0, axis->count);
}


/**
 * Lay out a grid item at its width.
 *
 * \param[in] ctx  Grid layout context
 * \param[in] b    The item
 * \return true on success, false on memory exhaustion
 */
static bool grid_layout_item(const struct grid_ctx *ctx, struct box *b)
{
	bool success;

	switch (b->type) {
	case BOX_BLOCK:
		success = layout_block_context(b, -1, ctx->content);
		break;
	case BOX_TABLE:
		b->float_container = b->parent;
		success = layout_table(b, b->width, ctx->content);
		b->float_container = NULL;
		break;
	case BOX_FLEX:
		b->float_container = b->parent;
		success = layout_flex(b, b->width, ctx->content);
		b->float_container = NULL;
		break;
	default:
		success = true;
		break;
	}

	if (!success) {
		NSLOG(layout, ERROR, "box %p: grid item layout failed", b);
	}

	return success;
}


/**
 * Free a grid layout context.
 */
static void grid_ctx_destroy(struct grid_ctx *ctx)
{
	int a;

	for (a = 0; a < 2; a++) {
		free(ctx->axis[a].tracks);
		free(ctx->axis[a].sizes);
		free(ctx->axis[a].fit);
		free(ctx->axis[a].names);
	}

	free(ctx->items);
}


/**
 * Set up a grid layout context and place the items.
 *
 * \param[in] ctx        Grid layout context to set up
 * \param[in] grid       The grid container
 * \param[in] content    The HTML content, for lengths
 * \param[in] width      Width of the content box, or AUTO
 * \param[in] height     Height of the content box, or AUTO
 * \return true on success, false on memory exhaustion
 */
static bool grid_ctx_init(struct grid_ctx *ctx, struct box *grid,
		const html_content *content, int width, int height)
{
	const css_computed_style *style = grid->style;
	const css_computed_grid_track *tracks;
	const css_computed_grid_area *areas = NULL;
	css_fixed length = 0;
	css_unit unit = CSS_UNIT_PX;
	struct box *b;
	size_t n = 0;
	int a;

	memset(ctx, 0, sizeof(*ctx));
	ctx->grid = grid;
	ctx->unit_len_ctx = &content->unit_len_ctx;

	switch (css_computed_grid_auto_flow(style)) {
	case CSS_GRID_AUTO_FLOW_COLUMN:
		ctx->column_flow = true;
		break;
	case CSS_GRID_AUTO_FLOW_ROW_DENSE:
		ctx->dense = true;
		break;
	case CSS_GRID_AUTO_FLOW_COLUMN_DENSE:
		ctx->column_flow = true;
		ctx->dense = true;
		break;
	default:
		break;
	}

	ctx->axis[0].available = width;
	ctx->axis[1].available = height;

	if (css_computed_column_gap(style, &length, &unit) ==
			CSS_COLUMN_GAP_SET) {
		grid_length(ctx, length, unit, width, &ctx->axis[0].gap);
	}
	if (css_computed_row_gap(style, &length, &unit) ==
			CSS_ROW_GAP_SET) {
		grid_length(ctx, length, unit, height, &ctx->axis[1].gap);
	}

	if (css_computed_grid_template_areas(style, &areas) !=
			CSS_GRID_TEMPLATE_AREAS_SET)
		areas = NULL;

	for (a = 0; a < 2; a++) {
		struct grid_axis *axis = &ctx->axis[a];
		const css_computed_grid_track *t;
		uint8_t type;

		type = a == 0 ?
			css_computed_grid_template_columns(style, &tracks) :
			css_computed_grid_template_rows(style, &tracks);
		if (type != CSS_GRID_TEMPLATE_SET)
			tracks = NULL;

		if (!grid_axis_explicit(ctx, axis, tracks, areas, a == 1))
			return false;

		type = a == 0 ?
			css_computed_grid_auto_columns(style, &tracks) :
			css_computed_grid_auto_rows(style, &tracks);
		if (type == CSS_GRID_AUTO_SET && tracks != NULL) {
			axis->auto_sizes = tracks;
			for (t = tracks; t->kind !=
					CSS_COMPUTED_GRID_TRACK_END; t++)
				axis->auto_count++;
		}
	}

	for (b = grid->children; b != NULL; b = b->next) {
		if (!lh__box_is_absolute(b))
			n++;
	}

	ctx->items = calloc(max(n, 1), sizeof(*ctx->items));
	if (ctx->items == NULL)
		return false;

	for (b = grid->children; b != NULL; b = b->next) {
		struct grid_item *item;

		if (lh__box_is_absolute(b))
			continue;

		item = &ctx->items[ctx->count++];
		item->box = b;
		item->min_contribution[0] = b->min_width;
		item->max_contribution[0] = b->max_width;
		css_computed_order(b->style, &item->order);
	}

	/* Items are placed in order of their order property, and then in
	 * document order */
	for (n = 1; n < ctx->count; n++) {
		struct grid_item item = ctx->items[n];
		size_t j = n;

		while (j > 0 && ctx->items[j - 1].order > item.order) {
			ctx->items[j] = ctx->items[j - 1];
			j--;
		}
		ctx->items[j] = item;
	}

	if (!grid_place_items(ctx))
		return false;

	for (a = 0; a < 2; a++) {
		if (!grid_axis_tracks(ctx, &ctx->axis[a], a))
			return false;
	}

	return true;
}


/* Exported function documented in html/layout_internal.h */
bool layout_minmax_grid(struct box *grid, const html_content *content,
		int *min_width, int *max_width)
{
	struct grid_ctx ctx;
	bool success = false;

	if (!grid_ctx_init(&ctx, grid, content, AUTO, AUTO))
		goto cleanup;

	grid_size_tracks(&ctx, 0, true);
	*min_width = grid_axis_size(&ctx.axis[0]);

	grid_size_tracks(&ctx, 0, false);
	*max_width = max(grid_axis_size(&ctx.axis[0]), *min_width);

	success = true;

cleanup:
	grid_ctx_destroy(&ctx);
	return success;
}


/**
 * Work out the width of an item and lay it out.
 *
 * \param[in] ctx   Grid layout context
 * \param[in] item  The item
 * \return true on success, false on memory exhaustion
 */
static bool grid_layout_item_width(struct grid_ctx *ctx,
		struct grid_item *item)
{
	struct box *b = item->box;
	int area = grid_area_size(&ctx->axis[0], item->start[0],
			item->span[0]);
	int max_width, min_width, max_height, min_height;
	enum css_align_self_e justify;

	b->float_container = b->parent;
	layout_find_dimensions(ctx->unit_len_ctx, area, -1, b, b->style,
			&b->width, &b->height, &max_width, &min_width,
			&max_height, &min_height,
			b->margin, b->padding, b->border);
	b->float_container = NULL;

	justify = css_computed_justify_self(b->style);
	if (justify == (enum css_align_self_e) CSS_JUSTIFY_SELF_AUTO)
		justify = css_computed_justify_items(ctx->grid->style);

	if (b->width == AUTO) {
		int outer = area;

		if (justify != (enum css_align_self_e)
				CSS_JUSTIFY_SELF_STRETCH ||
				b->margin[LEFT] == AUTO ||
				b->margin[RIGHT] == AUTO) {
			/* Shrink to fit */
			outer = min(max(b->min_width, area), b->max_width);
		}

		b->width = max(outer - lh__delta_outer_width(b), 0);
	}

	if (max_width >= 0 && b->width > max_width)
		b->width = max_width;
	if (min_width > 0 && b->width < min_width)
		b->width = min_width;

	if (b->height != AUTO) {
		if (max_height >= 0 && b->height > max_height)
			b->height = max_height;
		if (min_height > 0 && b->height < min_height)
			b->height = min_height;
	}

	if (!grid_layout_item(ctx, b))
		return false;

	if (max_height >= 0 && b->height > max_height)
		b->height = max_height;
	if (min_height > 0 && b->height < min_height)
		b->height = min_height;

	item->min_contribution[1] = b->height + lh__delta_outer_height(b);
	item->max_contribution[1] = item->min_contribution[1];

	return true;
}


/**
 * Find where a box goes in its area along one axis.
 *
 * \param[in] size       The size of the area
 * \param[in] outer      The size of the box with its margins
 * \param[in] align      The alignment, as a css_align_self_e
 * \param[in] auto_start Whether the start margin is auto
 * \param[in] auto_end   Whether the end margin is auto
 * \return the offset of the box's margin edge in the area
 */
static int grid_align(int size, int outer, enum css_align_self_e align,
		bool auto_start, bool auto_end)
{
	int free_space = size - outer;

	if (auto_start && auto_end)
		return max(free_space / 2, 0);
	if (auto_start)
		return max(free_space, 0);
	if (auto_end)
		return 0;

	switch (align) {
	case CSS_ALIGN_SELF_FLEX_END:
		return free_space;
	case CSS_ALIGN_SELF_CENTER:
		return free_space / 2;
	default:
		return 0;
	}
}


/**
 * Position an item in its area, stretching it if it should fill it.
 *
 * \param[in] ctx   Grid layout context
 * \param[in] item  The item
 * \return true on success, false on memory exhaustion
 */
static bool grid_position_item(struct grid_ctx *ctx,
		const struct grid_item *item)
{
	struct box *b = item->box;
	const struct grid_axis *columns = &ctx->axis[0];
	const struct grid_axis *rows = &ctx->axis[1];
	int width = grid_area_size(columns, item->start[0], item->span[0]);
	int height = grid_area_size(rows, item->start[1], item->span[1]);
	enum css_align_self_e justify, align;
	bool auto_left = b->margin[LEFT] == AUTO;
	bool auto_right = b->margin[RIGHT] == AUTO;
	bool auto_top = b->margin[TOP] == AUTO;
	bool auto_bottom = b->margin[BOTTOM] == AUTO;
	int x, y;

	justify = css_computed_justify_self(b->style);
	if (justify == (enum css_align_self_e) CSS_JUSTIFY_SELF_AUTO)
		justify = css_computed_justify_items(ctx->grid->style);

	/* justify-self's values are mapped onto align-self's */
	switch ((enum css_justify_self_e) justify) {
	case CSS_JUSTIFY_SELF_START:
		justify = CSS_ALIGN_SELF_FLEX_START;
		break;
	case CSS_JUSTIFY_SELF_END:
		justify = CSS_ALIGN_SELF_FLEX_END;
		break;
	case CSS_JUSTIFY_SELF_CENTER:
		justify = CSS_ALIGN_SELF_CENTER;
		break;
	default:
		justify = CSS_ALIGN_SELF_STRETCH;
		break;
	}

	align = lh__box_align_self(ctx->grid, b);

	if (align == CSS_ALIGN_SELF_STRETCH && !auto_top && !auto_bottom &&
			lh__box_size_cross_is_auto(true, b)) {
		int stretched = height - lh__delta_outer_height(b);

		if (stretched > b->height) {
			b->height = stretched;

			/* A flex or grid container lays out its items to
			 * fill the new height */
			if (b->type == BOX_FLEX) {
				if (!grid_layout_item(ctx, b))
					return false;
				b->height = max(b->height, stretched);
			}
		}
	}

	x = grid_align(width, b->width + lh__delta_outer_width(b), justify,
			auto_left, auto_right);
	y = grid_align(height, b->height + lh__delta_outer_height(b), align,
			auto_top, auto_bottom);

	if (auto_left)
		b->margin[LEFT] = 0;
	if (auto_right)
		b->margin[RIGHT] = 0;
	if (auto_top)
		b->margin[TOP] = 0;
	if (auto_bottom)
		b->margin[BOTTOM] = 0;

	b->x = ctx->grid->padding[LEFT] + columns->tracks[item->start[0]].pos +
			x + b->margin[LEFT] + b->border[LEFT].width;
	b->y = ctx->grid->padding[TOP] + rows->tracks[item->start[1]].pos +
			y + b->margin[TOP] + b->border[TOP].width;

	return true;
}


/**
 * Lay out the absolutely positioned children of a grid at their static
 * position, the start of the content box.
 *
 * \return true on success, false on memory exhaustion
 */
static bool grid_layout_absolute(struct grid_ctx *ctx)
{
	struct box *grid = ctx->grid;
	struct box *b;

	for (b = grid->children; b != NULL; b = b->next) {
		int max_width, min_width;

		if (!lh__box_is_absolute(b))
			continue;

		b->float_container = b->parent;
		layout_find_dimensions(ctx->unit_len_ctx, grid->width, -1,
				b, b->style, &b->width, &b->height,
				&max_width, &min_width, NULL, NULL,
				b->margin, b->padding, b->border);
		b->float_container = NULL;

		if (b->width == AUTO)
			b->width = max(min(b->max_width, grid->width) -
					lh__delta_outer_width(b), 0);
		if (max_width >= 0 && b->width > max_width)
			b->width = max_width;
		if (min_width > 0 && b->width < min_width)
			b->width = min_width;

		if (!grid_layout_item(ctx, b))
			return false;

		b->x = grid->padding[LEFT] + lh__non_auto_margin(b, LEFT) +
				b->border[LEFT].width;
		b->y = grid->padding[TOP] + lh__non_auto_margin(b, TOP) +
				b->border[TOP].width;
	}

	return true;
}


/* Exported function documented in html/layout_internal.h */
bool layout_grid(struct box *grid, int available_width,
		html_content *content)
{
	struct grid_ctx ctx;
	int max_height, min_height;
	enum css_align_content_e justify_content, align_content;
	bool success = false;
	size_t i;

	layout_find_dimensions(&content->unit_len_ctx, available_width, -1,
			grid, grid->style, NULL, &grid->height,
			NULL, NULL, &max_height, &min_height,
			grid->margin, grid->padding, grid->border);

	if (!grid_ctx_init(&ctx, grid, content, grid->width, grid->height))
		goto cleanup;
	ctx.content = content;

	/* Columns, from the items' widths */
	grid_size_tracks(&ctx, 0, false);

	switch (css_computed_justify_content(grid->style)) {
	case CSS_JUSTIFY_CONTENT_FLEX_END:
		justify_content = CSS_ALIGN_CONTENT_FLEX_END;
		break;
	case CSS_JUSTIFY_CONTENT_CENTER:
		justify_content = CSS_ALIGN_CONTENT_CENTER;
		break;
	case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
		justify_content = CSS_ALIGN_CONTENT_SPACE_BETWEEN;
		break;
	case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
		justify_content = CSS_ALIGN_CONTENT_SPACE_AROUND;
		break;
	case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
		justify_content = CSS_ALIGN_CONTENT_SPACE_EVENLY;
		break;
	default:
		/* The initial value, which is normal, stretches */
		justify_content = CSS_ALIGN_CONTENT_STRETCH;
		break;
	}
	grid_place_tracks(&ctx.axis[0], justify_content);

	/* Rows, from the items' heights at their widths */
	for (i = 0; i < ctx.count; i++) {
		if (!grid_layout_item_width(&ctx, &ctx.items[i]))
			goto cleanup;
	}

	grid_size_tracks(&ctx, 1, false);

	align_content = css_computed_align_content(grid->style);
	grid_place_tracks(&ctx.axis[1], align_content);

	for (i = 0; i < ctx.count; i++) {
		if (!grid_position_item(&ctx, &ctx.items[i]))
			goto cleanup;
	}

	if (!grid_layout_absolute(&ctx))
		goto cleanup;

	if (grid->height == AUTO) {
		int rows = ctx.axis[1].count;

		grid->height = rows > 0 ?
				ctx.axis[1].tracks[rows - 1].pos +
				ctx.axis[1].tracks[rows - 1].base : 0;
		grid->height = max(grid->height, 0);
	}

	if (max_height >= 0 && grid->height > max_height)
		grid->height = max_height;
	if (min_height > 0 && grid->height < min_height)
		grid->height = min_height;

	success = true;

cleanup:
	grid_ctx_destroy(&ctx);

	NSLOG(layout, DEEPDEBUG, "grid %p: %s: w: %i, h: %i", grid,
			success ? "success" : "failure",
			grid->width, grid->height);
	return success;
}
