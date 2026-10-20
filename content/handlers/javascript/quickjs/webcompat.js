/* Web platform interfaces for the QuickJS binding in NetSurf
 *
 * Copyright 2026 Trevor Cockayne
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * Released under the terms of the MIT License,
 *         http://www.opensource.org/licenses/mit-license
 *
 * Implementations of interfaces which can be built from the DOM bindings
 * and are simpler to write in javascript than in a binding.  Many replace
 * binding members which are not implemented, which otherwise exist but do
 * nothing and so defeat feature detection.
 *
 * This is loaded into every QuickJS context after polyfill.js and must
 * only have side effects.  It may use any syntax QuickJS supports, unlike
 * polyfill.js which duktape also loads.
 */

(() => {
  "use strict";

  const global = globalThis;

  /* Native layout queries the binding provides for this file only */
  const native = {
    /* [x, y, width, height, clientWidth, clientHeight, scrollWidth,
     * scrollHeight] of an element in document coordinates, or null
     */
    geometry: global.__netsurf_geometry,
    /* [width, height, scrollX, scrollY] of the viewport */
    viewport: global.__netsurf_viewport,
    /* computed value of a css property of an element, or undefined */
    style: global.__netsurf_style,
    /* scroll the window to a document position */
    scrollTo: global.__netsurf_scroll_to,
    /* start a network request, returning its id; see qjsky.c */
    fetch: global.__netsurf_fetch,
    /* abort a network request by id */
    fetchAbort: global.__netsurf_fetch_abort,
    /* decode an ArrayBuffer in a character set */
    decode: global.__netsurf_decode,
    /* call the Window's listeners for an event, false if cancelled */
    windowDispatch: global.__netsurf_window_dispatch
  };
  for (const name of ["__netsurf_geometry", "__netsurf_viewport",
                      "__netsurf_style", "__netsurf_scroll_to",
                      "__netsurf_fetch", "__netsurf_fetch_abort",
                      "__netsurf_decode", "__netsurf_window_dispatch"]) {
    delete global[name];
  }

  /* Define methods on a prototype as WebIDL operations are */
  const defineMethods = (target, methods) => {
    for (const name of Object.keys(methods)) {
      Object.defineProperty(target, name, {
        value: methods[name], writable: true, enumerable: true, configurable: true
      });
    }
  };

  /* Define accessors on a prototype as WebIDL attributes are */
  const defineAccessors = (target, accessors) => {
    for (const name of Object.keys(accessors)) {
      const desc = Object.getOwnPropertyDescriptor(accessors, name);
      Object.defineProperty(target, name, {
        get: desc.get, set: desc.set, enumerable: true, configurable: true
      });
    }
  };

  /* Define a global value, such as an interface object */
  const defineGlobal = (name, value) => {
    Object.defineProperty(global, name, {
      value, writable: true, enumerable: false, configurable: true
    });
  };

  /* Static list of nodes with the NodeList item() method */
  const nodeList = (nodes) => {
    nodes.item = function (index) {
      const node = this[index];
      return node === undefined ? null : node;
    };
    return nodes;
  };

  /* DOMException */

  const exceptionCodes = {
    IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4,
    InvalidCharacterError: 5, NoModificationAllowedError: 7,
    NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11,
    SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14,
    InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18,
    NetworkError: 19, AbortError: 20, URLMismatchError: 21,
    QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24,
    DataCloneError: 25
  };

  if (typeof global.DOMException !== "function" ||
      !("code" in global.DOMException.prototype)) {
    class DOMException extends Error {
      constructor(message = "", name = "Error") {
        super(message);
        Object.defineProperty(this, "name", { value: String(name), configurable: true, writable: true });
      }
      get code() {
        return exceptionCodes[this.name] || 0;
      }
    }
    defineGlobal("DOMException", DOMException);
  }
  const domError = (name, message) => new global.DOMException(message, name);

  /* Window */

  /* NetSurf has no frames in javascript so the window is its own parent */
  defineAccessors(global, {
    get self() { return global; },
    get frames() { return global; },
    get top() { return global; },
    get parent() { return global; },
    get length() { return 0; },
    get opener() { return null; },
    get frameElement() { return null; },
    get closed() { return false; }
  });

  /* The viewport, from the browser window */
  const viewport = {
    get width() { return native.viewport()[0]; },
    get height() { return native.viewport()[1]; },
    get scrollX() { return native.viewport()[2]; },
    get scrollY() { return native.viewport()[3]; }
  };
  defineAccessors(global, {
    get innerWidth() { return viewport.width; },
    get innerHeight() { return viewport.height; },
    get outerWidth() { return viewport.width; },
    get outerHeight() { return viewport.height; },
    get devicePixelRatio() { return 1; },
    get scrollX() { return viewport.scrollX; },
    get scrollY() { return viewport.scrollY; },
    get pageXOffset() { return viewport.scrollX; },
    get pageYOffset() { return viewport.scrollY; },
    get screenX() { return 0; },
    get screenY() { return 0; }
  });

  /** \todo the screen size should come from the front end */
  defineGlobal("screen", Object.freeze({
    get width() { return viewport.width; },
    get height() { return viewport.height; },
    get availWidth() { return viewport.width; },
    get availHeight() { return viewport.height; },
    colorDepth: 24, pixelDepth: 24
  }));

  /* Arguments of scroll methods: (x, y) or ({ left, top }) */
  const scrollArgs = (args, current) => {
    if (args.length === 1 && typeof args[0] === "object" && args[0] !== null) {
      const o = args[0];
      return [o.left === undefined ? current[0] : Number(o.left),
              o.top === undefined ? current[1] : Number(o.top)];
    }
    return [Number(args[0]) || 0, Number(args[1]) || 0];
  };

  defineMethods(global, {
    scroll(...args) {
      const [x, y] = scrollArgs(args, [viewport.scrollX, viewport.scrollY]);
      native.scrollTo(x, y);
    },
    scrollTo(...args) {
      const [x, y] = scrollArgs(args, [viewport.scrollX, viewport.scrollY]);
      native.scrollTo(x, y);
    },
    scrollBy(...args) {
      const [x, y] = scrollArgs(args, [0, 0]);
      native.scrollTo(viewport.scrollX + x, viewport.scrollY + y);
    },
    focus() {},
    blur() {}
  });

  /* Base64 */

  const base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  defineMethods(global, {
    btoa(data) {
      data = String(data);
      let out = "";
      for (let i = 0; i < data.length; i += 3) {
        const a = data.charCodeAt(i);
        const b = data.charCodeAt(i + 1);
        const c = data.charCodeAt(i + 2);
        if (a > 0xff || b > 0xff || c > 0xff) {
          throw domError("InvalidCharacterError", "btoa: character out of range");
        }
        const n = (a << 16) | ((b || 0) << 8) | (c || 0);
        out += base64[n >> 18] + base64[(n >> 12) & 63] +
          (i + 1 < data.length ? base64[(n >> 6) & 63] : "=") +
          (i + 2 < data.length ? base64[n & 63] : "=");
      }
      return out;
    },

    atob(data) {
      data = String(data).replace(/[\t\n\f\r ]+/g, "");
      if (data.length % 4 === 0) {
        data = data.replace(/==?$/, "");
      }
      if (data.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(data)) {
        throw domError("InvalidCharacterError", "atob: invalid base64");
      }
      let out = "";
      let bits = 0;
      let value = 0;
      for (const ch of data) {
        value = (value << 6) | base64.indexOf(ch);
        bits += 6;
        if (bits >= 8) {
          bits -= 8;
          out += String.fromCharCode((value >> bits) & 0xff);
        }
      }
      return out;
    }
  });

  /* Timing */

  let nextFrameId = 1;
  const frames = new Map();

  defineMethods(global, {
    requestAnimationFrame(callback) {
      if (typeof callback !== "function") {
        throw new TypeError("requestAnimationFrame: callback is not a function");
      }
      const id = nextFrameId++;
      frames.set(id, setTimeout(() => {
        frames.delete(id);
        callback(performance.now());
      }, 16));
      return id;
    },

    cancelAnimationFrame(id) {
      const timer = frames.get(id);
      if (timer !== undefined) {
        clearTimeout(timer);
        frames.delete(id);
      }
    },

    queueMicrotask(callback) {
      if (typeof callback !== "function") {
        throw new TypeError("queueMicrotask: callback is not a function");
      }
      Promise.resolve().then(callback);
    },

    structuredClone(value) {
      /** \todo handle the full structured clone algorithm */
      return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
    }
  });

  /* Events */

  /* Make an event constructor from the binding interface object so
   * instanceof and the prototype members work.  Events are created
   * through Document.createEvent as the bindings cannot construct them.
   */
  const eventConstructor = (name, parent, defaults) => {
    const native = global[name];
    const proto = native ? native.prototype :
      Object.create(parent ? parent.prototype : Object.prototype);
    const allDefaults = Object.assign({}, parent ? parent.defaults : {}, defaults);

    const ctor = function (type, init) {
      if (new.target === undefined) {
        throw new TypeError(`Constructor ${name} requires 'new'`);
      }
      if (arguments.length < 1) {
        throw new TypeError(`${name} constructor: type is required`);
      }
      init = init || {};
      const evt = document.createEvent("Event");
      evt.initEvent(String(type), !!init.bubbles, !!init.cancelable);
      Object.setPrototypeOf(evt, new.target.prototype);
      for (const key of Object.keys(allDefaults)) {
        Object.defineProperty(evt, key, {
          value: key in init ? init[key] : allDefaults[key],
          enumerable: true, configurable: true
        });
      }
      return evt;
    };
    Object.defineProperty(ctor, "name", { value: name });
    ctor.prototype = proto;
    ctor.defaults = allDefaults;
    if (parent) {
      Object.setPrototypeOf(ctor, parent);
      if (!native) {
        Object.setPrototypeOf(proto, parent.prototype);
      }
    }
    Object.defineProperty(proto, "constructor", {
      value: ctor, writable: true, configurable: true
    });
    for (const key of ["NONE", "CAPTURING_PHASE", "AT_TARGET", "BUBBLING_PHASE"]) {
      if (native && key in native) {
        ctor[key] = native[key];
      }
    }
    defineGlobal(name, ctor);
    return ctor;
  };

  const modifiers = {
    ctrlKey: false, shiftKey: false, altKey: false, metaKey: false
  };

  const EventCtor = eventConstructor("Event", null, { composed: false });
  eventConstructor("CustomEvent", EventCtor, { detail: null });
  const UIEventCtor = eventConstructor("UIEvent", EventCtor, { view: null, detail: 0 });
  eventConstructor("FocusEvent", UIEventCtor, { relatedTarget: null });
  const MouseEventCtor = eventConstructor("MouseEvent", UIEventCtor, Object.assign({
    screenX: 0, screenY: 0, clientX: 0, clientY: 0, pageX: 0, pageY: 0,
    offsetX: 0, offsetY: 0, movementX: 0, movementY: 0,
    button: 0, buttons: 0, relatedTarget: null
  }, modifiers));
  eventConstructor("PointerEvent", MouseEventCtor, {
    pointerId: 0, width: 1, height: 1, pressure: 0, pointerType: "mouse",
    isPrimary: false
  });
  eventConstructor("WheelEvent", MouseEventCtor, {
    deltaX: 0, deltaY: 0, deltaZ: 0, deltaMode: 0
  });
  eventConstructor("KeyboardEvent", UIEventCtor, Object.assign({
    key: "", code: "", location: 0, repeat: false, isComposing: false,
    charCode: 0, keyCode: 0, which: 0
  }, modifiers));
  eventConstructor("InputEvent", UIEventCtor, {
    data: null, inputType: "", isComposing: false
  });
  eventConstructor("ErrorEvent", EventCtor, {
    message: "", filename: "", lineno: 0, colno: 0, error: null
  });
  eventConstructor("MessageEvent", EventCtor, {
    data: null, origin: "", lastEventId: "", source: null, ports: []
  });
  eventConstructor("HashChangeEvent", EventCtor, { oldURL: "", newURL: "" });
  eventConstructor("PopStateEvent", EventCtor, { state: null });
  eventConstructor("PageTransitionEvent", EventCtor, { persisted: false });
  eventConstructor("ProgressEvent", EventCtor, {
    lengthComputable: false, loaded: 0, total: 0
  });

  defineAccessors(EventCtor.prototype, {
    get timeStamp() { return performance.now(); },
    get composed() { return false; }
  });
  defineMethods(EventCtor.prototype, {
    composedPath() {
      const path = [];
      for (let node = this.target; node; node = node.parentNode) {
        path.push(node);
      }
      if (path.length > 0) {
        path.push(global);
      }
      return path;
    }
  });

  /* Nodes */

  const ELEMENT_NODE = 1;
  const TEXT_NODE = 3;
  const DOCUMENT_NODE = 9;
  const DOCUMENT_FRAGMENT_NODE = 11;

  const elementChildren = (node) => {
    const out = [];
    for (let n = node.firstChild; n; n = n.nextSibling) {
      if (n.nodeType === ELEMENT_NODE) {
        out.push(n);
      }
    }
    return out;
  };

  const ownerDocument = (node) =>
    node.nodeType === DOCUMENT_NODE ? node : node.ownerDocument;

  /* Convert the arguments of append() and friends to a single node */
  const nodesToNode = (doc, nodes) => {
    const convert = (n) => n instanceof Node ? n : doc.createTextNode(String(n));
    if (nodes.length === 1) {
      return convert(nodes[0]);
    }
    const fragment = doc.createDocumentFragment();
    for (const n of nodes) {
      fragment.appendChild(convert(n));
    }
    return fragment;
  };

  const parentNodeMembers = {
    append(...nodes) {
      this.appendChild(nodesToNode(ownerDocument(this), nodes));
    },
    prepend(...nodes) {
      this.insertBefore(nodesToNode(ownerDocument(this), nodes), this.firstChild);
    },
    replaceChildren(...nodes) {
      const node = nodesToNode(ownerDocument(this), nodes);
      while (this.firstChild) {
        this.removeChild(this.firstChild);
      }
      this.appendChild(node);
    }
  };

  const parentNodeAccessors = {
    get children() { return nodeList(elementChildren(this)); },
    get firstElementChild() { return elementChildren(this)[0] || null; },
    get lastElementChild() { const c = elementChildren(this); return c[c.length - 1] || null; },
    get childElementCount() { return elementChildren(this).length; }
  };

  const childNodeMembers = {
    before(...nodes) {
      const parent = this.parentNode;
      if (parent) {
        parent.insertBefore(nodesToNode(this.ownerDocument, nodes), this);
      }
    },
    after(...nodes) {
      const parent = this.parentNode;
      if (parent) {
        parent.insertBefore(nodesToNode(this.ownerDocument, nodes), this.nextSibling);
      }
    },
    replaceWith(...nodes) {
      const parent = this.parentNode;
      if (parent) {
        parent.replaceChild(nodesToNode(this.ownerDocument, nodes), this);
      }
    },
    remove() {
      const parent = this.parentNode;
      if (parent) {
        parent.removeChild(this);
      }
    }
  };

  for (const iface of [Document, DocumentFragment, Element]) {
    defineMethods(iface.prototype, parentNodeMembers);
  }
  /* Element has working element child accessors of its own */
  defineAccessors(Element.prototype, {
    get children() { return nodeList(elementChildren(this)); }
  });
  for (const iface of [Document, DocumentFragment]) {
    defineAccessors(iface.prototype, parentNodeAccessors);
  }
  for (const iface of [Element, CharacterData, DocumentType]) {
    defineMethods(iface.prototype, childNodeMembers);
  }

  defineMethods(DocumentFragment.prototype, {
    getElementById(id) {
      return this.querySelector("#" + CSS.escape(String(id)));
    }
  });

  /* Element */

  defineMethods(Element.prototype, {
    hasAttributes() {
      return this.attributes.length > 0;
    },
    getAttributeNames() {
      const names = [];
      for (let i = 0; i < this.attributes.length; i++) {
        names.push(this.attributes.item(i).name);
      }
      return names;
    },
    toggleAttribute(name, force) {
      const present = this.hasAttribute(name);
      if (force === undefined ? present : !force) {
        this.removeAttribute(name);
        return false;
      }
      if (!present) {
        this.setAttribute(name, "");
      }
      return true;
    },
    insertAdjacentElement(where, element) {
      return insertAdjacent(this, where, element);
    },
    insertAdjacentText(where, text) {
      insertAdjacent(this, where, this.ownerDocument.createTextNode(String(text)));
    },
    insertAdjacentHTML(where, html) {
      const container = this.ownerDocument.createElement("div");
      container.innerHTML = String(html);
      const fragment = this.ownerDocument.createDocumentFragment();
      while (container.firstChild) {
        fragment.appendChild(container.firstChild);
      }
      insertAdjacent(this, where, fragment);
    },
    getBoundingClientRect() {
      const g = native.geometry(this);
      if (g === null) {
        return new DOMRect(0, 0, 0, 0);
      }
      return new DOMRect(g[0] - viewport.scrollX, g[1] - viewport.scrollY, g[2], g[3]);
    },
    getClientRects() {
      return native.geometry(this) === null ? [] : [this.getBoundingClientRect()];
    },
    scrollIntoView(arg) {
      const g = native.geometry(this);
      if (g === null) {
        return;
      }
      const block = (typeof arg === "object" && arg !== null) ? arg.block :
        (arg === false ? "end" : "start");
      let y = g[1];
      if (block === "end") {
        y = g[1] + g[3] - viewport.height;
      } else if (block === "center") {
        y = g[1] + (g[3] - viewport.height) / 2;
      } else if (block === "nearest") {
        const top = viewport.scrollY;
        if (g[1] >= top && g[1] + g[3] <= top + viewport.height) {
          return;
        }
      }
      native.scrollTo(viewport.scrollX, y);
    },
    /** \todo scrolling within elements is not supported */
    scroll() {},
    scrollTo() {},
    scrollBy() {}
  });

  const insertAdjacent = (element, where, node) => {
    switch (String(where).toLowerCase()) {
    case "beforebegin":
      if (!element.parentNode) {
        return null;
      }
      element.parentNode.insertBefore(node, element);
      return node;
    case "afterbegin":
      element.insertBefore(node, element.firstChild);
      return node;
    case "beforeend":
      element.appendChild(node);
      return node;
    case "afterend":
      if (!element.parentNode) {
        return null;
      }
      element.parentNode.insertBefore(node, element.nextSibling);
      return node;
    }
    throw domError("SyntaxError", `'${where}' is not a valid position`);
  };

  /* Element geometry from the layout */

  const isRoot = (el) => el.ownerDocument !== null && el === el.ownerDocument.documentElement;
  const isBody = (el) => el.ownerDocument !== null && el === el.ownerDocument.body;
  /* The element whose scrolling is the window's */
  const scrollsWindow = (el) => isRoot(el) || isBody(el);
  const geometryValue = (el, index) => {
    const g = native.geometry(el);
    return g === null ? 0 : g[index];
  };
  const pxValue = (el, property) => parseInt(native.style(el, property), 10) || 0;

  defineAccessors(Element.prototype, {
    get clientWidth() { return isRoot(this) ? viewport.width : geometryValue(this, 4); },
    get clientHeight() { return isRoot(this) ? viewport.height : geometryValue(this, 5); },
    get clientTop() { return pxValue(this, "border-top-width"); },
    get clientLeft() { return pxValue(this, "border-left-width"); },
    get scrollWidth() { return geometryValue(this, 6); },
    get scrollHeight() { return geometryValue(this, 7); },
    get scrollTop() { return scrollsWindow(this) ? viewport.scrollY : 0; },
    set scrollTop(v) {
      if (scrollsWindow(this)) {
        native.scrollTo(viewport.scrollX, Number(v) || 0);
      }
    },
    get scrollLeft() { return scrollsWindow(this) ? viewport.scrollX : 0; },
    set scrollLeft(v) {
      if (scrollsWindow(this)) {
        native.scrollTo(Number(v) || 0, viewport.scrollY);
      }
    }
  });

  /* The nearest positioned ancestor, or the body */
  const offsetParentOf = (el) => {
    if (native.geometry(el) === null || isRoot(el) || isBody(el) ||
        native.style(el, "position") === "fixed") {
      return null;
    }
    for (let p = el.parentElement; p !== null; p = p.parentElement) {
      if (isBody(p) || native.style(p, "position") !== "static") {
        return p;
      }
    }
    return null;
  };

  /* Offset of an element from its offset parent's padding edge */
  const offsetOf = (el, index) => {
    const g = native.geometry(el);
    if (g === null) {
      return 0;
    }
    const parent = offsetParentOf(el);
    if (parent === null || isBody(parent)) {
      return g[index];
    }
    const pg = native.geometry(parent);
    const border = pxValue(parent, index === 0 ? "border-left-width" : "border-top-width");
    return g[index] - pg[index] - border;
  };

  defineAccessors(HTMLElement.prototype, {
    get offsetWidth() { return geometryValue(this, 2); },
    get offsetHeight() { return geometryValue(this, 3); },
    get offsetTop() { return offsetOf(this, 1); },
    get offsetLeft() { return offsetOf(this, 0); },
    get offsetParent() { return offsetParentOf(this); }
  });

  class DOMRectReadOnly {
    constructor(x = 0, y = 0, width = 0, height = 0) {
      Object.defineProperties(this, {
        x: { value: +x, enumerable: true }, y: { value: +y, enumerable: true },
        width: { value: +width, enumerable: true }, height: { value: +height, enumerable: true }
      });
    }
    get top() { return Math.min(this.y, this.y + this.height); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    get left() { return Math.min(this.x, this.x + this.width); }
    get right() { return Math.max(this.x, this.x + this.width); }
    toJSON() {
      const { x, y, width, height, top, right, bottom, left } = this;
      return { x, y, width, height, top, right, bottom, left };
    }
  }
  class DOMRect extends DOMRectReadOnly {}
  defineGlobal("DOMRectReadOnly", DOMRectReadOnly);
  defineGlobal("DOMRect", DOMRect);

  /* HTML serialisation */

  const voidElements = new Set([
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr", "basefont", "bgsound",
    "frame", "keygen"
  ]);
  const rawTextElements = new Set([
    "style", "script", "xmp", "iframe", "noembed", "noframes", "plaintext"
  ]);

  const escapeText = (s) => s.replace(/&/g, "&amp;").replace(/ /g, "&nbsp;")
    .replace(/</g, "&lt;").replace(/>/g, "&gt;");
  const escapeAttr = (s) => s.replace(/&/g, "&amp;").replace(/ /g, "&nbsp;")
    .replace(/"/g, "&quot;");

  /* HTML element names are reported in upper case and serialised in
   * lower case; others, such as those of SVG, keep their case.
   */
  const serialName = (element) => {
    const name = element.tagName;
    return name === name.toUpperCase() ? name.toLowerCase() : name;
  };

  const serializeChildren = (node) => {
    let out = "";
    const raw = node.nodeType === ELEMENT_NODE && rawTextElements.has(serialName(node));
    for (let child = node.firstChild; child; child = child.nextSibling) {
      out += serializeNode(child, raw);
    }
    return out;
  };

  const serializeNode = (node, rawParent) => {
    switch (node.nodeType) {
    case ELEMENT_NODE: {
      const name = serialName(node);
      let out = "<" + name;
      const attrs = node.attributes;
      for (let i = 0; i < attrs.length; i++) {
        const attr = attrs.item(i);
        out += " " + attr.name + "=\"" + escapeAttr(attr.value) + "\"";
      }
      out += ">";
      if (voidElements.has(name)) {
        return out;
      }
      return out + serializeChildren(node) + "</" + name + ">";
    }
    case TEXT_NODE:
    case 4: /* CDATA section */
      return rawParent ? node.nodeValue : escapeText(node.nodeValue);
    case 7: /* processing instruction */
      return "<?" + node.nodeName + " " + node.nodeValue + ">";
    case 8: /* comment */
      return "<!--" + node.nodeValue + "-->";
    case 10: /* document type */
      return "<!DOCTYPE " + node.nodeName + ">";
    case DOCUMENT_NODE:
    case DOCUMENT_FRAGMENT_NODE:
      return serializeChildren(node);
    }
    return "";
  };

  const innerHTML = Object.getOwnPropertyDescriptor(Element.prototype, "innerHTML");
  defineAccessors(Element.prototype, {
    get innerHTML() { return serializeChildren(this); },
    set innerHTML(html) { innerHTML.set.call(this, html); },
    get outerHTML() { return serializeNode(this, false); },
    set outerHTML(html) {
      const parent = this.parentNode;
      if (!parent) {
        return;
      }
      if (parent.nodeType === DOCUMENT_NODE) {
        throw domError("NoModificationAllowedError", "cannot replace the document element");
      }
      const container = this.ownerDocument.createElement("div");
      container.innerHTML = String(html);
      const fragment = this.ownerDocument.createDocumentFragment();
      while (container.firstChild) {
        fragment.appendChild(container.firstChild);
      }
      parent.replaceChild(fragment, this);
    }
  });

  /* CharacterData */

  defineAccessors(CharacterData.prototype, {
    get data() { return this.nodeValue; },
    set data(value) { this.nodeValue = value === null ? "" : String(value); },
    get length() { return this.nodeValue.length; },
    get previousElementSibling() {
      let n = this.previousSibling;
      while (n && n.nodeType !== ELEMENT_NODE) {
        n = n.previousSibling;
      }
      return n;
    },
    get nextElementSibling() {
      let n = this.nextSibling;
      while (n && n.nodeType !== ELEMENT_NODE) {
        n = n.nextSibling;
      }
      return n;
    }
  });

  const checkOffset = (node, offset) => {
    offset = offset >>> 0;
    if (offset > node.nodeValue.length) {
      throw domError("IndexSizeError", "offset is out of range");
    }
    return offset;
  };

  defineMethods(CharacterData.prototype, {
    substringData(offset, count) {
      offset = checkOffset(this, offset);
      return this.nodeValue.substr(offset, count >>> 0);
    },
    appendData(data) {
      this.nodeValue += String(data);
    },
    insertData(offset, data) {
      offset = checkOffset(this, offset);
      const v = this.nodeValue;
      this.nodeValue = v.slice(0, offset) + String(data) + v.slice(offset);
    },
    deleteData(offset, count) {
      offset = checkOffset(this, offset);
      const v = this.nodeValue;
      this.nodeValue = v.slice(0, offset) + v.slice(offset + (count >>> 0));
    },
    replaceData(offset, count, data) {
      offset = checkOffset(this, offset);
      const v = this.nodeValue;
      this.nodeValue = v.slice(0, offset) + String(data) + v.slice(offset + (count >>> 0));
    }
  });

  defineAccessors(Text.prototype, {
    get wholeText() {
      let start = this;
      while (start.previousSibling && start.previousSibling.nodeType === TEXT_NODE) {
        start = start.previousSibling;
      }
      let out = "";
      for (let n = start; n && n.nodeType === TEXT_NODE; n = n.nextSibling) {
        out += n.nodeValue;
      }
      return out;
    }
  });

  defineMethods(Text.prototype, {
    splitText(offset) {
      offset = checkOffset(this, offset);
      const v = this.nodeValue;
      const rest = this.ownerDocument.createTextNode(v.slice(offset));
      this.nodeValue = v.slice(0, offset);
      if (this.parentNode) {
        this.parentNode.insertBefore(rest, this.nextSibling);
      }
      return rest;
    }
  });

  /* Document */

  const titleElement = (doc) => doc.querySelector("title");

  defineAccessors(Document.prototype, {
    get title() {
      const title = titleElement(this);
      if (!title) {
        return "";
      }
      return title.textContent.replace(/[\t\n\f\r ]+/g, " ").trim();
    },
    set title(value) {
      let title = titleElement(this);
      if (!title) {
        const head = this.head;
        if (!head) {
          return;
        }
        title = this.createElement("title");
        head.appendChild(title);
      }
      title.textContent = String(value);
    },
    get defaultView() { return global; },
    get compatMode() { return "CSS1Compat"; },
    get characterSet() { return "UTF-8"; },
    get charset() { return "UTF-8"; },
    get inputEncoding() { return "UTF-8"; },
    get contentType() { return "text/html"; },
    get documentURI() { return location.href; },
    get URL() { return location.href; },
    get doctype() {
      for (let n = this.firstChild; n; n = n.nextSibling) {
        if (n.nodeType === 10) {
          return n;
        }
      }
      return null;
    },
    get activeElement() { return this.body; },
    get currentScript() { return null; },
    get hidden() { return false; },
    get visibilityState() { return "visible"; },
    get scrollingElement() { return this.documentElement; },
    get images() { return this.getElementsByTagName("img"); },
    get scripts() { return this.getElementsByTagName("script"); },
    get forms() { return this.getElementsByTagName("form"); },
    get links() { return this.querySelectorAll("a[href], area[href]"); },
    get anchors() { return this.querySelectorAll("a[name]"); },
    get embeds() { return this.getElementsByTagName("embed"); },
    get plugins() { return this.getElementsByTagName("embed"); }
  });

  defineMethods(Document.prototype, {
    hasFocus() { return true; },
    createComment(data) {
      /* comments are created through the parser */
      const container = this.createElement("div");
      container.innerHTML = "<!--" + String(data).replace(/--/g, "- -") + "-->";
      const comment = container.firstChild;
      container.removeChild(comment);
      return comment;
    },
    getElementsByName(name) {
      return this.querySelectorAll(`[name="${CSS.escape(String(name))}"]`);
    }
  });

  /* HTMLElement */

  const camelToData = (name) => "data-" + name.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
  const dataToCamel = (name) => name.slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase());

  const datasets = new WeakMap();
  const makeDataset = (element) => new Proxy(Object.create(DOMStringMap.prototype), {
    get(target, prop) {
      if (typeof prop !== "string") {
        return target[prop];
      }
      const value = element.getAttribute(camelToData(prop));
      return value === null ? undefined : value;
    },
    set(target, prop, value) {
      if (typeof prop !== "string") {
        return false;
      }
      element.setAttribute(camelToData(prop), String(value));
      return true;
    },
    has(target, prop) {
      return typeof prop === "string" && element.hasAttribute(camelToData(prop));
    },
    deleteProperty(target, prop) {
      if (typeof prop === "string") {
        element.removeAttribute(camelToData(prop));
      }
      return true;
    },
    ownKeys() {
      return element.getAttributeNames()
        .filter((n) => n.startsWith("data-")).map(dataToCamel);
    },
    getOwnPropertyDescriptor(target, prop) {
      if (typeof prop !== "string") {
        return undefined;
      }
      const value = element.getAttribute(camelToData(prop));
      if (value === null) {
        return undefined;
      }
      return { value, writable: true, enumerable: true, configurable: true };
    }
  });

  class DOMStringMap {
    constructor() {
      throw new TypeError("Illegal constructor");
    }
  }
  defineGlobal("DOMStringMap", DOMStringMap);

  const reflectBoolean = (name) => ({
    get() { return this.hasAttribute(name); },
    set(v) { if (v) { this.setAttribute(name, ""); } else { this.removeAttribute(name); } },
    enumerable: true, configurable: true
  });

  const focusable = new Set(["a", "button", "input", "select", "textarea", "iframe"]);

  defineAccessors(HTMLElement.prototype, {
    get dataset() {
      let ds = datasets.get(this);
      if (ds === undefined) {
        ds = makeDataset(this);
        datasets.set(this, ds);
      }
      return ds;
    },
    get tabIndex() {
      const v = parseInt(this.getAttribute("tabindex"), 10);
      if (!isNaN(v)) {
        return v;
      }
      return focusable.has(serialName(this)) ? 0 : -1;
    },
    set tabIndex(v) { this.setAttribute("tabindex", String(v | 0)); },
    get isContentEditable() {
      for (let n = this; n; n = n.parentElement) {
        const v = n.getAttribute("contenteditable");
        if (v !== null) {
          return v.toLowerCase() !== "false";
        }
      }
      return false;
    },
    get contentEditable() {
      const v = this.getAttribute("contenteditable");
      if (v === null) {
        return "inherit";
      }
      return v.toLowerCase() === "false" ? "false" : "true";
    },
    set contentEditable(v) { this.setAttribute("contenteditable", String(v)); },
    get accessKey() { return this.getAttribute("accesskey") || ""; },
    set accessKey(v) { this.setAttribute("accesskey", String(v)); },
    get translate() { return this.getAttribute("translate") !== "no"; },
    set translate(v) { this.setAttribute("translate", v ? "yes" : "no"); },
    get spellcheck() { return this.getAttribute("spellcheck") !== "false"; },
    set spellcheck(v) { this.setAttribute("spellcheck", v ? "true" : "false"); },
    get draggable() { return this.getAttribute("draggable") === "true"; },
    set draggable(v) { this.setAttribute("draggable", v ? "true" : "false"); }
  });
  Object.defineProperty(HTMLElement.prototype, "hidden", reflectBoolean("hidden"));

  defineMethods(HTMLElement.prototype, {
    click() {
      if (this.disabled === true) {
        return;
      }
      this.dispatchEvent(new MouseEvent("click", {
        bubbles: true, cancelable: true, view: global, detail: 1
      }));
    },
    focus() {},
    blur() {}
  });

  /* CSS */

  defineGlobal("CSS", Object.freeze({
    escape(value) {
      const s = String(value);
      let out = "";
      for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        const ch = s.charAt(i);
        if (c === 0) {
          out += "�";
        } else if ((c >= 1 && c <= 0x1f) || c === 0x7f ||
                   (i === 0 && c >= 0x30 && c <= 0x39) ||
                   (i === 1 && c >= 0x30 && c <= 0x39 && s.charCodeAt(0) === 0x2d)) {
          out += "\\" + c.toString(16) + " ";
        } else if (i === 0 && s.length === 1 && c === 0x2d) {
          out += "\\" + ch;
        } else if (c >= 0x80 || c === 0x2d || c === 0x5f ||
                   (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) ||
                   (c >= 0x61 && c <= 0x7a)) {
          out += ch;
        } else {
          out += "\\" + ch;
        }
      }
      return out;
    },
    supports() {
      return false;
    }
  }));

  /* CSS declarations: element.style and getComputedStyle()
   *
   * Inline styles are read from and written to the style attribute, so
   * changes reach the layout through the box tree rebuild.  Computed
   * styles come from the layout.  Only properties NetSurf supports are
   * recognised, so feature detection reflects what it can render.
   */

  const supportedProperties = new Set((
    "align-content align-items align-self azimuth background " +
    "background-attachment background-color background-image " +
    "background-position background-repeat border border-bottom " +
    "border-bottom-color border-bottom-style border-bottom-width " +
    "border-collapse border-color border-left border-left-color " +
    "border-left-style border-left-width border-right border-right-color " +
    "border-right-style border-right-width border-spacing border-style " +
    "border-top border-top-color border-top-style border-top-width " +
    "border-width bottom box-sizing break-after break-before break-inside " +
    "caption-side clear clip color columns column-count column-fill " +
    "column-gap column-rule column-rule-color column-rule-style " +
    "column-rule-width column-span column-width content counter-increment " +
    "counter-reset cue cue-after cue-before cursor direction display " +
    "elevation empty-cells fill-opacity flex flex-basis flex-direction " +
    "flex-flow flex-grow flex-shrink flex-wrap float font font-family " +
    "font-size font-style font-variant font-weight height justify-content " +
    "left letter-spacing line-height list-style list-style-image " +
    "list-style-position list-style-type margin margin-bottom margin-left " +
    "margin-right margin-top max-height max-width min-height min-width " +
    "opacity order orphans outline outline-color outline-style " +
    "outline-width overflow overflow-x overflow-y padding padding-bottom " +
    "padding-left padding-right padding-top page-break-after " +
    "page-break-before page-break-inside pause pause-after pause-before " +
    "pitch-range pitch play-during position quotes richness right " +
    "speak-header speak-numeral speak-punctuation speak speech-rate " +
    "stress stroke-opacity table-layout text-align text-decoration " +
    "text-indent text-transform top unicode-bidi vertical-align " +
    "visibility voice-family volume white-space widows width word-spacing " +
    "writing-mode z-index"
  ).split(" "));

  /* Convert a scripting property name to a css property name, or null if
   * it is not a supported property.
   */
  const cssName = (prop) => {
    let name;
    if (prop === "cssFloat") {
      name = "float";
    } else if (prop.indexOf("-") >= 0) {
      name = prop.toLowerCase();
    } else {
      name = prop.replace(/^(webkit|moz|ms|o)(?=[A-Z])/, "-$1")
        .replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
    }
    return supportedProperties.has(name) ? name : null;
  };

  /* Split declarations, such as the style attribute, into [name, value,
   * important] entries, later ones replacing earlier ones.
   */
  const parseDeclarations = (text) => {
    const parts = [];
    let buf = "";
    let depth = 0;
    let quote = null;
    for (const ch of String(text)) {
      if (quote !== null) {
        if (ch === quote) {
          quote = null;
        }
      } else if (ch === "\"" || ch === "'") {
        quote = ch;
      } else if (ch === "(") {
        depth++;
      } else if (ch === ")") {
        depth = Math.max(0, depth - 1);
      } else if (ch === ";" && depth === 0) {
        parts.push(buf);
        buf = "";
        continue;
      }
      buf += ch;
    }
    parts.push(buf);

    const entries = [];
    for (const part of parts) {
      const colon = part.indexOf(":");
      if (colon < 0) {
        continue;
      }
      let name = part.slice(0, colon).trim();
      if (!name.startsWith("--")) {
        name = name.toLowerCase();
      }
      let value = part.slice(colon + 1).trim();
      let important = false;
      const m = /!\s*important\s*$/i.exec(value);
      if (m) {
        important = true;
        value = value.slice(0, m.index).trim();
      }
      if (name === "" || value === "") {
        continue;
      }
      const at = entries.findIndex(([n]) => n === name);
      if (at >= 0) {
        entries.splice(at, 1);
      }
      entries.push([name, value, important]);
    }
    return entries;
  };

  const serializeDeclarations = (entries) => entries.map(([name, value, important]) =>
    name + ": " + value + (important ? " !important" : "") + ";").join(" ");

  /* The owner of each declaration: { element, computed } */
  const declarationOwners = new WeakMap();

  const ownerOf = (decl) => {
    const owner = declarationOwners.get(decl);
    if (owner === undefined) {
      throw new TypeError("Illegal invocation");
    }
    return owner;
  };

  const inlineEntries = (element) => parseDeclarations(element.getAttribute("style") || "");

  const writeEntries = (element, entries) => {
    if (entries.length === 0) {
      element.removeAttribute("style");
    } else {
      element.setAttribute("style", serializeDeclarations(entries));
    }
  };

  const readOnly = () => domError("NoModificationAllowedError",
    "computed style declarations are read only");

  defineMethods(CSSStyleDeclaration.prototype, {
    getPropertyValue(name) {
      const owner = ownerOf(this);
      name = String(name);
      if (!name.startsWith("--")) {
        name = name.toLowerCase();
      }
      if (owner.computed) {
        const value = native.style(owner.element, name);
        return value === undefined ? "" : value;
      }
      const entry = inlineEntries(owner.element).find(([n]) => n === name);
      return entry ? entry[1] : "";
    },
    getPropertyPriority(name) {
      const owner = ownerOf(this);
      if (owner.computed) {
        return "";
      }
      name = String(name).toLowerCase();
      const entry = inlineEntries(owner.element).find(([n]) => n === name);
      return entry && entry[2] ? "important" : "";
    },
    setProperty(name, value, priority = "") {
      const owner = ownerOf(this);
      if (owner.computed) {
        throw readOnly();
      }
      name = String(name);
      if (!name.startsWith("--")) {
        name = name.toLowerCase();
        if (!supportedProperties.has(name)) {
          return;
        }
      }
      if (value === null || String(value).trim() === "") {
        this.removeProperty(name);
        return;
      }
      const entries = inlineEntries(owner.element).filter(([n]) => n !== name);
      entries.push([name, String(value).trim(), String(priority).toLowerCase() === "important"]);
      writeEntries(owner.element, entries);
    },
    removeProperty(name) {
      const owner = ownerOf(this);
      if (owner.computed) {
        throw readOnly();
      }
      name = String(name);
      if (!name.startsWith("--")) {
        name = name.toLowerCase();
      }
      const entries = inlineEntries(owner.element);
      const entry = entries.find(([n]) => n === name);
      if (entry === undefined) {
        return "";
      }
      writeEntries(owner.element, entries.filter(([n]) => n !== name));
      return entry[1];
    },
    item(index) {
      const owner = ownerOf(this);
      if (owner.computed) {
        return "";
      }
      const entry = inlineEntries(owner.element)[index >>> 0];
      return entry ? entry[0] : "";
    }
  });

  defineAccessors(CSSStyleDeclaration.prototype, {
    get length() {
      const owner = ownerOf(this);
      return owner.computed ? 0 : inlineEntries(owner.element).length;
    },
    get cssText() {
      const owner = ownerOf(this);
      return owner.computed ? "" : serializeDeclarations(inlineEntries(owner.element));
    },
    set cssText(text) {
      const owner = ownerOf(this);
      if (owner.computed) {
        throw readOnly();
      }
      writeEntries(owner.element, parseDeclarations(text));
    },
    get cssFloat() { return this.getPropertyValue("float"); },
    set cssFloat(value) { this.setProperty("float", value); },
    get parentRule() { return null; }
  });

  /* Supported properties are also accessed by their scripting names */
  const declarationHandler = {
    get(target, prop, receiver) {
      if (typeof prop === "string" && !(prop in target)) {
        const name = cssName(prop);
        if (name !== null) {
          return receiver.getPropertyValue(name);
        }
      }
      return Reflect.get(target, prop, receiver);
    },
    set(target, prop, value, receiver) {
      if (typeof prop === "string" && !(prop in target)) {
        const name = cssName(prop);
        if (name !== null) {
          receiver.setProperty(name, value);
          return true;
        }
      }
      return Reflect.set(target, prop, value, receiver);
    },
    has(target, prop) {
      return (typeof prop === "string" && cssName(prop) !== null) || prop in target;
    }
  };

  const makeDeclaration = (element, computed) => {
    const decl = new Proxy(Object.create(CSSStyleDeclaration.prototype), declarationHandler);
    declarationOwners.set(decl, { element, computed });
    return decl;
  };

  const inlineStyles = new WeakMap();
  const styleAccessor = {
    get style() {
      let decl = inlineStyles.get(this);
      if (decl === undefined) {
        decl = makeDeclaration(this, false);
        inlineStyles.set(this, decl);
      }
      return decl;
    },
    set style(text) {
      this.style.cssText = text;
    }
  };
  defineAccessors(Element.prototype, styleAccessor);
  defineAccessors(HTMLElement.prototype, styleAccessor);

  defineMethods(global, {
    getComputedStyle(element) {
      if (!(element instanceof Element)) {
        throw new TypeError("getComputedStyle: argument is not an Element");
      }
      return makeDeclaration(element, true);
    },

    matchMedia(query) {
      query = String(query);
      return {
        media: query,
        matches: evaluateMedia(query),
        onchange: null,
        addListener() {},
        removeListener() {},
        addEventListener() {},
        removeEventListener() {},
        dispatchEvent() { return true; }
      };
    }
  });

  /* Evaluate a media query list against the viewport for the common
   * width, height and preference features.
   */
  const evaluateMedia = (query) => {
    const lengthPx = (value) => {
      const m = /^([\d.]+)(px|em|rem)?$/.exec(value.trim());
      if (!m) {
        return NaN;
      }
      return parseFloat(m[1]) * (m[2] === "em" || m[2] === "rem" ? 16 : 1);
    };
    const feature = (text) => {
      const m = /^\(\s*([a-z-]+)\s*(?::\s*([^)]+))?\)$/.exec(text.trim());
      if (!m) {
        return null;
      }
      const name = m[1];
      const value = m[2] === undefined ? undefined : m[2].trim();
      switch (name) {
      case "min-width": return viewport.width >= lengthPx(value);
      case "max-width": return viewport.width <= lengthPx(value);
      case "width": return viewport.width === lengthPx(value);
      case "min-height": return viewport.height >= lengthPx(value);
      case "max-height": return viewport.height <= lengthPx(value);
      case "height": return viewport.height === lengthPx(value);
      case "orientation": return value === "landscape";
      case "prefers-color-scheme": return value === "light";
      case "prefers-reduced-motion": return value === "no-preference";
      case "prefers-contrast": return value === "no-preference";
      case "hover": return value === undefined || value === "hover";
      case "any-hover": return value === undefined || value === "hover";
      case "pointer": return value === "fine";
      case "any-pointer": return value === "fine";
      case "color": return true;
      }
      return false;
    };
    return query.split(",").some((part) => {
      let text = part.trim().toLowerCase();
      let negate = false;
      if (text.startsWith("not ")) {
        negate = true;
        text = text.slice(4);
      }
      text = text.replace(/^only\s+/, "");
      const terms = text.split(/\s+and\s+/);
      const result = terms.every((term) => {
        if (term === "all" || term === "screen") {
          return true;
        }
        if (term === "print" || term === "speech") {
          return false;
        }
        const f = feature(term);
        return f === null ? false : f;
      });
      return negate ? !result : result;
    });
  };

  /* Storage */

  /** \todo Storage should persist and be shared between documents of
   * an origin.  This keeps it for the lifetime of the document.
   */
  const storageItems = new WeakMap();

  const makeStorage = () => {
    const items = new Map();
    const target = Object.create(Storage.prototype);
    const storage = new Proxy(target, {
      get(target, prop, receiver) {
        if (typeof prop === "string" && !(prop in target)) {
          const v = items.get(prop);
          return v === undefined ? undefined : v;
        }
        return Reflect.get(target, prop, receiver);
      },
      set(target, prop, value) {
        if (typeof prop === "string" && !(prop in target)) {
          items.set(prop, String(value));
          return true;
        }
        return Reflect.set(target, prop, value);
      },
      has(target, prop) {
        return (typeof prop === "string" && items.has(prop)) || prop in target;
      },
      deleteProperty(target, prop) {
        if (typeof prop === "string") {
          items.delete(prop);
        }
        return true;
      },
      ownKeys() {
        return Array.from(items.keys());
      },
      getOwnPropertyDescriptor(target, prop) {
        if (typeof prop === "string" && items.has(prop)) {
          return { value: items.get(prop), writable: true, enumerable: true, configurable: true };
        }
        return undefined;
      }
    });
    storageItems.set(storage, items);
    return storage;
  };

  const itemsOf = (storage) => {
    const items = storageItems.get(storage);
    if (items === undefined) {
      throw new TypeError("Illegal invocation");
    }
    return items;
  };

  defineMethods(Storage.prototype, {
    key(index) {
      const keys = Array.from(itemsOf(this).keys());
      const key = keys[index >>> 0];
      return key === undefined ? null : key;
    },
    getItem(key) {
      const v = itemsOf(this).get(String(key));
      return v === undefined ? null : v;
    },
    setItem(key, value) {
      itemsOf(this).set(String(key), String(value));
    },
    removeItem(key) {
      itemsOf(this).delete(String(key));
    },
    clear() {
      itemsOf(this).clear();
    }
  });
  defineAccessors(Storage.prototype, {
    get length() { return itemsOf(this).size; }
  });

  const localStorage = makeStorage();
  const sessionStorage = makeStorage();
  defineAccessors(global, {
    get localStorage() { return localStorage; },
    get sessionStorage() { return sessionStorage; }
  });

  /* History */

  /** \todo integrate with the browser history */
  let historyState = null;
  const history = Object.create(History.prototype);
  defineAccessors(History.prototype, {
    get length() { return 1; },
    get state() { return historyState; },
    get scrollRestoration() { return "auto"; },
    set scrollRestoration(v) {}
  });
  defineMethods(History.prototype, {
    pushState(state) { historyState = state === undefined ? null : state; },
    replaceState(state) { historyState = state === undefined ? null : state; },
    back() {},
    forward() {},
    go() {}
  });
  defineAccessors(global, {
    get history() { return history; }
  });

  /* Observers */

  /** \todo deliver mutation records */
  class MutationObserver {
    constructor(callback) {
      if (typeof callback !== "function") {
        throw new TypeError("MutationObserver: callback is not a function");
      }
    }
    observe() {}
    disconnect() {}
    takeRecords() { return []; }
  }
  defineGlobal("MutationObserver", MutationObserver);

  /* Without layout information every observed element is reported as
   * fully visible, so content which is loaded when it scrolls into view
   * is loaded straight away.
   */
  class IntersectionObserver {
    #callback;
    #targets = new Set();
    constructor(callback, options = {}) {
      if (typeof callback !== "function") {
        throw new TypeError("IntersectionObserver: callback is not a function");
      }
      this.#callback = callback;
      this.root = options.root || null;
      this.rootMargin = options.rootMargin || "0px";
      const t = options.threshold;
      this.thresholds = Array.isArray(t) ? t : [t || 0];
    }
    observe(target) {
      if (this.#targets.has(target)) {
        return;
      }
      this.#targets.add(target);
      const rect = new DOMRect(0, 0, 0, 0);
      const entry = {
        target, isIntersecting: true, intersectionRatio: 1,
        boundingClientRect: rect, intersectionRect: rect,
        rootBounds: new DOMRect(0, 0, viewport.width, viewport.height),
        time: performance.now()
      };
      setTimeout(() => {
        if (this.#targets.has(target)) {
          this.#callback([entry], this);
        }
      }, 0);
    }
    unobserve(target) { this.#targets.delete(target); }
    disconnect() { this.#targets.clear(); }
    takeRecords() { return []; }
  }
  defineGlobal("IntersectionObserver", IntersectionObserver);

  /** \todo report element sizes once layout is available */
  class ResizeObserver {
    constructor(callback) {
      if (typeof callback !== "function") {
        throw new TypeError("ResizeObserver: callback is not a function");
      }
    }
    observe() {}
    unobserve() {}
    disconnect() {}
  }
  defineGlobal("ResizeObserver", ResizeObserver);

  /* Text encoding */

  class TextEncoder {
    get encoding() { return "utf-8"; }
    encode(input = "") {
      const s = String(input);
      const out = [];
      for (const ch of s) {
        let c = ch.codePointAt(0);
        if (c >= 0xd800 && c <= 0xdfff) {
          c = 0xfffd; /* lone surrogate */
        }
        if (c < 0x80) {
          out.push(c);
        } else if (c < 0x800) {
          out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
        } else if (c < 0x10000) {
          out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        } else {
          out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63),
                   0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
      }
      return new Uint8Array(out);
    }
  }

  class TextDecoder {
    #fatal;
    constructor(label = "utf-8", options = {}) {
      label = String(label).trim().toLowerCase();
      if (label !== "utf-8" && label !== "utf8" && label !== "unicode-1-1-utf-8") {
        throw new RangeError(`TextDecoder: unsupported encoding ${label}`);
      }
      this.#fatal = !!options.fatal;
    }
    get encoding() { return "utf-8"; }
    get fatal() { return this.#fatal; }
    decode(input) {
      if (input === undefined) {
        return "";
      }
      const bytes = input instanceof ArrayBuffer ? new Uint8Array(input) :
        new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
      let out = "";
      let i = 0;
      const bad = () => {
        if (this.#fatal) {
          throw new TypeError("TextDecoder: invalid UTF-8");
        }
        return "�";
      };
      if (bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
        i = 3;
      }
      while (i < bytes.length) {
        const b = bytes[i];
        let need = 0;
        let c;
        if (b < 0x80) {
          out += String.fromCharCode(b);
          i++;
          continue;
        } else if (b >= 0xc2 && b < 0xe0) {
          need = 1; c = b & 0x1f;
        } else if (b >= 0xe0 && b < 0xf0) {
          need = 2; c = b & 0x0f;
        } else if (b >= 0xf0 && b < 0xf5) {
          need = 3; c = b & 0x07;
        } else {
          out += bad();
          i++;
          continue;
        }
        let j = 1;
        for (; j <= need; j++) {
          const n = bytes[i + j];
          if (n === undefined || (n & 0xc0) !== 0x80) {
            break;
          }
          c = (c << 6) | (n & 0x3f);
        }
        if (j <= need || (need === 2 && (c < 0x800 || (c >= 0xd800 && c <= 0xdfff))) ||
            (need === 3 && (c < 0x10000 || c > 0x10ffff))) {
          out += bad();
          i += j;
          continue;
        }
        out += String.fromCodePoint(c);
        i += need + 1;
      }
      return out;
    }
  }
  defineGlobal("TextEncoder", TextEncoder);
  defineGlobal("TextDecoder", TextDecoder);

  /* URL and URLSearchParams following the WHATWG URL standard.
   *
   * \todo internationalised host names are not converted to punycode.
   */

  const specialPorts = {
    "ftp:": 21, "file:": null, "http:": 80, "https:": 443, "ws:": 80, "wss:": 443
  };
  const isSpecial = (scheme) => Object.prototype.hasOwnProperty.call(specialPorts, scheme);

  const hex = (c) => "%" + (c < 16 ? "0" : "") + c.toString(16).toUpperCase();

  /* Percent encode the characters of s for which test is true, and all
   * non-ASCII characters as UTF-8.
   */
  const percentEncode = (s, test) => {
    let out = "";
    for (const ch of s) {
      const c = ch.codePointAt(0);
      if (c > 0x7e) {
        try {
          out += encodeURIComponent(ch);
        } catch (e) {
          out += "%EF%BF%BD"; /* lone surrogate */
        }
      } else if (c < 0x20 || test(ch)) {
        out += hex(c);
      } else {
        out += ch;
      }
    }
    return out;
  };

  const inSet = (chars) => (ch) => chars.indexOf(ch) >= 0;
  const fragmentSet = inSet(" \"<>`");
  const querySet = inSet(" \"#<>");
  const specialQuerySet = inSet(" \"#<>'");
  const pathSet = inSet(" \"#<>?^`{}");
  const userinfoSet = inSet(" \"#<>?`{}/:;=@[\\]^|");

  const percentDecode = (s) => {
    const bytes = [];
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c === 0x25 && /^[0-9a-fA-F]{2}$/.test(s.substr(i + 1, 2))) {
        bytes.push(parseInt(s.substr(i + 1, 2), 16));
        i += 2;
      } else if (c < 0x80) {
        bytes.push(c);
      } else {
        const ch = String.fromCodePoint(s.codePointAt(i));
        bytes.push(...new TextEncoder().encode(ch));
        i += ch.length - 1;
      }
    }
    return new TextDecoder().decode(new Uint8Array(bytes));
  };

  class URLParseError extends TypeError {}

  const fail = (why) => {
    throw new URLParseError(why);
  };

  /* Punycode encoding of a label (RFC 3492) */
  const punycode = (label) => {
    const base = 36, tMin = 1, tMax = 26, skew = 38, damp = 700;
    const points = Array.from(label, (ch) => ch.codePointAt(0));
    let out = points.filter((c) => c < 0x80).map((c) => String.fromCharCode(c)).join("");
    const basic = out.length;
    let handled = basic;
    if (basic > 0) {
      out += "-";
    }
    const digit = (d) => String.fromCharCode(d + 22 + (d < 26 ? 75 : 0));
    const adapt = (delta, count, first) => {
      delta = first ? Math.floor(delta / damp) : delta >> 1;
      delta += Math.floor(delta / count);
      let k = 0;
      while (delta > ((base - tMin) * tMax) >> 1) {
        delta = Math.floor(delta / (base - tMin));
        k += base;
      }
      return k + Math.floor((base - tMin + 1) * delta / (delta + skew));
    };
    let n = 128;
    let delta = 0;
    let bias = 72;
    while (handled < points.length) {
      const m = Math.min(...points.filter((c) => c >= n));
      delta += (m - n) * (handled + 1);
      n = m;
      for (const c of points) {
        if (c < n) {
          delta++;
        } else if (c === n) {
          let q = delta;
          for (let k = base; ; k += base) {
            const t = k <= bias ? tMin : (k >= bias + tMax ? tMax : k - bias);
            if (q < t) {
              break;
            }
            out += digit(t + (q - t) % (base - t));
            q = Math.floor((q - t) / (base - t));
          }
          out += digit(q);
          bias = adapt(delta, handled + 1, handled === basic);
          delta = 0;
          handled++;
        }
      }
      delta++;
      n++;
    }
    return out;
  };

  /* Code points removed by UTS 46 mapping */
  const ignoredInDomain = /[­͏᠋-᠍​-‍⁠︀-️﻿]/g;
  /* Code points which make a domain invalid after mapping */
  const disallowedInDomain = /[\x00-\x1f\x7f-\x9f�﷐-﷯\ud800-\udfff]|[￾￿]|[\ud83f\ud87f\ud8bf\ud8ff\ud93f\ud97f\ud9bf\ud9ff\uda3f\uda7f\udabf\udaff\udb3f\udb7f\udbbf\udbff][\udffe\udfff]/;
  const forbiddenDomain = /[\x00-\x20#%/:<>?@[\\\]^|\x7f]/;
  const forbiddenHost = /[\x00\t\n\r #/:<>?@[\\\]^|]/;

  /* UTS 46 processing of a domain as needed by the URL standard: map
   * case and compatibility forms, remove ignored characters, convert
   * labels to punycode.  The full mapping table is not applied.
   */
  const domainToASCII = (domain) => {
    let s = domain.toLowerCase().normalize("NFKC").toLowerCase();
    s = s.replace(ignoredInDomain, "").replace(/[。．｡]/g, ".");
    if (disallowedInDomain.test(s.replace(/[\ud800-\udbff][\udc00-\udfff]/g, (pair) =>
      (pair.charCodeAt(1) & 0x3fe) === 0x3fe ? pair : "a"))) {
      fail("invalid domain");
    }
    const labels = s.split(".").map((label) => {
      if (/[^\x00-\x7f]/.test(label)) {
        return "xn--" + punycode(label);
      }
      return label;
    });
    s = labels.join(".");
    if (s === "") {
      fail("empty domain");
    }
    return s;
  };

  /* Parse one part of an IPv4 address, returning NaN if invalid */
  const ipv4Number = (part) => {
    if (part === "") {
      return NaN;
    }
    let radix = 10;
    if (/^0x/i.test(part)) {
      part = part.slice(2);
      radix = 16;
      if (part === "") {
        return 0;
      }
    } else if (part.length > 1 && part.startsWith("0")) {
      part = part.slice(1);
      radix = 8;
    }
    const valid = radix === 16 ? /^[0-9a-f]+$/i : (radix === 8 ? /^[0-7]+$/ : /^[0-9]+$/);
    if (!valid.test(part)) {
      return NaN;
    }
    return parseInt(part, radix);
  };

  const endsInNumber = (host) => {
    const parts = host.split(".");
    if (parts[parts.length - 1] === "") {
      if (parts.length === 1) {
        return false;
      }
      parts.pop();
    }
    const last = parts[parts.length - 1];
    return /^[0-9]+$/.test(last) || /^0x[0-9a-f]*$/i.test(last);
  };

  const parseIPv4 = (host) => {
    const parts = host.split(".");
    if (parts[parts.length - 1] === "" && parts.length > 1) {
      parts.pop();
    }
    if (parts.length > 4) {
      fail("invalid IPv4 address");
    }
    const nums = parts.map(ipv4Number);
    if (nums.some((n) => isNaN(n))) {
      fail("invalid IPv4 address");
    }
    for (let i = 0; i < nums.length - 1; i++) {
      if (nums[i] > 255) {
        fail("invalid IPv4 address");
      }
    }
    if (nums[nums.length - 1] >= Math.pow(256, 5 - nums.length)) {
      fail("invalid IPv4 address");
    }
    let value = nums[nums.length - 1];
    for (let i = 0; i < nums.length - 1; i++) {
      value += nums[i] * Math.pow(256, 3 - i);
    }
    return [3, 2, 1, 0].map((i) => Math.floor(value / Math.pow(256, i)) % 256).join(".");
  };

  /* Parse and serialise an IPv6 address, without its brackets */
  const parseIPv6 = (input) => {
    const pieces = [0, 0, 0, 0, 0, 0, 0, 0];
    let index = 0;
    let compress = null;
    let p = 0;
    const at = (i) => input.charAt(i);
    const isHex = (c) => /^[0-9a-f]$/i.test(c);
    if (at(p) === ":") {
      if (at(p + 1) !== ":") {
        fail("invalid IPv6 address");
      }
      p += 2;
      index++;
      compress = index;
    }
    while (p < input.length) {
      if (index === 8) {
        fail("invalid IPv6 address");
      }
      if (at(p) === ":") {
        if (compress !== null) {
          fail("invalid IPv6 address");
        }
        p++;
        index++;
        compress = index;
        continue;
      }
      let value = 0;
      let length = 0;
      while (length < 4 && isHex(at(p))) {
        value = value * 16 + parseInt(at(p), 16);
        p++;
        length++;
      }
      if (at(p) === ".") {
        if (length === 0 || index > 6) {
          fail("invalid IPv6 address");
        }
        p -= length;
        let seen = 0;
        while (p < input.length) {
          let v4 = null;
          if (seen > 0) {
            if (at(p) === "." && seen < 4) {
              p++;
            } else {
              fail("invalid IPv6 address");
            }
          }
          if (!/^[0-9]$/.test(at(p))) {
            fail("invalid IPv6 address");
          }
          while (/^[0-9]$/.test(at(p))) {
            const n = parseInt(at(p), 10);
            if (v4 === null) {
              v4 = n;
            } else if (v4 === 0) {
              fail("invalid IPv6 address");
            } else {
              v4 = v4 * 10 + n;
            }
            if (v4 > 255) {
              fail("invalid IPv6 address");
            }
            p++;
          }
          pieces[index] = pieces[index] * 256 + v4;
          seen++;
          if (seen === 2 || seen === 4) {
            index++;
          }
        }
        if (seen !== 4) {
          fail("invalid IPv6 address");
        }
        break;
      } else if (at(p) === ":") {
        p++;
        if (p >= input.length) {
          fail("invalid IPv6 address");
        }
      } else if (p < input.length) {
        fail("invalid IPv6 address");
      }
      pieces[index] = value;
      index++;
    }
    if (compress !== null) {
      let swaps = index - compress;
      index = 7;
      while (index !== 0 && swaps > 0) {
        const tmp = pieces[compress + swaps - 1];
        pieces[compress + swaps - 1] = pieces[index];
        pieces[index] = tmp;
        index--;
        swaps--;
      }
    } else if (index !== 8) {
      fail("invalid IPv6 address");
    }

    /* serialise, compressing the first longest run of two or more zeros */
    let best = -1;
    let bestLength = 1;
    for (let i = 0; i < 8; i++) {
      let j = i;
      while (j < 8 && pieces[j] === 0) {
        j++;
      }
      if (j - i > bestLength) {
        best = i;
        bestLength = j - i;
      }
    }
    let out = "";
    let ignoring = false;
    for (let i = 0; i < 8; i++) {
      if (ignoring && pieces[i] === 0) {
        continue;
      }
      ignoring = false;
      if (i === best) {
        out += i === 0 ? "::" : ":";
        ignoring = true;
        continue;
      }
      out += pieces[i].toString(16);
      if (i !== 7) {
        out += ":";
      }
    }
    return "[" + out + "]";
  };

  const parseHost = (input, special) => {
    if (input.startsWith("[")) {
      if (!input.endsWith("]")) {
        fail("invalid IPv6 address");
      }
      return parseIPv6(input.slice(1, -1));
    }
    if (!special) {
      if (forbiddenHost.test(input)) {
        fail("invalid host");
      }
      return percentEncode(input, () => false);
    }
    const domain = percentDecode(input);
    const ascii = domainToASCII(domain);
    if (forbiddenDomain.test(ascii)) {
      fail("invalid host");
    }
    if (endsInNumber(ascii)) {
      return parseIPv4(ascii);
    }
    return ascii;
  };

  const isSingleDot = (s) => s === "." || s.toLowerCase() === "%2e";
  const isDoubleDot = (s) => /^(\.|%2e)(\.|%2e)$/i.test(s);
  const isDriveLetter = (s) => /^[a-zA-Z][:|]$/.test(s);
  const isNormalizedDrive = (s) => /^[a-zA-Z]:$/.test(s);
  const startsWithDrive = (s) => /^[a-zA-Z][:|]([/\\?#]|$)/.test(s);

  const shortenPath = (record) => {
    if (record.scheme === "file:" && record.path.length === 1 &&
        isNormalizedDrive(record.path[0])) {
      return;
    }
    record.path.pop();
  };

  /* Append the segments of a relative path to a record's path */
  const parsePath = (record, input) => {
    const special = isSpecial(record.scheme);
    const segments = special ? input.split(/[/\\]/) : input.split("/");
    for (let i = 0; i < segments.length; i++) {
      let seg = segments[i];
      const last = i === segments.length - 1;
      if (isDoubleDot(seg)) {
        shortenPath(record);
        if (last) {
          record.path.push("");
        }
      } else if (isSingleDot(seg)) {
        if (last) {
          record.path.push("");
        }
      } else {
        if (record.scheme === "file:" && record.path.length === 0 && isDriveLetter(seg)) {
          seg = seg.charAt(0) + ":";
        }
        record.path.push(percentEncode(seg, pathSet));
      }
    }
  };

  /* Split off the query and fragment of input into record */
  const parseQueryFragment = (record, input) => {
    let rest = input;
    const hashAt = rest.indexOf("#");
    if (hashAt >= 0) {
      record.fragment = percentEncode(rest.slice(hashAt + 1), fragmentSet);
      rest = rest.slice(0, hashAt);
    }
    const queryAt = rest.indexOf("?");
    if (queryAt >= 0) {
      record.query = percentEncode(rest.slice(queryAt + 1),
        isSpecial(record.scheme) ? specialQuerySet : querySet);
      rest = rest.slice(0, queryAt);
    }
    return rest;
  };

  /* Parse an authority (userinfo, host and port) returning what follows */
  const parseAuthority = (record, input) => {
    const special = isSpecial(record.scheme);
    const end = input.search(special ? /[/\\?#]/ : /[/?#]/);
    let authority = end < 0 ? input : input.slice(0, end);
    const rest = end < 0 ? "" : input.slice(end);
    const at = authority.lastIndexOf("@");
    if (at >= 0) {
      const userinfo = authority.slice(0, at);
      authority = authority.slice(at + 1);
      if (authority === "") {
        fail("credentials without a host");
      }
      const colon = userinfo.indexOf(":");
      record.username = percentEncode(colon < 0 ? userinfo : userinfo.slice(0, colon), userinfoSet);
      record.password = colon < 0 ? "" : percentEncode(userinfo.slice(colon + 1), userinfoSet);
    }
    const m = /^(\[[^\]]*\]|[^:]*)(?::(.*))?$/.exec(authority);
    if (m === null) {
      fail("invalid host");
    }
    const host = m[1];
    if (m[2] !== undefined) {
      if (host === "") {
        fail("port without a host");
      }
      if (m[2] !== "") {
        if (!/^[0-9]+$/.test(m[2]) || parseInt(m[2], 10) > 65535) {
          fail("invalid port");
        }
        record.port = parseInt(m[2], 10);
        if (record.port === specialPorts[record.scheme]) {
          record.port = null;
        }
      }
    }
    if (host === "" && special) {
      fail("missing host");
    }
    record.host = host === "" ? "" : parseHost(host, special);
    return rest;
  };

  /* Parse the path, query and fragment following an authority */
  const parseHierarchical = (record, input) => {
    const special = isSpecial(record.scheme);
    const slash = special ? /^[/\\]/ : /^\//;
    const rest = parseQueryFragment(record, input);
    if (rest !== "") {
      parsePath(record, slash.test(rest) ? rest.slice(1) : rest);
    } else if (special) {
      record.path.push("");
    }
  };

  const newRecord = (scheme) => ({
    scheme, username: "", password: "", host: null, port: null,
    path: [], opaque: null, query: null, fragment: null
  });

  /* The file state of the URL parser, with an optional file base */
  const parseFile = (input, base) => {
    const record = newRecord("file:");
    record.host = "";
    if (/^[/\\]{2}/.test(input)) {
      /* file host */
      const after = input.slice(2);
      const end = after.search(/[/\\?#]/);
      const buffer = end < 0 ? after : after.slice(0, end);
      if (isDriveLetter(buffer)) {
        /* a drive letter is part of the path, not a host */
        parseHierarchical(record, "/" + after);
        return record;
      }
      if (buffer !== "") {
        const host = parseHost(buffer, true);
        record.host = host === "localhost" ? "" : host;
      }
      parseHierarchical(record, end < 0 ? "" : after.slice(end));
      return record;
    }
    if (/^[/\\]/.test(input)) {
      /* file slash */
      if (base !== null) {
        record.host = base.host;
        if (!startsWithDrive(input.slice(1)) && base.path.length > 0 &&
            isNormalizedDrive(base.path[0])) {
          record.path.push(base.path[0]);
        }
      }
      parseHierarchical(record, input);
      return record;
    }
    if (base !== null) {
      record.host = base.host;
      record.path = base.path.slice();
      record.query = base.query;
      if (input === "") {
        return record;
      }
      if (input.startsWith("#")) {
        record.fragment = percentEncode(input.slice(1), fragmentSet);
        return record;
      }
      record.query = null;
      if (input.startsWith("?")) {
        parseQueryFragment(record, input);
        return record;
      }
      if (startsWithDrive(input)) {
        record.path = [];
      } else {
        shortenPath(record);
      }
    }
    const rest = parseQueryFragment(record, input);
    parsePath(record, rest);
    return record;
  };

  /* Parse an opaque path, such as that of a mailto: or data: URL */
  const parseOpaque = (record, input) => {
    let rest = input;
    let trailing = false;
    const hashAt = rest.indexOf("#");
    if (hashAt >= 0) {
      record.fragment = percentEncode(rest.slice(hashAt + 1), fragmentSet);
      rest = rest.slice(0, hashAt);
      trailing = true;
    }
    const queryAt = rest.indexOf("?");
    if (queryAt >= 0) {
      record.query = percentEncode(rest.slice(queryAt + 1), querySet);
      rest = rest.slice(0, queryAt);
      trailing = true;
    }
    let opaque = percentEncode(rest, () => false);
    if (trailing && opaque.endsWith(" ")) {
      opaque = opaque.slice(0, -1) + "%20";
    }
    record.opaque = opaque;
    return record;
  };

  const parseURL = (input, base) => {
    input = String(input).replace(/^[\x00-\x20]+|[\x00-\x20]+$/g, "").replace(/[\t\n\r]/g, "");
    const m = /^([a-zA-Z][a-zA-Z0-9+.-]*):/.exec(input);
    if (m) {
      const scheme = m[1].toLowerCase() + ":";
      let rest = input.slice(m[0].length);
      if (scheme === "file:") {
        return parseFile(rest, base !== null && base.scheme === "file:" ? base : null);
      }
      const record = newRecord(scheme);
      const special = isSpecial(scheme);
      if (special && base !== null && base.scheme === scheme && !/^[/\\]{2}/.test(rest)) {
        /* a relative reference with the scheme of the base */
        return resolve(rest, base);
      }
      if (special) {
        parseHierarchical(record, parseAuthority(record, rest.replace(/^[/\\]*/, "")));
        return record;
      }
      if (rest.startsWith("//")) {
        parseHierarchical(record, parseAuthority(record, rest.slice(2)));
        return record;
      }
      if (rest.startsWith("/")) {
        parseHierarchical(record, rest);
        return record;
      }
      return parseOpaque(record, rest);
    }
    if (base === null) {
      fail("invalid URL");
    }
    return resolve(input, base);
  };

  const copyRecord = (base) => Object.assign(newRecord(base.scheme), base,
    { path: base.path.slice(), query: null, fragment: null });

  /* Resolve a relative reference against a base */
  const resolve = (input, base) => {
    if (base.opaque !== null) {
      if (!input.startsWith("#")) {
        fail("cannot resolve against an opaque URL");
      }
      const record = copyRecord(base);
      record.query = base.query;
      record.fragment = percentEncode(input.slice(1), fragmentSet);
      return record;
    }
    if (base.scheme === "file:") {
      return parseFile(input, base);
    }
    const special = isSpecial(base.scheme);
    const slash = special ? /^[/\\]/ : /^\//;
    if (special && /^[/\\]{2}/.test(input)) {
      const record = newRecord(base.scheme);
      parseHierarchical(record, parseAuthority(record, input.replace(/^[/\\]*/, "")));
      return record;
    }
    if (!special && input.startsWith("//")) {
      const record = newRecord(base.scheme);
      parseHierarchical(record, parseAuthority(record, input.slice(2)));
      return record;
    }
    const record = copyRecord(base);
    if (input === "") {
      record.query = base.query;
      return record;
    }
    if (input.startsWith("#")) {
      record.query = base.query;
      record.fragment = percentEncode(input.slice(1), fragmentSet);
      return record;
    }
    if (input.startsWith("?")) {
      parseQueryFragment(record, input);
      return record;
    }
    if (slash.test(input)) {
      record.path = [];
    } else {
      shortenPath(record);
    }
    const rest = parseQueryFragment(record, input);
    parsePath(record, rest.replace(slash, ""));
    return record;
  };

  const serializePath = (record) => {
    if (record.opaque !== null) {
      return record.opaque;
    }
    return record.path.length > 0 ? "/" + record.path.join("/") : "";
  };

  const serializeHost = (record) => {
    let out = "";
    if (record.username !== "" || record.password !== "") {
      out += record.username + (record.password !== "" ? ":" + record.password : "") + "@";
    }
    out += record.host;
    if (record.port !== null) {
      out += ":" + record.port;
    }
    return out;
  };

  const serializeURL = (record, excludeFragment) => {
    let out = record.scheme;
    if (record.host !== null) {
      out += "//" + serializeHost(record);
    } else if (record.opaque === null && record.path.length > 1 && record.path[0] === "") {
      /* stop the path being read as an authority */
      out += "/.";
    }
    out += serializePath(record);
    if (record.query !== null) {
      out += "?" + record.query;
    }
    if (!excludeFragment && record.fragment !== null) {
      out += "#" + record.fragment;
    }
    return out;
  };

  /* application/x-www-form-urlencoded */
  const formEncode = (s) => percentEncode(String(s).replace(/\r\n|\r|\n/g, "\r\n"),
    (ch) => !/[A-Za-z0-9*\-._ ]/.test(ch)).replace(/ /g, "+");
  const formDecode = (s) => percentDecode(s.replace(/\+/g, " "));
  const parseForm = (s) => s.split("&").filter((p) => p !== "").map((p) => {
    const eq = p.indexOf("=");
    return eq < 0 ? [formDecode(p), ""] : [formDecode(p.slice(0, eq)), formDecode(p.slice(eq + 1))];
  });

  /* Functions which reach the private state of URL and URLSearchParams,
   * set by their static blocks.
   */
  let linkParams;
  let relistParams;
  let setURLQuery;

  class URLSearchParams {
    #list = [];
    #url = null;

    static {
      linkParams = (params, url, query) => {
        params.#url = url;
        params.#list = query === null ? [] : parseForm(query);
      };
      relistParams = (params, query) => {
        params.#list = query === null ? [] : parseForm(query);
      };
    }

    constructor(init = "") {
      if (typeof init === "object" && init !== null) {
        if (typeof init[Symbol.iterator] === "function") {
          for (const pair of init) {
            const items = Array.from(pair);
            if (items.length !== 2) {
              throw new TypeError("URLSearchParams: each pair must have two items");
            }
            this.#list.push([String(items[0]), String(items[1])]);
          }
        } else {
          for (const key of Object.keys(init)) {
            this.#list.push([key, String(init[key])]);
          }
        }
      } else {
        const s = String(init);
        this.#list = parseForm(s.startsWith("?") ? s.slice(1) : s);
      }
    }

    #update() {
      if (this.#url !== null) {
        const s = this.toString();
        setURLQuery(this.#url, s === "" ? null : s);
      }
    }

    get size() { return this.#list.length; }
    append(name, value) {
      this.#list.push([String(name), String(value)]);
      this.#update();
    }
    delete(name, value) {
      name = String(name);
      this.#list = this.#list.filter(([n, v]) => n !== name || (value !== undefined && v !== String(value)));
      this.#update();
    }
    get(name) {
      name = String(name);
      const pair = this.#list.find(([n]) => n === name);
      return pair ? pair[1] : null;
    }
    getAll(name) {
      name = String(name);
      return this.#list.filter(([n]) => n === name).map(([, v]) => v);
    }
    has(name, value) {
      name = String(name);
      return this.#list.some(([n, v]) => n === name && (value === undefined || v === String(value)));
    }
    set(name, value) {
      name = String(name);
      value = String(value);
      const at = this.#list.findIndex(([n]) => n === name);
      if (at < 0) {
        this.#list.push([name, value]);
      } else {
        this.#list[at][1] = value;
        this.#list = this.#list.filter(([n], i) => n !== name || i === at);
      }
      this.#update();
    }
    sort() {
      this.#list = this.#list.map((p, i) => [p, i]).sort(([a, ai], [b, bi]) =>
        a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : ai - bi).map(([p]) => p);
      this.#update();
    }
    forEach(callback, thisArg) {
      for (const [n, v] of this.#list.slice()) {
        callback.call(thisArg, v, n, this);
      }
    }
    *entries() {
      for (const [n, v] of this.#list.slice()) {
        yield [n, v];
      }
    }
    *keys() {
      for (const [n] of this.#list.slice()) {
        yield n;
      }
    }
    *values() {
      for (const [, v] of this.#list.slice()) {
        yield v;
      }
    }
    [Symbol.iterator]() {
      return this.entries();
    }
    toString() {
      return this.#list.map(([n, v]) => formEncode(n) + "=" + formEncode(v)).join("&");
    }
    get [Symbol.toStringTag]() {
      return "URLSearchParams";
    }
  }

  const stripTabs = (value) => String(value).replace(/[\t\n\r]/g, "");

  /* Set the port from the leading digits of s, ignoring the rest */
  const setPort = (r, s) => {
    const digits = /^[0-9]*/.exec(s)[0];
    if (digits === "") {
      return;
    }
    const port = parseInt(digits, 10);
    if (port > 65535) {
      return;
    }
    r.port = port === specialPorts[r.scheme] ? null : port;
  };

  /* The host and hostname setters, which parse with a host state
   * override: an invalid port leaves the new host set.
   */
  const setHost = (r, value, hostnameOnly) => {
    if (r.opaque !== null) {
      return;
    }
    const v = stripTabs(value);
    const special = isSpecial(r.scheme);
    const end = v.search(special ? /[/\\?#]/ : /[/?#]/);
    const buffer = end < 0 ? v : v.slice(0, end);
    if (r.scheme === "file:") {
      if (isDriveLetter(buffer)) {
        return;
      }
      try {
        const host = buffer === "" ? "" : parseHost(buffer, true);
        r.host = host === "localhost" ? "" : host;
      } catch (e) {
        /* invalid hosts are ignored */
      }
      return;
    }
    /* split at the first colon outside an IPv6 address */
    let colon = -1;
    let inBrackets = false;
    for (let i = 0; i < buffer.length; i++) {
      const c = buffer.charAt(i);
      if (c === "[") {
        inBrackets = true;
      } else if (c === "]") {
        inBrackets = false;
      } else if (c === ":" && !inBrackets) {
        colon = i;
        break;
      }
    }
    if (colon >= 0 && hostnameOnly) {
      return;
    }
    const hostPart = colon < 0 ? buffer : buffer.slice(0, colon);
    if (hostPart === "") {
      if (special || colon >= 0 || r.username !== "" || r.password !== "" || r.port !== null) {
        return;
      }
      r.host = "";
      return;
    }
    try {
      r.host = parseHost(hostPart, special);
    } catch (e) {
      return;
    }
    if (colon >= 0) {
      setPort(r, buffer.slice(colon + 1));
    }
  };

  class URL {
    #record;
    #params;

    constructor(url, base) {
      let baseRecord = null;
      if (base !== undefined) {
        baseRecord = (base instanceof URL) ? base.#record : parseURL(String(base), null);
      }
      this.#record = parseURL(String(url), baseRecord);
      this.#params = new URLSearchParams();
      linkParams(this.#params, this, this.#record.query);
    }

    static canParse(url, base) {
      try {
        new URL(url, base);
        return true;
      } catch (e) {
        return false;
      }
    }

    static parse(url, base) {
      try {
        return new URL(url, base);
      } catch (e) {
        return null;
      }
    }

    static {
      setURLQuery = (url, query) => {
        url.#record.query = query;
      };
    }

    get href() { return serializeURL(this.#record, false); }
    set href(value) {
      this.#record = parseURL(String(value), null);
      relistParams(this.#params, this.#record.query);
    }
    toString() { return this.href; }
    toJSON() { return this.href; }

    get origin() {
      const r = this.#record;
      if (r.scheme === "blob:" && r.opaque !== null) {
        try {
          const inner = new URL(r.opaque);
          return (inner.protocol === "http:" || inner.protocol === "https:") ? inner.origin : "null";
        } catch (e) {
          return "null";
        }
      }
      if (!isSpecial(r.scheme) || r.scheme === "file:") {
        return "null";
      }
      return r.scheme + "//" + r.host + (r.port !== null ? ":" + r.port : "");
    }

    get protocol() { return this.#record.scheme; }
    set protocol(value) {
      const m = /^([a-zA-Z][a-zA-Z0-9+.-]*)(?::|$)/.exec(stripTabs(value));
      if (!m) {
        return;
      }
      const scheme = m[1].toLowerCase() + ":";
      const r = this.#record;
      if (isSpecial(scheme) !== isSpecial(r.scheme) ||
          (scheme === "file:" && (r.username !== "" || r.password !== "" || r.port !== null)) ||
          (r.scheme === "file:" && r.host === "")) {
        return;
      }
      r.scheme = scheme;
      if (r.port === specialPorts[scheme]) {
        r.port = null;
      }
    }

    get username() { return this.#record.username; }
    set username(value) {
      const r = this.#record;
      if (r.host === null || r.host === "" || r.scheme === "file:") {
        return;
      }
      r.username = percentEncode(String(value), userinfoSet);
    }

    get password() { return this.#record.password; }
    set password(value) {
      const r = this.#record;
      if (r.host === null || r.host === "" || r.scheme === "file:") {
        return;
      }
      r.password = percentEncode(String(value), userinfoSet);
    }

    get host() {
      const r = this.#record;
      if (r.host === null) {
        return "";
      }
      return r.host + (r.port !== null ? ":" + r.port : "");
    }
    set host(value) {
      setHost(this.#record, value, false);
    }

    get hostname() {
      const host = this.#record.host;
      return host === null ? "" : host;
    }
    set hostname(value) {
      setHost(this.#record, value, true);
    }

    get port() {
      const port = this.#record.port;
      return port === null ? "" : String(port);
    }
    set port(value) {
      const r = this.#record;
      if (r.host === null || r.host === "" || r.scheme === "file:") {
        return;
      }
      if (String(value) === "") {
        r.port = null;
        return;
      }
      setPort(r, stripTabs(value));
    }

    get pathname() { return serializePath(this.#record); }
    set pathname(value) {
      const r = this.#record;
      if (r.opaque !== null) {
        return;
      }
      let s = stripTabs(value);
      const special = isSpecial(r.scheme);
      r.path = [];
      if (s === "" && !special) {
        /* a path without a host must keep its leading slash */
        if (r.host === null) {
          r.path.push("");
        }
        return;
      }
      if (special ? /^[/\\]/.test(s) : s.startsWith("/")) {
        s = s.slice(1);
      }
      parsePath(r, s);
    }

    get search() {
      const query = this.#record.query;
      return query === null || query === "" ? "" : "?" + query;
    }
    set search(value) {
      const r = this.#record;
      let s = stripTabs(value);
      if (s === "") {
        r.query = null;
      } else {
        if (s.startsWith("?")) {
          s = s.slice(1);
        }
        r.query = percentEncode(s, isSpecial(r.scheme) ? specialQuerySet : querySet);
      }
      relistParams(this.#params, r.query);
    }

    get searchParams() { return this.#params; }

    get hash() {
      const fragment = this.#record.fragment;
      return fragment === null || fragment === "" ? "" : "#" + fragment;
    }
    set hash(value) {
      let s = stripTabs(value);
      if (s === "") {
        this.#record.fragment = null;
        return;
      }
      if (s.startsWith("#")) {
        s = s.slice(1);
      }
      this.#record.fragment = percentEncode(s, fragmentSet);
    }

    get [Symbol.toStringTag]() {
      return "URL";
    }
  }

  defineGlobal("URL", URL);
  defineGlobal("URLSearchParams", URLSearchParams);

  /* Element names */

  /* libdom keeps element names in upper case, losing the case of SVG
   * names, so restore those the HTML parser would give
   */
  const SVG_NS = "http://www.w3.org/2000/svg";
  const HTML_NS = "http://www.w3.org/1999/xhtml";
  const svgNames = new Map([
    "altGlyph", "altGlyphDef", "altGlyphItem", "animateColor", "animateMotion",
    "animateTransform", "clipPath", "feBlend", "feColorMatrix",
    "feComponentTransfer", "feComposite", "feConvolveMatrix",
    "feDiffuseLighting", "feDisplacementMap", "feDistantLight", "feDropShadow",
    "feFlood", "feFuncA", "feFuncB", "feFuncG", "feFuncR", "feGaussianBlur",
    "feImage", "feMerge", "feMergeNode", "feMorphology", "feOffset",
    "fePointLight", "feSpecularLighting", "feSpotLight", "feTile",
    "feTurbulence", "foreignObject", "glyphRef", "linearGradient",
    "radialGradient", "textPath"
  ].map((name) => [name.toLowerCase(), name]));
  {
    const elementProto = global.Element.prototype;
    const nativeLocalName = Object.getOwnPropertyDescriptor(elementProto, "localName").get;
    const nativeTagName = Object.getOwnPropertyDescriptor(elementProto, "tagName").get;
    defineAccessors(elementProto, {
      get localName() {
        const name = nativeLocalName.call(this);
        if (this.namespaceURI === SVG_NS) {
          return svgNames.get(name) || name;
        }
        return name;
      },
      get tagName() {
        const ns = this.namespaceURI;
        if (ns === HTML_NS || ns === null) {
          return nativeTagName.call(this);
        }
        const prefix = this.prefix;
        return (prefix ? prefix + ":" : "") + this.localName;
      }
    });
  }

  /* Document base URL */

  /* libdom has no notion of the document URL so find the base here */
  const documentBase = (doc) => {
    const url = doc.URL;
    const base = doc.querySelector ? doc.querySelector("base[href]") : null;
    if (base) {
      try {
        return new URL(base.getAttribute("href"), url).href;
      } catch (e) {
        /* fall back to the document URL */
      }
    }
    return url;
  };
  defineAccessors(global.Node.prototype, {
    get baseURI() {
      const doc = this.nodeType === DOCUMENT_NODE ? this : this.ownerDocument;
      return doc ? documentBase(doc) : "about:blank";
    }
  });

  /* EventTarget */

  /* Objects made by scripts, rather than nodes, keep their listeners
   * here.  The EventTarget methods use these for such objects and the
   * binding for nodes and the window.
   */
  const scriptTargets = new WeakMap();
  const nativeTarget = {
    ctor: global.EventTarget,
    add: global.EventTarget.prototype.addEventListener,
    remove: global.EventTarget.prototype.removeEventListener,
    dispatch: global.EventTarget.prototype.dispatchEvent
  };

  const listenerOptions = (options) => {
    if (typeof options === "boolean") {
      return { capture: options, once: false, signal: null };
    }
    options = options || {};
    return { capture: !!options.capture, once: !!options.once, signal: options.signal || null };
  };

  /* The binding for nodes and the window only takes a capture flag, so
   * options are handled here.  A once listener is added as a wrapper,
   * kept by the callback so it can still be removed by it.
   */
  const nativeOnce = new WeakMap();
  const nativeOnceKey = (type, capture) => String(type) + (capture ? "|capture" : "|bubble");

  const nativeRemove = (target, type, callback, capture) => {
    const byKey = nativeOnce.get(target);
    const wrappers = byKey && byKey.get(nativeOnceKey(type, capture));
    if (wrappers && wrappers.has(callback)) {
      const wrapper = wrappers.get(callback);
      wrappers.delete(callback);
      nativeTarget.remove.call(target, type, wrapper, capture);
      return;
    }
    nativeTarget.remove.call(target, type, callback, capture);
  };

  const nativeAdd = (target, type, callback, options) => {
    if (typeof options !== "object" || options === null) {
      return nativeTarget.add.call(target, type, callback, !!options);
    }
    const opts = listenerOptions(options);
    if (callback === null || callback === undefined) {
      return undefined;
    }
    if (opts.signal && opts.signal.aborted) {
      return undefined;
    }
    if (opts.once) {
      let byKey = nativeOnce.get(target);
      if (byKey === undefined) {
        byKey = new Map();
        nativeOnce.set(target, byKey);
      }
      const key = nativeOnceKey(type, opts.capture);
      let wrappers = byKey.get(key);
      if (wrappers === undefined) {
        wrappers = new Map();
        byKey.set(key, wrappers);
      }
      if (wrappers.has(callback)) {
        return undefined;
      }
      const wrapper = function (event) {
        nativeRemove(target, type, callback, opts.capture);
        if (typeof callback === "function") {
          return callback.call(this, event);
        }
        return callback.handleEvent(event);
      };
      wrappers.set(callback, wrapper);
      nativeTarget.add.call(target, type, wrapper, opts.capture);
    } else {
      nativeTarget.add.call(target, type, callback, opts.capture);
    }
    if (opts.signal) {
      opts.signal.addEventListener("abort", () => {
        nativeRemove(target, type, callback, opts.capture);
      });
    }
    return undefined;
  };

  const EventTarget = function EventTarget() {
    if (new.target === undefined) {
      throw new TypeError("Constructor EventTarget requires 'new'");
    }
    const target = Object.create(new.target.prototype);
    scriptTargets.set(target, new Map());
    return target;
  };
  EventTarget.prototype = nativeTarget.ctor.prototype;
  Object.defineProperty(EventTarget.prototype, "constructor", {
    value: EventTarget, writable: true, configurable: true
  });
  defineGlobal("EventTarget", EventTarget);

  /* Set a read only member of an event, which the binding would set
   * during a real dispatch
   */
  const setEventMember = (event, name, value) => {
    Object.defineProperty(event, name, { value, configurable: true });
  };

  defineMethods(EventTarget.prototype, {
    addEventListener(type, callback, options) {
      const map = scriptTargets.get(this);
      if (map === undefined) {
        return nativeAdd(this, type, callback, options);
      }
      if (callback === null || callback === undefined) {
        return undefined;
      }
      const opts = listenerOptions(options);
      if (opts.signal && opts.signal.aborted) {
        return undefined;
      }
      type = String(type);
      let list = map.get(type);
      if (list === undefined) {
        list = [];
        map.set(type, list);
      }
      if (list.some((l) => l.callback === callback && l.capture === opts.capture)) {
        return undefined;
      }
      const listener = { callback, capture: opts.capture, once: opts.once, removed: false };
      list.push(listener);
      if (opts.signal) {
        opts.signal.addEventListener("abort", () => {
          this.removeEventListener(type, callback, { capture: opts.capture });
        });
      }
      return undefined;
    },

    removeEventListener(type, callback, options) {
      const map = scriptTargets.get(this);
      if (map === undefined) {
        return nativeRemove(this, type, callback, listenerOptions(options).capture);
      }
      const capture = listenerOptions(options).capture;
      const list = map.get(String(type));
      if (list !== undefined) {
        const i = list.findIndex((l) => l.callback === callback && l.capture === capture);
        if (i >= 0) {
          list[i].removed = true;
          list.splice(i, 1);
        }
      }
      return undefined;
    },

    dispatchEvent(event) {
      const map = scriptTargets.get(this);
      if (map === undefined) {
        if (this === global) {
          if (!(event instanceof global.Event)) {
            throw new TypeError("dispatchEvent: argument is not an Event");
          }
          setEventMember(event, "target", global);
          setEventMember(event, "currentTarget", global);
          setEventMember(event, "eventPhase", 2);
          setEventMember(event, "isTrusted", false);
          const result = native.windowDispatch(event);
          setEventMember(event, "currentTarget", null);
          setEventMember(event, "eventPhase", 0);
          return result;
        }
        return nativeTarget.dispatch.apply(this, arguments);
      }
      if (!(event instanceof global.Event)) {
        throw new TypeError("dispatchEvent: argument is not an Event");
      }
      setEventMember(event, "target", this);
      setEventMember(event, "currentTarget", this);
      setEventMember(event, "eventPhase", 2);
      setEventMember(event, "isTrusted", false);
      let stopped = false;
      const stop = event.stopImmediatePropagation;
      setEventMember(event, "stopImmediatePropagation", function () {
        stopped = true;
        return stop.call(this);
      });
      for (const listener of (map.get(event.type) || []).slice()) {
        if (listener.removed) {
          continue;
        }
        if (listener.once) {
          this.removeEventListener(event.type, listener.callback, { capture: listener.capture });
        }
        try {
          if (typeof listener.callback === "function") {
            listener.callback.call(this, event);
          } else if (typeof listener.callback.handleEvent === "function") {
            listener.callback.handleEvent(event);
          }
        } catch (e) {
          reportError(e);
        }
        if (stopped) {
          break;
        }
      }
      setEventMember(event, "currentTarget", null);
      setEventMember(event, "eventPhase", 0);
      return !event.defaultPrevented;
    }
  });

  /* Report an exception from a callback without stopping the caller */
  const reportError = (e) => {
    console.error(e instanceof Error ? `${e.name}: ${e.message}\n${e.stack || ""}` : String(e));
  };
  if (typeof global.reportError !== "function") {
    defineMethods(global, { reportError });
  }

  /* Define onevent handler attributes, which are listeners added the
   * first time they are set
   */
  const defineEventHandlers = (proto, types) => {
    const handlers = new WeakMap();
    for (const type of types) {
      Object.defineProperty(proto, "on" + type, {
        get() {
          const h = handlers.get(this);
          return h && h[type] ? h[type].value : null;
        },
        set(value) {
          let h = handlers.get(this);
          if (h === undefined) {
            h = {};
            handlers.set(this, h);
          }
          if (h[type] === undefined) {
            h[type] = { value: null };
            const slot = h[type];
            this.addEventListener(type, function (event) {
              if (typeof slot.value === "function") {
                const ret = slot.value.call(this, event);
                if (ret === false) {
                  event.preventDefault();
                }
              }
            });
          }
          h[type].value = typeof value === "function" ? value : null;
        },
        enumerable: true,
        configurable: true
      });
    }
  };

  /* Make an event to dispatch from a script target */
  const makeEvent = (type, init) => new global.Event(type, init);
  const makeProgressEvent = (type, loaded, total) =>
    new global.ProgressEvent(type, { lengthComputable: total > 0, loaded, total });

  /* AbortController and AbortSignal */

  const abortState = new WeakMap();

  class AbortSignal extends EventTarget {
    constructor() {
      if (arguments[0] !== abortState) {
        throw new TypeError("Illegal constructor");
      }
      super();
      abortState.set(this, { aborted: false, reason: undefined });
    }

    static abort(reason) {
      const signal = new AbortSignal(abortState);
      signalAbort(signal, reason);
      return signal;
    }

    static timeout(ms) {
      const signal = new AbortSignal(abortState);
      setTimeout(() => signalAbort(signal, domError("TimeoutError", "signal timed out")), Number(ms));
      return signal;
    }

    static any(signals) {
      const signal = new AbortSignal(abortState);
      for (const s of signals) {
        if (s.aborted) {
          signalAbort(signal, s.reason);
          return signal;
        }
      }
      for (const s of signals) {
        s.addEventListener("abort", () => signalAbort(signal, s.reason));
      }
      return signal;
    }

    get aborted() { return abortState.get(this).aborted; }
    get reason() { return abortState.get(this).reason; }

    throwIfAborted() {
      if (this.aborted) {
        throw this.reason;
      }
    }

    get [Symbol.toStringTag]() { return "AbortSignal"; }
  }
  defineEventHandlers(AbortSignal.prototype, ["abort"]);

  const signalAbort = (signal, reason) => {
    const state = abortState.get(signal);
    if (state.aborted) {
      return;
    }
    state.aborted = true;
    state.reason = reason === undefined ? domError("AbortError", "signal is aborted without reason") : reason;
    signal.dispatchEvent(makeEvent("abort"));
  };

  class AbortController {
    #signal = new AbortSignal(abortState);
    get signal() { return this.#signal; }
    abort(reason) { signalAbort(this.#signal, reason); }
    get [Symbol.toStringTag]() { return "AbortController"; }
  }

  defineGlobal("AbortSignal", AbortSignal);
  defineGlobal("AbortController", AbortController);

  /* Bytes and text */

  const textEncoder = new TextEncoder();
  const encodeUTF8 = (s) => textEncoder.encode(s);
  const decodeText = (bytes, charset) => {
    const buffer = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength ?
      bytes.buffer : bytes.slice().buffer;
    return native.decode(buffer, charset);
  };

  /* Bytes of an ArrayBuffer or view */
  const bufferBytes = (value) => {
    if (value instanceof ArrayBuffer) {
      return new Uint8Array(value.slice(0));
    }
    if (ArrayBuffer.isView(value)) {
      return new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
    }
    return null;
  };

  /* The charset parameter of a MIME type */
  const mimeCharset = (type) => {
    const m = /;\s*charset\s*=\s*"?([^";\s]+)/i.exec(type || "");
    return m ? m[1] : "utf-8";
  };

  /* The essence of a MIME type, without parameters */
  const mimeEssence = (type) => String(type || "").split(";")[0].trim().toLowerCase();

  /* Blob and File */

  const blobBytes = new WeakMap();

  class Blob {
    #type;
    constructor(parts = [], options = {}) {
      const chunks = [];
      for (const part of parts) {
        if (part instanceof Blob) {
          chunks.push(blobBytes.get(part));
        } else if (part instanceof ArrayBuffer || ArrayBuffer.isView(part)) {
          chunks.push(bufferBytes(part));
        } else {
          chunks.push(encodeUTF8(String(part)));
        }
      }
      let length = 0;
      for (const c of chunks) {
        length += c.length;
      }
      const bytes = new Uint8Array(length);
      let at = 0;
      for (const c of chunks) {
        bytes.set(c, at);
        at += c.length;
      }
      blobBytes.set(this, bytes);
      const type = options.type === undefined ? "" : String(options.type);
      this.#type = /^[\x20-\x7e]*$/.test(type) ? type.toLowerCase() : "";
    }
    get size() { return blobBytes.get(this).length; }
    get type() { return this.#type; }
    slice(start = 0, end = this.size, type = "") {
      const blob = new Blob([], { type });
      blobBytes.set(blob, blobBytes.get(this).slice(start, end));
      return blob;
    }
    arrayBuffer() { return Promise.resolve(blobBytes.get(this).slice().buffer); }
    bytes() { return Promise.resolve(blobBytes.get(this).slice()); }
    text() { return Promise.resolve(decodeText(blobBytes.get(this), "utf-8")); }
    get [Symbol.toStringTag]() { return "Blob"; }
  }

  class File extends Blob {
    #name;
    #lastModified;
    constructor(parts, name, options = {}) {
      super(parts, options);
      this.#name = String(name);
      this.#lastModified = options.lastModified === undefined ? Date.now() : Number(options.lastModified);
    }
    get name() { return this.#name; }
    get lastModified() { return this.#lastModified; }
    get [Symbol.toStringTag]() { return "File"; }
  }

  defineGlobal("Blob", Blob);
  defineGlobal("File", File);

  /* FormData */

  class FormData {
    #entries = [];

    constructor(form, submitter) {
      if (form === undefined) {
        return;
      }
      if (!(form instanceof global.HTMLFormElement)) {
        throw new TypeError("FormData: argument is not a form");
      }
      for (const el of form.querySelectorAll("input, select, textarea, button")) {
        const name = el.getAttribute("name");
        if (!name || el.disabled || el.closest("fieldset[disabled]")) {
          continue;
        }
        const tag = el.localName;
        const type = (el.getAttribute("type") || "").toLowerCase();
        if (tag === "button" || (tag === "input" && ["submit", "button", "reset", "image"].includes(type))) {
          if (el === submitter) {
            this.append(name, el.value);
          }
          continue;
        }
        if (tag === "input" && (type === "checkbox" || type === "radio")) {
          if (el.checked) {
            this.append(name, el.hasAttribute("value") ? el.value : "on");
          }
        } else if (tag === "input" && type === "file") {
          /* file inputs are not readable by scripts */
          this.append(name, new File([], "", { type: "application/octet-stream" }));
        } else if (tag === "select") {
          for (const option of el.querySelectorAll("option")) {
            if (option.selected && !option.disabled) {
              this.append(name, option.value);
            }
          }
        } else {
          this.append(name, el.value);
        }
      }
    }

    #entry(name, value, filename) {
      name = String(name);
      if (value instanceof Blob) {
        if (!(value instanceof File) || filename !== undefined) {
          value = new File([value], filename === undefined ? "blob" : filename, { type: value.type });
        }
      } else {
        value = String(value);
      }
      return [name, value];
    }

    append(name, value, filename) { this.#entries.push(this.#entry(name, value, filename)); }
    delete(name) { name = String(name); this.#entries = this.#entries.filter((e) => e[0] !== name); }
    get(name) { name = String(name); const e = this.#entries.find((x) => x[0] === name); return e ? e[1] : null; }
    getAll(name) { name = String(name); return this.#entries.filter((e) => e[0] === name).map((e) => e[1]); }
    has(name) { name = String(name); return this.#entries.some((e) => e[0] === name); }
    set(name, value, filename) {
      const entry = this.#entry(name, value, filename);
      const i = this.#entries.findIndex((e) => e[0] === entry[0]);
      if (i < 0) {
        this.#entries.push(entry);
      } else {
        this.#entries[i] = entry;
        this.#entries = this.#entries.filter((e, j) => j <= i || e[0] !== entry[0]);
      }
    }
    forEach(callback, thisArg) {
      for (const [name, value] of this.#entries.slice()) {
        callback.call(thisArg, value, name, this);
      }
    }
    *entries() { yield* this.#entries.map((e) => [e[0], e[1]]); }
    *keys() { for (const e of this.#entries) { yield e[0]; } }
    *values() { for (const e of this.#entries) { yield e[1]; } }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return "FormData"; }
  }
  defineGlobal("FormData", FormData);

  /* Encode form data as multipart/form-data */
  const encodeMultipart = (formData) => {
    const boundary = "----NetSurfFormBoundary" +
      Math.random().toString(36).slice(2) + Math.random().toString(36).slice(2);
    const escape = (s) => s.replace(/\r\n?|\n/g, "\r\n").replace(/"/g, "%22").replace(/\r/g, "%0D").replace(/\n/g, "%0A");
    const parts = [];
    for (const [name, value] of formData) {
      let head = `--${boundary}\r\nContent-Disposition: form-data; name="${escape(name)}"`;
      if (value instanceof File) {
        head += `; filename="${escape(value.name)}"\r\nContent-Type: ${value.type || "application/octet-stream"}\r\n\r\n`;
        parts.push(head, value, "\r\n");
      } else {
        parts.push(head + "\r\n\r\n" + value.replace(/\r\n?|\n/g, "\r\n") + "\r\n");
      }
    }
    parts.push(`--${boundary}--\r\n`);
    return { bytes: blobBytes.get(new Blob(parts)), type: `multipart/form-data; boundary=${boundary}` };
  };

  /* Headers */

  const headerNameRe = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
  const forbiddenRequestHeaders = new Set([
    "accept-charset", "accept-encoding", "access-control-request-headers",
    "access-control-request-method", "connection", "content-length",
    "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
    "origin", "referer", "set-cookie", "te", "trailer",
    "transfer-encoding", "upgrade", "via"
  ]);
  const isForbiddenRequestHeader = (name) =>
    forbiddenRequestHeaders.has(name) || name.startsWith("proxy-") || name.startsWith("sec-");

  /* Strip leading and trailing HTTP whitespace */
  const normaliseHeaderValue = (value) => String(value).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, "");

  const headersGuard = new WeakMap();

  class Headers {
    #list = [];

    constructor(init) {
      if (init === undefined || init === null) {
        return;
      }
      if (typeof init !== "object") {
        throw new TypeError("Headers: init is not an object");
      }
      if (typeof init[Symbol.iterator] === "function") {
        for (const pair of init) {
          const p = Array.from(pair);
          if (p.length !== 2) {
            throw new TypeError("Headers: init pair must have two members");
          }
          this.append(p[0], p[1]);
        }
      } else {
        for (const key of Reflect.ownKeys(init)) {
          if (typeof key === "string" && Object.getOwnPropertyDescriptor(init, key).enumerable) {
            this.append(key, init[key]);
          }
        }
      }
    }

    #check(name, value) {
      name = String(name);
      if (!headerNameRe.test(name)) {
        throw new TypeError(`Headers: invalid header name "${name}"`);
      }
      if (value !== undefined && /[\0\r\n]/.test(value)) {
        throw new TypeError(`Headers: invalid value for "${name}"`);
      }
      if (headersGuard.get(this) === "immutable") {
        throw new TypeError("Headers: headers are immutable");
      }
      return name.toLowerCase();
    }

    #ignored(lower) {
      return headersGuard.get(this) === "request" && isForbiddenRequestHeader(lower);
    }

    append(name, value) {
      value = normaliseHeaderValue(value);
      const lower = this.#check(name, value);
      if (!this.#ignored(lower)) {
        this.#list.push([lower, value]);
      }
    }

    delete(name) {
      const lower = this.#check(name);
      if (!this.#ignored(lower)) {
        this.#list = this.#list.filter((h) => h[0] !== lower);
      }
    }

    get(name) {
      const lower = String(name).toLowerCase();
      if (!headerNameRe.test(lower)) {
        throw new TypeError(`Headers: invalid header name "${name}"`);
      }
      const values = this.#list.filter((h) => h[0] === lower).map((h) => h[1]);
      return values.length === 0 ? null : values.join(", ");
    }

    getSetCookie() {
      return this.#list.filter((h) => h[0] === "set-cookie").map((h) => h[1]);
    }

    has(name) {
      const lower = String(name).toLowerCase();
      if (!headerNameRe.test(lower)) {
        throw new TypeError(`Headers: invalid header name "${name}"`);
      }
      return this.#list.some((h) => h[0] === lower);
    }

    set(name, value) {
      value = normaliseHeaderValue(value);
      const lower = this.#check(name, value);
      if (this.#ignored(lower)) {
        return;
      }
      const i = this.#list.findIndex((h) => h[0] === lower);
      if (i < 0) {
        this.#list.push([lower, value]);
      } else {
        this.#list[i] = [lower, value];
        this.#list = this.#list.filter((h, j) => j <= i || h[0] !== lower);
      }
    }

    /* sorted and combined, as iteration requires */
    #sorted() {
      const names = [...new Set(this.#list.map((h) => h[0]))].sort();
      const out = [];
      for (const name of names) {
        if (name === "set-cookie") {
          for (const v of this.getSetCookie()) {
            out.push([name, v]);
          }
        } else {
          out.push([name, this.get(name)]);
        }
      }
      return out;
    }

    forEach(callback, thisArg) {
      for (const [name, value] of this.#sorted()) {
        callback.call(thisArg, value, name, this);
      }
    }
    *entries() { yield* this.#sorted(); }
    *keys() { for (const h of this.#sorted()) { yield h[0]; } }
    *values() { for (const h of this.#sorted()) { yield h[1]; } }
    [Symbol.iterator]() { return this.entries(); }
    get [Symbol.toStringTag]() { return "Headers"; }
  }
  defineGlobal("Headers", Headers);

  const makeHeaders = (init, guard) => {
    const headers = new Headers(guard === "request" ? undefined : init);
    if (guard === "request") {
      headersGuard.set(headers, "request");
      if (init !== undefined && init !== null) {
        for (const [name, value] of new Headers(init)) {
          headers.append(name, value);
        }
      }
    } else if (guard) {
      headersGuard.set(headers, guard);
    }
    return headers;
  };

  /* Bodies of requests and responses */

  /* Extract a body and its content type from a body init value */
  const extractBody = (init) => {
    if (typeof init === "string") {
      return { text: init, type: "text/plain;charset=UTF-8" };
    }
    if (init instanceof URLSearchParams) {
      return { text: init.toString(), type: "application/x-www-form-urlencoded;charset=UTF-8" };
    }
    if (init instanceof Blob) {
      return { bytes: blobBytes.get(init).slice(), type: init.type || null };
    }
    if (init instanceof FormData) {
      return encodeMultipart(init);
    }
    const bytes = bufferBytes(init);
    if (bytes) {
      return { bytes, type: null };
    }
    return { text: String(init), type: "text/plain;charset=UTF-8" };
  };

  const bodyState = new WeakMap();

  /* The bytes of a body, which may be held as text */
  const bodyBytes = (state) => {
    if (state.bytes === undefined) {
      state.bytes = encodeUTF8(state.text);
    }
    return state.bytes;
  };

  /* Consume a body, as the Body mixin methods do */
  const consumeBody = (obj, convert) => {
    const state = bodyState.get(obj);
    if (state.used) {
      return Promise.reject(new TypeError("Body has already been used"));
    }
    if (state.body !== null) {
      state.used = true;
    }
    try {
      return Promise.resolve(convert(state.body));
    } catch (e) {
      return Promise.reject(e);
    }
  };

  const bodyText = (body) => {
    if (body === null) {
      return "";
    }
    return body.text !== undefined ? body.text : decodeText(body.bytes, "utf-8");
  };

  const bodyMixin = {
    get body() {
      /** \todo ReadableStream */
      return null;
    },
    get bodyUsed() { return bodyState.get(this).used; },
    arrayBuffer() {
      return consumeBody(this, (b) => b === null ? new ArrayBuffer(0) : bodyBytes(b).slice().buffer);
    },
    bytes() {
      return consumeBody(this, (b) => b === null ? new Uint8Array(0) : bodyBytes(b).slice());
    },
    blob() {
      const type = this.headers.get("content-type") || "";
      return consumeBody(this, (b) => new Blob(b === null ? [] : [bodyBytes(b)], { type }));
    },
    text() { return consumeBody(this, bodyText); },
    json() { return consumeBody(this, (b) => JSON.parse(bodyText(b))); },
    formData() {
      const type = mimeEssence(this.headers.get("content-type"));
      return consumeBody(this, (b) => {
        if (type !== "application/x-www-form-urlencoded") {
          /** \todo parse multipart/form-data */
          throw new TypeError("formData: unsupported content type");
        }
        const fd = new FormData();
        for (const [name, value] of new URLSearchParams(bodyText(b))) {
          fd.append(name, value);
        }
        return fd;
      });
    }
  };

  const defineBody = (proto) => {
    for (const name of Object.keys(bodyMixin)) {
      const desc = Object.getOwnPropertyDescriptor(bodyMixin, name);
      if (desc.get) {
        Object.defineProperty(proto, name, { get: desc.get, enumerable: true, configurable: true });
      } else {
        Object.defineProperty(proto, name, {
          value: desc.value, writable: true, enumerable: true, configurable: true
        });
      }
    }
  };

  const copyBody = (body) => body === null ? null :
    { text: body.text, bytes: body.bytes === undefined ? undefined : body.bytes.slice() };

  /* Request */

  const normalMethods = ["DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT", "PATCH"];
  const requestState = new WeakMap();

  class Request {
    constructor(input, init = {}) {
      let base;
      if (input instanceof Request) {
        base = requestState.get(input);
        if (bodyState.get(input).used) {
          throw new TypeError("Request: body has already been used");
        }
      } else {
        let url;
        try {
          url = new URL(String(input), document.baseURI);
        } catch (e) {
          throw new TypeError(`Request: invalid URL "${input}"`);
        }
        if (url.username || url.password) {
          throw new TypeError("Request: URL includes credentials");
        }
        base = {
          url: url.href, method: "GET", headers: undefined, mode: "cors",
          credentials: "same-origin", redirect: "follow", cache: "default",
          referrer: "about:client", referrerPolicy: "", integrity: "",
          keepalive: false, signal: null
        };
      }
      init = init || {};
      const state = Object.assign({}, base);
      if (init.method !== undefined) {
        let method = String(init.method);
        if (!headerNameRe.test(method)) {
          throw new TypeError(`Request: invalid method "${method}"`);
        }
        if (["CONNECT", "TRACE", "TRACK"].includes(method.toUpperCase())) {
          throw new TypeError(`Request: forbidden method "${method}"`);
        }
        if (normalMethods.includes(method.toUpperCase())) {
          method = method.toUpperCase();
        }
        state.method = method;
      }
      for (const key of ["mode", "credentials", "redirect", "cache", "referrerPolicy", "integrity"]) {
        if (init[key] !== undefined) {
          state[key] = String(init[key]);
        }
      }
      if (state.mode === "navigate") {
        throw new TypeError("Request: mode navigate is not allowed");
      }
      if (init.referrer !== undefined) {
        state.referrer = String(init.referrer);
      }
      if (init.keepalive !== undefined) {
        state.keepalive = !!init.keepalive;
      }
      state.signal = new AbortSignal(abortState);
      const parentSignal = init.signal !== undefined ? init.signal :
        (input instanceof Request ? input.signal : null);
      if (parentSignal) {
        if (parentSignal.aborted) {
          signalAbort(state.signal, parentSignal.reason);
        } else {
          parentSignal.addEventListener("abort", () => signalAbort(state.signal, parentSignal.reason));
        }
      }
      requestState.set(this, state);

      const headersInit = init.headers !== undefined ? init.headers :
        (input instanceof Request ? input.headers : undefined);
      state.headers = makeHeaders(headersInit, "request");

      let body = null;
      if (init.body !== undefined && init.body !== null) {
        if (state.method === "GET" || state.method === "HEAD") {
          throw new TypeError("Request: GET and HEAD requests cannot have a body");
        }
        const extracted = extractBody(init.body);
        body = { text: extracted.text, bytes: extracted.bytes };
        if (extracted.type && !state.headers.has("content-type")) {
          state.headers.append("content-type", extracted.type);
        }
      } else if (input instanceof Request) {
        body = bodyState.get(input).body;
        if (body !== null) {
          bodyState.get(input).used = true;
        }
      }
      bodyState.set(this, { body, used: false });
    }

    get method() { return requestState.get(this).method; }
    get url() { return requestState.get(this).url; }
    get headers() { return requestState.get(this).headers; }
    get destination() { return ""; }
    get referrer() {
      const r = requestState.get(this).referrer;
      return r === "about:client" ? "about:client" : r;
    }
    get referrerPolicy() { return requestState.get(this).referrerPolicy; }
    get mode() { return requestState.get(this).mode; }
    get credentials() { return requestState.get(this).credentials; }
    get cache() { return requestState.get(this).cache; }
    get redirect() { return requestState.get(this).redirect; }
    get integrity() { return requestState.get(this).integrity; }
    get keepalive() { return requestState.get(this).keepalive; }
    get signal() { return requestState.get(this).signal; }

    clone() {
      if (bodyState.get(this).used) {
        throw new TypeError("Request.clone: body has already been used");
      }
      const copy = new Request(this.url, {
        method: this.method, headers: this.headers, mode: this.mode,
        credentials: this.credentials, redirect: this.redirect,
        cache: this.cache, referrerPolicy: this.referrerPolicy,
        integrity: this.integrity, signal: this.signal
      });
      bodyState.get(copy).body = copyBody(bodyState.get(this).body);
      return copy;
    }

    get [Symbol.toStringTag]() { return "Request"; }
  }
  defineBody(Request.prototype);
  defineGlobal("Request", Request);

  /* Response */

  const responseState = new WeakMap();
  const nullBodyStatus = [101, 103, 204, 205, 304];
  const redirectStatus = [301, 302, 303, 307, 308];

  class Response {
    constructor(body = null, init = {}) {
      init = init || {};
      const status = init.status === undefined ? 200 : Number(init.status);
      if (!Number.isInteger(status) || status < 200 || status > 599) {
        throw new RangeError(`Response: invalid status ${init.status}`);
      }
      const statusText = init.statusText === undefined ? "" : String(init.statusText);
      if (/[^\t\x20-\x7e\x80-\xff]/.test(statusText)) {
        throw new TypeError("Response: invalid status text");
      }
      const headers = makeHeaders(init.headers, "response");
      let state = null;
      if (body !== null && body !== undefined) {
        if (nullBodyStatus.includes(status)) {
          throw new TypeError(`Response: status ${status} cannot have a body`);
        }
        const extracted = extractBody(body);
        state = { text: extracted.text, bytes: extracted.bytes };
        if (extracted.type && !headers.has("content-type")) {
          headers.append("content-type", extracted.type);
        }
      }
      responseState.set(this, {
        type: "default", url: "", redirected: false, status, statusText, headers
      });
      bodyState.set(this, { body: state, used: false });
    }

    static error() {
      const r = new Response(null, { status: 200 });
      Object.assign(responseState.get(r), { type: "error", status: 0 });
      headersGuard.set(r.headers, "immutable");
      return r;
    }

    static redirect(url, status = 302) {
      if (!redirectStatus.includes(status)) {
        throw new RangeError(`Response.redirect: invalid status ${status}`);
      }
      const r = new Response(null, { status });
      r.headers.set("location", new URL(String(url), document.baseURI).href);
      headersGuard.set(r.headers, "immutable");
      return r;
    }

    static json(data, init = {}) {
      const text = JSON.stringify(data);
      if (text === undefined) {
        throw new TypeError("Response.json: data is not JSON serialisable");
      }
      const headers = new Headers(init && init.headers);
      if (!headers.has("content-type")) {
        headers.set("content-type", "application/json");
      }
      return new Response(text, Object.assign({}, init, { headers }));
    }

    get type() { return responseState.get(this).type; }
    get url() { return responseState.get(this).url; }
    get redirected() { return responseState.get(this).redirected; }
    get status() { return responseState.get(this).status; }
    get ok() { const s = this.status; return s >= 200 && s <= 299; }
    get statusText() { return responseState.get(this).statusText; }
    get headers() { return responseState.get(this).headers; }

    clone() {
      if (bodyState.get(this).used) {
        throw new TypeError("Response.clone: body has already been used");
      }
      const state = responseState.get(this);
      const copy = new Response(null, { status: 200 });
      const headers = new Headers(state.headers);
      if (headersGuard.get(state.headers)) {
        headersGuard.set(headers, headersGuard.get(state.headers));
      }
      responseState.set(copy, Object.assign({}, state, { headers }));
      bodyState.set(copy, { body: copyBody(bodyState.get(this).body), used: false });
      return copy;
    }

    get [Symbol.toStringTag]() { return "Response"; }
  }
  defineBody(Response.prototype);
  defineGlobal("Response", Response);

  /* Parse the "name: value\r\n" headers the browser passes */
  const parseHeaderLines = (text) => {
    const list = [];
    for (const line of text.split("\r\n")) {
      const i = line.indexOf(":");
      if (i > 0) {
        list.push([line.slice(0, i), normaliseHeaderValue(line.slice(i + 1))]);
      }
    }
    return list;
  };

  /* The origin of the document, for response types */
  const documentOrigin = () => {
    try {
      return new URL(document.URL).origin;
    } catch (e) {
      return "null";
    }
  };

  /* Flatten request headers into alternating names and values */
  const flatHeaders = (headers) => {
    const flat = [];
    for (const [name, value] of headers) {
      flat.push(name, value);
    }
    return flat;
  };

  /* The body to pass to the browser: a string or an ArrayBuffer */
  const nativeBody = (body) => {
    if (body === null) {
      return undefined;
    }
    if (body.text !== undefined) {
      return body.text;
    }
    return body.bytes.slice().buffer;
  };

  /* fetch */

  defineMethods(global, {
    fetch(input, init) {
      return new Promise((resolve, reject) => {
        const request = new Request(input, init);
        const state = requestState.get(request);
        const signal = request.signal;
        if (signal.aborted) {
          reject(signal.reason);
          return;
        }
        let url;
        try {
          url = new URL(request.url);
        } catch (e) {
          reject(new TypeError("fetch: invalid URL"));
          return;
        }
        if (state.mode === "same-origin" && url.origin !== documentOrigin()) {
          reject(new TypeError("fetch: cross origin request in same-origin mode"));
          return;
        }
        const body = bodyState.get(request);
        if (body.body !== null) {
          body.used = true;
        }
        let done = false;
        let id;
        const onAbort = () => {
          if (!done) {
            done = true;
            native.fetchAbort(id);
            reject(signal.reason);
          }
        };
        try {
          id = native.fetch(request.method, request.url, flatHeaders(request.headers),
            nativeBody(body.body), state.credentials,
            state.redirect === "error" ? "error" : "follow",
            (result) => {
              signal.removeEventListener("abort", onAbort);
              if (done) {
                return;
              }
              done = true;
              if (result.error !== undefined) {
                reject(new TypeError(`Failed to fetch: ${result.error}`));
                return;
              }
              const response = new Response(null, { status: 200 });
              let origin;
              try {
                origin = new URL(result.url).origin;
              } catch (e) {
                origin = "null";
              }
              const headers = new Headers(parseHeaderLines(result.headers));
              headersGuard.set(headers, "immutable");
              Object.assign(responseState.get(response), {
                type: origin === documentOrigin() || result.url.startsWith("data:") ? "basic" : "cors",
                url: result.url,
                redirected: result.redirected,
                status: result.status,
                statusText: result.statusText,
                headers
              });
              const empty = request.method === "HEAD" || nullBodyStatus.includes(result.status);
              bodyState.get(response).body = empty ? null : { bytes: new Uint8Array(result.body) };
              resolve(response);
            });
        } catch (e) {
          reject(e instanceof TypeError ? e : new TypeError(String(e)));
          return;
        }
        signal.addEventListener("abort", onAbort);
      });
    }
  });

  /* XMLHttpRequest */

  const xhrEvents = ["loadstart", "progress", "abort", "error", "load", "timeout", "loadend"];

  class XMLHttpRequestEventTarget extends EventTarget {
    constructor() {
      if (arguments[0] !== abortState) {
        throw new TypeError("Illegal constructor");
      }
      super();
    }
    get [Symbol.toStringTag]() { return "XMLHttpRequestEventTarget"; }
  }
  defineEventHandlers(XMLHttpRequestEventTarget.prototype, xhrEvents);

  class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {
    get [Symbol.toStringTag]() { return "XMLHttpRequestUpload"; }
  }

  const UNSENT = 0;
  const OPENED = 1;
  const HEADERS_RECEIVED = 2;
  const LOADING = 3;
  const DONE = 4;

  class XMLHttpRequest extends XMLHttpRequestEventTarget {
    #state = UNSENT;
    #method = "GET";
    #url = null;
    #headers = null;
    #send = false;
    #id = undefined;
    #timer = undefined;
    #timeout = 0;
    #withCredentials = false;
    #responseType = "";
    #overrideMime = null;
    #upload = new XMLHttpRequestUpload(abortState);
    #response = null;
    #responseBytes = null;
    #responseObject = undefined;
    #errored = false;
    #uploading = false;

    constructor() {
      super(abortState);
    }

    get readyState() { return this.#state; }
    get upload() { return this.#upload; }
    get timeout() { return this.#timeout; }
    set timeout(ms) { this.#timeout = Math.max(0, Number(ms) || 0); }
    get withCredentials() { return this.#withCredentials; }
    set withCredentials(value) {
      if (this.#state !== UNSENT && this.#state !== OPENED || this.#send) {
        throw domError("InvalidStateError", "withCredentials cannot be set now");
      }
      this.#withCredentials = !!value;
    }
    get responseType() { return this.#responseType; }
    set responseType(value) {
      value = String(value);
      if (!["", "arraybuffer", "blob", "document", "json", "text"].includes(value)) {
        return;
      }
      if (this.#state === LOADING || this.#state === DONE) {
        throw domError("InvalidStateError", "responseType cannot be set now");
      }
      this.#responseType = value;
    }

    open(method, url, async = true, username = null, password = null) {
      method = String(method);
      if (!headerNameRe.test(method)) {
        throw domError("SyntaxError", `invalid method "${method}"`);
      }
      if (["CONNECT", "TRACE", "TRACK"].includes(method.toUpperCase())) {
        throw domError("SecurityError", `forbidden method "${method}"`);
      }
      if (normalMethods.includes(method.toUpperCase())) {
        method = method.toUpperCase();
      }
      let parsed;
      try {
        parsed = new URL(String(url), document.baseURI);
      } catch (e) {
        throw domError("SyntaxError", `invalid URL "${url}"`);
      }
      if (arguments.length > 2 && !async) {
        /* the browser cannot block scripts while a request is made */
        throw domError("InvalidAccessError", "synchronous XMLHttpRequest is not supported");
      }
      if (username !== null && username !== undefined) {
        parsed.username = String(username);
      }
      if (password !== null && password !== undefined) {
        parsed.password = String(password);
      }
      this.#cancel();
      this.#method = method;
      this.#url = parsed.href;
      this.#headers = makeHeaders(undefined, "request");
      this.#send = false;
      this.#response = null;
      this.#responseBytes = null;
      this.#responseObject = undefined;
      this.#errored = false;
      if (this.#state !== OPENED) {
        this.#state = OPENED;
        this.dispatchEvent(makeEvent("readystatechange"));
      }
    }

    setRequestHeader(name, value) {
      if (this.#state !== OPENED || this.#send) {
        throw domError("InvalidStateError", "setRequestHeader called before open or after send");
      }
      name = String(name);
      value = normaliseHeaderValue(value);
      if (!headerNameRe.test(name) || /[\0\r\n]/.test(value)) {
        throw domError("SyntaxError", `invalid header "${name}"`);
      }
      const lower = name.toLowerCase();
      if (isForbiddenRequestHeader(lower)) {
        return;
      }
      const old = this.#headers.get(lower);
      this.#headers.set(lower, old === null ? value : `${old}, ${value}`);
    }

    send(body = null) {
      if (this.#state !== OPENED || this.#send) {
        throw domError("InvalidStateError", "send called before open or twice");
      }
      let nbody;
      if (this.#method === "GET" || this.#method === "HEAD") {
        body = null;
      }
      if (body !== null && body !== undefined) {
        let extracted;
        if (body instanceof global.Document) {
          const root = body.documentElement;
          extracted = { text: root ? root.outerHTML : "",
                        type: "text/html;charset=UTF-8" };
        } else {
          extracted = extractBody(body);
        }
        nbody = nativeBody({ text: extracted.text, bytes: extracted.bytes });
        if (extracted.type && !this.#headers.has("content-type")) {
          this.#headers.set("content-type", extracted.type);
        }
      }
      this.#send = true;
      this.#errored = false;
      this.#uploading = nbody !== undefined;
      this.dispatchEvent(makeProgressEvent("loadstart", 0, 0));
      if (this.#state !== OPENED || !this.#send) {
        /* a listener called open or abort */
        return;
      }
      if (this.#uploading) {
        this.#upload.dispatchEvent(makeProgressEvent("loadstart", 0, 0));
      }
      try {
        this.#id = native.fetch(this.#method, this.#url, flatHeaders(this.#headers), nbody,
          this.#withCredentials ? "include" : "same-origin", "follow",
          (result) => this.#complete(result));
      } catch (e) {
        Promise.resolve().then(() => this.#complete({ error: String(e) }));
      }
      if (this.#timeout > 0) {
        this.#timer = setTimeout(() => {
          this.#timer = undefined;
          this.#fail("timeout");
        }, this.#timeout);
      }
    }

    abort() {
      const active = this.#send;
      this.#cancel();
      if ((this.#state === OPENED && active) || this.#state === HEADERS_RECEIVED ||
          this.#state === LOADING) {
        this.#failEvents("abort");
      }
      if (this.#state === DONE) {
        this.#state = UNSENT;
        this.#response = null;
      }
    }

    /* Stop the request in progress */
    #cancel() {
      if (this.#id !== undefined) {
        native.fetchAbort(this.#id);
        this.#id = undefined;
      }
      if (this.#timer !== undefined) {
        clearTimeout(this.#timer);
        this.#timer = undefined;
      }
      this.#send = false;
    }

    /* Fail the request with an error, abort or timeout */
    #fail(type) {
      this.#cancel();
      this.#failEvents(type);
    }

    #failEvents(type) {
      this.#state = DONE;
      this.#send = false;
      this.#errored = true;
      this.#response = null;
      this.#responseBytes = null;
      this.dispatchEvent(makeEvent("readystatechange"));
      if (this.#uploading) {
        this.#uploading = false;
        this.#upload.dispatchEvent(makeProgressEvent(type, 0, 0));
        this.#upload.dispatchEvent(makeProgressEvent("loadend", 0, 0));
      }
      this.dispatchEvent(makeProgressEvent(type, 0, 0));
      this.dispatchEvent(makeProgressEvent("loadend", 0, 0));
    }

    /* The response has arrived */
    #complete(result) {
      this.#id = undefined;
      if (this.#timer !== undefined) {
        clearTimeout(this.#timer);
        this.#timer = undefined;
      }
      if (!this.#send) {
        return;
      }
      if (result.error !== undefined) {
        this.#send = false;
        this.#failEvents("error");
        return;
      }
      const headers = new Headers(parseHeaderLines(result.headers));
      this.#response = {
        status: result.status, statusText: result.statusText,
        url: result.url, headers, rawHeaders: result.headers
      };
      this.#responseBytes = new Uint8Array(result.body);
      const total = this.#responseBytes.length;

      if (this.#uploading) {
        this.#uploading = false;
        this.#upload.dispatchEvent(makeProgressEvent("progress", 0, 0));
        this.#upload.dispatchEvent(makeProgressEvent("load", 0, 0));
        this.#upload.dispatchEvent(makeProgressEvent("loadend", 0, 0));
      }
      this.#state = HEADERS_RECEIVED;
      this.dispatchEvent(makeEvent("readystatechange"));
      if (this.#state !== HEADERS_RECEIVED || !this.#send) {
        return;
      }
      this.#state = LOADING;
      this.dispatchEvent(makeEvent("readystatechange"));
      if (this.#state !== LOADING || !this.#send) {
        return;
      }
      this.dispatchEvent(makeProgressEvent("progress", total, total));
      if (this.#state !== LOADING || !this.#send) {
        return;
      }
      this.#state = DONE;
      this.#send = false;
      this.dispatchEvent(makeEvent("readystatechange"));
      this.dispatchEvent(makeProgressEvent("load", total, total));
      this.dispatchEvent(makeProgressEvent("loadend", total, total));
    }

    get status() { return this.#response && !this.#errored ? this.#response.status : 0; }
    get statusText() { return this.#response && !this.#errored ? this.#response.statusText : ""; }
    get responseURL() { return this.#response ? this.#response.url.replace(/#.*$/, "") : ""; }

    getResponseHeader(name) {
      if (this.#response === null || this.#state < HEADERS_RECEIVED) {
        return null;
      }
      return this.#response.headers.get(String(name));
    }

    getAllResponseHeaders() {
      if (this.#response === null || this.#state < HEADERS_RECEIVED) {
        return "";
      }
      let out = "";
      for (const [name, value] of this.#response.headers) {
        out += `${name}: ${value}\r\n`;
      }
      return out;
    }

    overrideMimeType(mime) {
      if (this.#state === LOADING || this.#state === DONE) {
        throw domError("InvalidStateError", "overrideMimeType called too late");
      }
      this.#overrideMime = String(mime);
    }

    /* The MIME type of the response */
    #mime() {
      if (this.#overrideMime !== null) {
        return this.#overrideMime;
      }
      const type = this.#response ? this.#response.headers.get("content-type") : null;
      return type || "text/xml";
    }

    #text() {
      if (this.#responseBytes === null) {
        return "";
      }
      if (this.#responseObject === undefined || typeof this.#responseObject !== "string") {
        this.#responseObject = decodeText(this.#responseBytes, mimeCharset(this.#mime()));
      }
      return this.#responseObject;
    }

    get responseText() {
      if (this.#responseType !== "" && this.#responseType !== "text") {
        throw domError("InvalidStateError", "responseText needs responseType text");
      }
      if (this.#state !== LOADING && this.#state !== DONE) {
        return "";
      }
      return this.#text();
    }

    get responseXML() {
      if (this.#responseType !== "" && this.#responseType !== "document") {
        throw domError("InvalidStateError", "responseXML needs responseType document");
      }
      /** \todo parse XML and HTML responses */
      return null;
    }

    get response() {
      const type = this.#responseType;
      if (type === "" || type === "text") {
        return this.responseText;
      }
      if (this.#state !== DONE || this.#errored) {
        return null;
      }
      if (this.#responseObject !== undefined) {
        return this.#responseObject;
      }
      const bytes = this.#responseBytes;
      if (type === "arraybuffer") {
        this.#responseObject = bytes.slice().buffer;
      } else if (type === "blob") {
        this.#responseObject = new Blob([bytes], { type: this.#mime() });
      } else if (type === "json") {
        try {
          this.#responseObject = JSON.parse(decodeText(bytes, "utf-8"));
        } catch (e) {
          this.#responseObject = null;
        }
      } else {
        this.#responseObject = null;
      }
      return this.#responseObject;
    }

    get [Symbol.toStringTag]() { return "XMLHttpRequest"; }
  }
  defineEventHandlers(XMLHttpRequest.prototype, ["readystatechange"]);
  for (const [name, value] of Object.entries({ UNSENT, OPENED, HEADERS_RECEIVED, LOADING, DONE })) {
    Object.defineProperty(XMLHttpRequest, name, { value, enumerable: true });
    Object.defineProperty(XMLHttpRequest.prototype, name, { value, enumerable: true });
  }

  defineGlobal("XMLHttpRequestEventTarget", XMLHttpRequestEventTarget);
  defineGlobal("XMLHttpRequestUpload", XMLHttpRequestUpload);
  defineGlobal("XMLHttpRequest", XMLHttpRequest);
})();
