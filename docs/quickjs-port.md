QuickJS port and CSS work
=========================

This branch (`quickjs-engine`) replaces NetSurf's Duktape JavaScript engine
with QuickJS-ng as an option, and adds a number of CSS features, mainly to
make modern sites usable on RISC OS on a Raspberry Pi.  It is offered to the
NetSurf project for whatever parts the developers want, and is not meant as a
replacement for NetSurf.

JavaScript
----------

* `NETSURF_USE_QUICKJS=YES` (and `NETSURF_USE_DUKTAPE=NO`) selects the engine.
  The engine is in `content/handlers/javascript/quickjs/engine`, under its own
  MIT licence.  The default build is unchanged.
* `content/handlers/javascript/quickjs/binding.bnd` and `qjsky.c` give the
  bindings; `duk_compat.c` lets the existing generated bindings run on it.
* `webcompat.js` holds web platform interfaces which are easier to write in
  JavaScript: `fetch`, `XMLHttpRequest`, `URL`, `CSSStyleDeclaration`,
  `CSS.supports()`, `getComputedStyle()` and others.
* Pages are rebuilt after script changes (`html/rebuild.c`), so scripts can
  change the document.

CSS and layout
--------------

These need the matching changes in libcss (branch `custom-properties`) and
libhubbub (branch `void-source-track`).

* Custom properties and `var()`, `calc()`, `@supports`.
* CSS grid (`html/layout_grid.c`): placement by lines, names, areas and spans,
  auto-placement, `fr`, `minmax()`, `fit-content()`, `auto-fill`/`auto-fit`,
  gaps and alignment.
* Flexbox: `order`, `gap`, `justify-content`, percentage bases, and column
  heights.
* `border-radius` with clipping, `box-shadow`, `background-size`,
  `overflow-wrap`/`word-break`, `transform` as a translation.

Testing
-------

The test pages and scripts, which run on Linux with AddressSanitizer and
inside RPCEmu for the RISC OS build, are at
<https://github.com/m0oml/netsurf-qjs-tests>.  At the time of writing the
Linux suite passes 660 of 660 pages' checks and the RISC OS suite 646 of 646.
