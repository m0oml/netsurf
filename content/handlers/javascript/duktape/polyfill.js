/* Polyfiller for Duktape for NetSurf
 *
 * This JavaScript will be loaded into heaps before the generics
 *
 * We only care for the side-effects of this, be careful.
 */

// Production steps of ECMA-262, Edition 6, 22.1.2.1
if (!Array.from) {
  Array.from = (function () {
    var toStr = Object.prototype.toString;
    var isCallable = function (fn) {
      return typeof fn === 'function' || toStr.call(fn) === '[object Function]';
    };
    var toInteger = function (value) {
      var number = Number(value);
      if (isNaN(number)) { return 0; }
      if (number === 0 || !isFinite(number)) { return number; }
      return (number > 0 ? 1 : -1) * Math.floor(Math.abs(number));
    };
    var maxSafeInteger = Math.pow(2, 53) - 1;
    var toLength = function (value) {
      var len = toInteger(value);
      return Math.min(Math.max(len, 0), maxSafeInteger);
    };

    // The length property of the from method is 1.
    return function from(arrayLike/*, mapFn, thisArg */) {
      // 1. Let C be the this value.
      var C = this;

      // 2. Let items be ToObject(arrayLike).
      var items = Object(arrayLike);

      // 3. ReturnIfAbrupt(items).
      if (arrayLike == null) {
        throw new TypeError('Array.from requires an array-like object - not null or undefined');
      }

      // 4. If mapfn is undefined, then let mapping be false.
      var mapFn = arguments.length > 1 ? arguments[1] : void undefined;
      var T;
      if (typeof mapFn !== 'undefined') {
        // 5. else
        // 5. a If IsCallable(mapfn) is false, throw a TypeError exception.
        if (!isCallable(mapFn)) {
          throw new TypeError('Array.from: when provided, the second argument must be a function');
        }

        // 5. b. If thisArg was supplied, let T be thisArg; else let T be undefined.
        if (arguments.length > 2) {
          T = arguments[2];
        }
      }

      // 10. Let lenValue be Get(items, "length").
      // 11. Let len be ToLength(lenValue).
      var len = toLength(items.length);

      // 13. If IsConstructor(C) is true, then
      // 13. a. Let A be the result of calling the [[Construct]] internal method 
      // of C with an argument list containing the single item len.
      // 14. a. Else, Let A be ArrayCreate(len).
      var A = isCallable(C) ? Object(new C(len)) : new Array(len);

      // 16. Let k be 0.
      var k = 0;
      // 17. Repeat, while k < len… (also steps a - h)
      var kValue;
      while (k < len) {
        kValue = items[k];
        if (mapFn) {
          A[k] = typeof T === 'undefined' ? mapFn(kValue, k) : mapFn.call(T, kValue, k);
        } else {
          A[k] = kValue;
        }
        k += 1;
      }
      // 18. Let putStatus be Put(A, "length", len, true).
      A.length = len;
      // 20. Return A.
      return A;
    };
  }());
}

// DOMTokenList formatter, in theory we can remove this if we do the stringifier IDL support

DOMTokenList.prototype.toString = function () {
  if (this.length == 0) {
    return "";
  }

  var ret = this.item(0);
  for (var index = 1; index < this.length; index++) {
    ret = ret + " " + this.item(index);
  }

  return ret;
}

// Inherit the same toString for settable lists
DOMSettableTokenList.prototype.toString = DOMTokenList.prototype.toString;
// Selector matching for querySelector(), querySelectorAll(), matches(),
// closest() and getElementsByClassName().
//
// Implements Selectors Level 3 with :is(), :where(), :not() taking a list,
// :scope and :has() from Level 4.  Selectors are parsed into a list of
// complex selectors, each an array of compound selectors with the
// combinator to the next, and matched right to left.

(function () {
  var cache = {};
  var cacheSize = 0;
  var CACHE_MAX = 256;

  // Pseudo-classes for states NetSurf does not track; they never match
  var neverStates = {
    "hover": true, "active": true, "focus": true, "focus-within": true,
    "focus-visible": true, "visited": true, "target": true,
    "target-within": true, "fullscreen": true, "modal": true,
    "popover-open": true, "playing": true, "paused": true,
    "autofill": true, "user-invalid": true, "user-valid": true,
    "indeterminate": true, "default": true, "invalid": true,
    "valid": true, "in-range": true, "out-of-range": true,
    "placeholder-shown": true, "blank": true, "current": true,
    "past": true, "future": true
  };

  var legacyPseudoElements = {
    "before": true, "after": true, "first-line": true, "first-letter": true
  };

  var formControls = {
    "input": true, "button": true, "select": true, "textarea": true,
    "option": true, "optgroup": true, "fieldset": true
  };

  function invalid(text) {
    return new SyntaxError("'" + text + "' is not a valid selector");
  }

  function isWhitespace(c) {
    return c === " " || c === "\t" || c === "\n" || c === "\r" || c === "\f";
  }

  function isNameChar(c) {
    return (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") ||
      (c >= "0" && c <= "9") || c === "-" || c === "_" || c > "\x7f";
  }

  function Parser(text) {
    this.text = text;
    this.pos = 0;
  }

  Parser.prototype.peek = function (offset) {
    return this.text.charAt(this.pos + (offset || 0));
  };

  Parser.prototype.eof = function () {
    return this.pos >= this.text.length;
  };

  Parser.prototype.skipWhitespace = function () {
    var skipped = false;
    while (!this.eof() && isWhitespace(this.peek())) {
      this.pos++;
      skipped = true;
    }
    return skipped;
  };

  Parser.prototype.fail = function () {
    throw invalid(this.text);
  };

  Parser.prototype.expect = function (c) {
    if (this.peek() !== c) {
      this.fail();
    }
    this.pos++;
  };

  Parser.prototype.escape = function () {
    // the backslash has been consumed
    var hex = "";
    while (hex.length < 6 && /[0-9a-fA-F]/.test(this.peek())) {
      hex += this.peek();
      this.pos++;
    }
    if (hex.length > 0) {
      if (isWhitespace(this.peek())) {
        this.pos++;
      }
      var code = parseInt(hex, 16);
      if (code === 0 || code > 0x10ffff) {
        return "�";
      }
      if (code > 0xffff) {
        code -= 0x10000;
        return String.fromCharCode(0xd800 + (code >> 10),
                                   0xdc00 + (code & 0x3ff));
      }
      return String.fromCharCode(code);
    }
    if (this.eof()) {
      return "�";
    }
    return this.text.charAt(this.pos++);
  };

  Parser.prototype.ident = function () {
    var out = "";
    var c = this.peek();
    if (c === "-") {
      out += c;
      this.pos++;
      c = this.peek();
    }
    if (!(isNameChar(c) && !(c >= "0" && c <= "9")) && c !== "\\" && out !== "-") {
      this.fail();
    }
    while (!this.eof()) {
      c = this.peek();
      if (c === "\\") {
        this.pos++;
        out += this.escape();
      } else if (isNameChar(c)) {
        out += c;
        this.pos++;
      } else {
        break;
      }
    }
    if (out === "" || out === "-") {
      this.fail();
    }
    return out;
  };

  Parser.prototype.string = function () {
    var quote = this.peek();
    var out = "";
    this.pos++;
    while (!this.eof() && this.peek() !== quote) {
      var c = this.peek();
      this.pos++;
      if (c === "\\") {
        if (this.peek() === "\n") {
          this.pos++;
        } else {
          out += this.escape();
        }
      } else {
        out += c;
      }
    }
    this.expect(quote);
    return out;
  };

  // Parse the argument of an :nth-*() pseudo-class
  Parser.prototype.nth = function (allowOf) {
    var start = this.pos;
    var depth = 0;
    while (!this.eof()) {
      var c = this.peek();
      if (c === "(") {
        depth++;
      } else if (c === ")") {
        if (depth === 0) {
          break;
        }
        depth--;
      }
      this.pos++;
    }
    var arg = this.text.substring(start, this.pos);
    var of = null;
    var ofAt = allowOf ? arg.search(/\sof\s/i) : -1;
    if (ofAt >= 0) {
      var sub = new Parser(arg.substring(ofAt + 4));
      of = sub.selectorList(false);
      if (!sub.eof()) {
        this.fail();
      }
      arg = arg.substring(0, ofAt);
    }
    arg = arg.replace(/\s+/g, "").toLowerCase();
    var a, b;
    var m;
    if (arg === "odd") {
      a = 2; b = 1;
    } else if (arg === "even") {
      a = 2; b = 0;
    } else if ((m = /^([+-]?\d*)n([+-]\d+)?$/.exec(arg)) !== null) {
      a = (m[1] === "" || m[1] === "+") ? 1 : (m[1] === "-" ? -1 : parseInt(m[1], 10));
      b = m[2] ? parseInt(m[2], 10) : 0;
    } else if (/^[+-]?\d+$/.test(arg)) {
      a = 0; b = parseInt(arg, 10);
    } else {
      this.fail();
    }
    return { a: a, b: b, of: of };
  };

  Parser.prototype.pseudo = function (compound) {
    this.expect(":");
    var element = false;
    if (this.peek() === ":") {
      this.pos++;
      element = true;
    }
    var name = this.ident().toLowerCase();
    var p = { name: name };
    if (this.peek() === "(") {
      this.pos++;
      this.skipWhitespace();
      switch (name) {
      case "not":
      case "is":
      case "where":
      case "matches":
      case "-webkit-any":
      case "-moz-any":
        p.list = this.selectorList(false);
        break;
      case "has":
        p.list = this.selectorList(true);
        break;
      case "nth-child":
      case "nth-last-child":
        p.nth = this.nth(true);
        break;
      case "nth-of-type":
      case "nth-last-of-type":
        p.nth = this.nth(false);
        break;
      case "lang":
        p.lang = this.ident().toLowerCase();
        break;
      default:
        if (!element) {
          this.fail();
        }
        // skip the argument of a functional pseudo-element
        while (!this.eof() && this.peek() !== ")") {
          this.pos++;
        }
        break;
      }
      this.skipWhitespace();
      this.expect(")");
    }
    if (element || legacyPseudoElements[name]) {
      // pseudo-elements are not elements so cannot match
      compound.never = true;
      return;
    }
    if (p.list === undefined && p.nth === undefined && p.lang === undefined) {
      switch (name) {
      case "root": case "empty": case "scope":
      case "first-child": case "last-child": case "only-child":
      case "first-of-type": case "last-of-type": case "only-of-type":
      case "checked": case "disabled": case "enabled":
      case "link": case "any-link": case "required": case "optional":
      case "read-only": case "read-write": case "defined":
        break;
      default:
        if (!neverStates[name]) {
          this.fail();
        }
        break;
      }
    }
    compound.pseudos.push(p);
  };

  Parser.prototype.attribute = function (compound) {
    this.expect("[");
    this.skipWhitespace();
    var attr = { name: this.ident().toLowerCase(), op: null, value: null, ci: false };
    this.skipWhitespace();
    var c = this.peek();
    if (c === "]") {
      this.pos++;
      compound.attrs.push(attr);
      return;
    }
    if (c === "=") {
      attr.op = "=";
      this.pos++;
    } else if ("~|^$*".indexOf(c) >= 0 && c !== "" && this.peek(1) === "=") {
      attr.op = c + "=";
      this.pos += 2;
    } else {
      this.fail();
    }
    this.skipWhitespace();
    c = this.peek();
    attr.value = (c === "\"" || c === "'") ? this.string() : this.ident();
    this.skipWhitespace();
    c = this.peek().toLowerCase();
    if (c === "i" || c === "s") {
      attr.ci = (c === "i");
      this.pos++;
      this.skipWhitespace();
    }
    this.expect("]");
    compound.attrs.push(attr);
  };

  Parser.prototype.compound = function () {
    var compound = { tag: null, id: null, classes: [], attrs: [], pseudos: [], never: false };
    var start = this.pos;
    var c = this.peek();
    if (c === "*") {
      this.pos++;
    } else if (c !== "" && (isNameChar(c) || c === "\\") && !(c >= "0" && c <= "9")) {
      compound.tag = this.ident().toLowerCase();
    }
    if (this.peek() === "|") {
      // namespace prefixes are accepted and ignored
      this.pos++;
      if (this.peek() === "*") {
        this.pos++;
        compound.tag = null;
      } else {
        compound.tag = this.ident().toLowerCase();
      }
    }
    for (;;) {
      c = this.peek();
      if (c === "#") {
        this.pos++;
        compound.id = this.ident();
      } else if (c === ".") {
        this.pos++;
        compound.classes.push(this.ident());
      } else if (c === "[") {
        this.attribute(compound);
      } else if (c === ":") {
        this.pseudo(compound);
      } else {
        break;
      }
    }
    if (this.pos === start) {
      this.fail();
    }
    return compound;
  };

  // Parse a complex selector.  A relative selector, for :has(), may
  // start with a combinator which is applied to the :has() subject.
  Parser.prototype.complex = function (relative) {
    var parts = [];
    var comb = null;
    this.skipWhitespace();
    if (relative) {
      var c = this.peek();
      comb = " ";
      if (c === ">" || c === "+" || c === "~") {
        comb = c;
        this.pos++;
        this.skipWhitespace();
      }
      parts.push({ compound: null, comb: comb });
    }
    for (;;) {
      var compound = this.compound();
      parts.push({ compound: compound, comb: null });
      var ws = this.skipWhitespace();
      var c = this.peek();
      if (c === ">" || c === "+" || c === "~") {
        this.pos++;
        this.skipWhitespace();
        parts[parts.length - 1].comb = c;
      } else if (ws && c !== "," && c !== ")" && c !== "") {
        parts[parts.length - 1].comb = " ";
      } else {
        break;
      }
    }
    return parts;
  };

  Parser.prototype.selectorList = function (relative) {
    var list = [this.complex(relative)];
    while (this.peek() === ",") {
      this.pos++;
      list.push(this.complex(relative));
    }
    return list;
  };

  function parse(text) {
    text = String(text);
    if (Object.prototype.hasOwnProperty.call(cache, text)) {
      return cache[text];
    }
    var parser = new Parser(text);
    var list = parser.selectorList(false);
    parser.skipWhitespace();
    if (!parser.eof()) {
      parser.fail();
    }
    if (cacheSize >= CACHE_MAX) {
      cache = {};
      cacheSize = 0;
    }
    cache[text] = list;
    cacheSize++;
    return list;
  }

  function localName(el) {
    return String(el.tagName).toLowerCase();
  }

  function hasClass(el, name) {
    var cls = el.getAttribute("class");
    if (cls === null || cls === undefined) {
      return false;
    }
    return (" " + cls.replace(/[ \t\r\n\f]+/g, " ") + " ").indexOf(" " + name + " ") >= 0;
  }

  function matchAttr(el, attr) {
    var v = el.getAttribute(attr.name);
    if (v === null || v === undefined) {
      return false;
    }
    if (attr.op === null) {
      return true;
    }
    var want = attr.value;
    if (attr.ci) {
      v = v.toLowerCase();
      want = want.toLowerCase();
    }
    switch (attr.op) {
    case "=":
      return v === want;
    case "~=":
      return want !== "" && (" " + v.replace(/[ \t\r\n\f]+/g, " ") + " ").indexOf(" " + want + " ") >= 0;
    case "|=":
      return v === want || v.substring(0, want.length + 1) === want + "-";
    case "^=":
      return want !== "" && v.substring(0, want.length) === want;
    case "$=":
      return want !== "" && v.length >= want.length && v.substring(v.length - want.length) === want;
    case "*=":
      return want !== "" && v.indexOf(want) >= 0;
    }
    return false;
  }

  // 1 based index of an element among its siblings which pass filter
  function siblingIndex(el, fromEnd, filter) {
    var idx = 1;
    var sib = fromEnd ? el.nextElementSibling : el.previousElementSibling;
    while (sib) {
      if (filter(sib)) {
        idx++;
      }
      sib = fromEnd ? sib.nextElementSibling : sib.previousElementSibling;
    }
    return idx;
  }

  function nthMatches(nth, idx) {
    if (nth.a === 0) {
      return idx === nth.b;
    }
    var n = (idx - nth.b) / nth.a;
    return n >= 0 && n === Math.floor(n);
  }

  function sameType(el) {
    var name = localName(el);
    return function (sib) { return localName(sib) === name; };
  }

  function anyElement() {
    return true;
  }

  function isEmpty(el) {
    for (var n = el.firstChild; n; n = n.nextSibling) {
      var type = n.nodeType;
      if (type === 1) {
        return false;
      }
      if ((type === 3 || type === 4) && n.nodeValue !== "") {
        return false;
      }
    }
    return true;
  }

  function isLink(el) {
    var name = localName(el);
    return (name === "a" || name === "area" || name === "link") &&
      el.getAttribute("href") !== null;
  }

  function isReadWrite(el) {
    var name = localName(el);
    if (name === "textarea" || name === "input") {
      return el.getAttribute("readonly") === null && el.getAttribute("disabled") === null;
    }
    var editable = el.getAttribute("contenteditable");
    return editable !== null && editable.toLowerCase() !== "false";
  }

  function matchPseudo(el, p, scope) {
    var name = localName(el);
    var parent;
    switch (p.name) {
    case "not":
      return !matchesList(el, p.list, scope);
    case "is":
    case "where":
    case "matches":
    case "-webkit-any":
    case "-moz-any":
      return matchesList(el, p.list, scope);
    case "has":
      return hasMatch(el, p.list);
    case "root":
      return el.parentElement === null && el.ownerDocument !== null &&
        el.ownerDocument.documentElement === el;
    case "scope":
      return scope ? el === scope : (el.ownerDocument !== null && el.ownerDocument.documentElement === el);
    case "empty":
      return isEmpty(el);
    case "first-child":
      return el.previousElementSibling === null;
    case "last-child":
      return el.nextElementSibling === null;
    case "only-child":
      return el.previousElementSibling === null && el.nextElementSibling === null;
    case "first-of-type":
      return siblingIndex(el, false, sameType(el)) === 1;
    case "last-of-type":
      return siblingIndex(el, true, sameType(el)) === 1;
    case "only-of-type":
      return siblingIndex(el, false, sameType(el)) === 1 &&
        siblingIndex(el, true, sameType(el)) === 1;
    case "nth-child":
    case "nth-last-child":
      if (p.nth.of !== null && !matchesList(el, p.nth.of, scope)) {
        return false;
      }
      return nthMatches(p.nth, siblingIndex(el, p.name === "nth-last-child",
        p.nth.of === null ? anyElement : function (sib) { return matchesList(sib, p.nth.of, scope); }));
    case "nth-of-type":
    case "nth-last-of-type":
      return nthMatches(p.nth, siblingIndex(el, p.name === "nth-last-of-type", sameType(el)));
    case "checked":
      if (name === "option") {
        return el.selected === true;
      }
      return name === "input" && el.checked === true;
    case "disabled":
      return formControls[name] === true && el.getAttribute("disabled") !== null;
    case "enabled":
      return formControls[name] === true && el.getAttribute("disabled") === null;
    case "required":
      return (name === "input" || name === "select" || name === "textarea") &&
        el.getAttribute("required") !== null;
    case "optional":
      return (name === "input" || name === "select" || name === "textarea") &&
        el.getAttribute("required") === null;
    case "read-write":
      return isReadWrite(el);
    case "read-only":
      return !isReadWrite(el);
    case "link":
    case "any-link":
      return isLink(el);
    case "defined":
      return true;
    case "lang":
      for (var n = el; n; n = n.parentElement) {
        var lang = n.getAttribute("lang");
        if (lang !== null) {
          lang = lang.toLowerCase();
          return lang === p.lang || lang.substring(0, p.lang.length + 1) === p.lang + "-";
        }
      }
      return false;
    }
    // states NetSurf does not track
    return false;
  }

  function matchCompound(el, compound, scope) {
    if (compound.never) {
      return false;
    }
    if (compound.tag !== null && localName(el) !== compound.tag) {
      return false;
    }
    if (compound.id !== null && el.id !== compound.id) {
      return false;
    }
    var i;
    for (i = 0; i < compound.classes.length; i++) {
      if (!hasClass(el, compound.classes[i])) {
        return false;
      }
    }
    for (i = 0; i < compound.attrs.length; i++) {
      if (!matchAttr(el, compound.attrs[i])) {
        return false;
      }
    }
    for (i = 0; i < compound.pseudos.length; i++) {
      if (!matchPseudo(el, compound.pseudos[i], scope)) {
        return false;
      }
    }
    return true;
  }

  // Match the parts of a complex selector up to index i against el.  A
  // part without a compound is the subject of a relative selector.
  function matchParts(el, parts, i, scope) {
    var part = parts[i];
    if (part.compound === null) {
      return el === scope;
    }
    if (!matchCompound(el, part.compound, scope)) {
      return false;
    }
    if (i === 0) {
      return true;
    }
    var n;
    switch (parts[i - 1].comb) {
    case ">":
      n = el.parentElement;
      return n !== null && matchParts(n, parts, i - 1, scope);
    case " ":
      for (n = el.parentElement; n; n = n.parentElement) {
        if (matchParts(n, parts, i - 1, scope)) {
          return true;
        }
      }
      return false;
    case "+":
      n = el.previousElementSibling;
      return n !== null && matchParts(n, parts, i - 1, scope);
    case "~":
      for (n = el.previousElementSibling; n; n = n.previousElementSibling) {
        if (matchParts(n, parts, i - 1, scope)) {
          return true;
        }
      }
      return false;
    }
    return false;
  }

  function matchesList(el, list, scope) {
    for (var i = 0; i < list.length; i++) {
      if (matchParts(el, list[i], list[i].length - 1, scope)) {
        return true;
      }
    }
    return false;
  }

  // Call fn for each element below root in document order, stopping if
  // it returns true.
  function walk(root, fn) {
    var el = root.firstElementChild;
    while (el) {
      if (fn(el)) {
        return true;
      }
      if (el.firstElementChild) {
        el = el.firstElementChild;
        continue;
      }
      while (el && el !== root && !el.nextElementSibling) {
        el = el.parentElement;
        if (el === null || el === root) {
          return false;
        }
      }
      if (!el || el === root) {
        return false;
      }
      el = el.nextElementSibling;
    }
    return false;
  }

  // The first element of a document or fragment.  These are not
  // elements so have no firstElementChild of their own in NetSurf.
  function firstElement(node) {
    if (node.nodeType === 9) {
      return node.documentElement;
    }
    var n = node.firstChild;
    while (n && n.nodeType !== 1) {
      n = n.nextSibling;
    }
    return n;
  }

  function walkFrom(root, fn) {
    if (root.nodeType === 1) {
      return walk(root, fn);
    }
    // documents and fragments: visit each top level element and below
    for (var el = firstElement(root); el; el = el.nextElementSibling) {
      if (fn(el) || walk(el, fn)) {
        return true;
      }
    }
    return false;
  }

  // :has() matches if any element relative to the subject matches
  function hasMatch(subject, list) {
    for (var i = 0; i < list.length; i++) {
      var parts = list[i];
      var comb = parts[0].comb;
      var found = false;
      var test = function (el) {
        found = matchParts(el, parts, parts.length - 1, subject);
        return found;
      };
      if (comb === " " || comb === ">") {
        walk(subject, test);
      } else {
        for (var sib = subject.nextElementSibling; sib && !found; sib = sib.nextElementSibling) {
          if (!test(sib)) {
            walk(sib, test);
          }
        }
      }
      if (found) {
        return true;
      }
    }
    return false;
  }

  function staticList(items) {
    items.item = function (index) {
      var el = this[index];
      return el === undefined ? null : el;
    };
    return items;
  }

  function scopeOf(node) {
    return node.nodeType === 1 ? node : null;
  }

  function querySelector(selectors) {
    var list = parse(selectors);
    var scope = scopeOf(this);
    var result = null;
    walkFrom(this, function (el) {
      if (matchesList(el, list, scope)) {
        result = el;
        return true;
      }
      return false;
    });
    return result;
  }

  function querySelectorAll(selectors) {
    var list = parse(selectors);
    var scope = scopeOf(this);
    var result = [];
    walkFrom(this, function (el) {
      if (matchesList(el, list, scope)) {
        result.push(el);
      }
      return false;
    });
    return staticList(result);
  }

  function getElementsByClassName(names) {
    var wanted = String(names).split(/[ \t\r\n\f]+/).filter(function (n) { return n !== ""; });
    var result = [];
    if (wanted.length > 0) {
      walkFrom(this, function (el) {
        for (var i = 0; i < wanted.length; i++) {
          if (!hasClass(el, wanted[i])) {
            return false;
          }
        }
        result.push(el);
        return false;
      });
    }
    return staticList(result);
  }

  function matches(selectors) {
    return matchesList(this, parse(selectors), this);
  }

  function closest(selectors) {
    var list = parse(selectors);
    for (var el = this; el; el = el.parentElement) {
      if (matchesList(el, list, this)) {
        return el;
      }
    }
    return null;
  }

  var targets = [Document.prototype, DocumentFragment.prototype, Element.prototype];
  for (var i = 0; i < targets.length; i++) {
    targets[i].querySelector = querySelector;
    targets[i].querySelectorAll = querySelectorAll;
  }
  Document.prototype.getElementsByClassName = getElementsByClassName;
  Element.prototype.getElementsByClassName = getElementsByClassName;
  Element.prototype.matches = matches;
  Element.prototype.webkitMatchesSelector = matches;
  Element.prototype.msMatchesSelector = matches;
  Element.prototype.closest = closest;
})();
