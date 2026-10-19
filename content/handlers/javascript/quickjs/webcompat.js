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

  /** \todo the viewport size should come from the browser window */
  const viewport = { width: 1024, height: 768 };
  defineAccessors(global, {
    get innerWidth() { return viewport.width; },
    get innerHeight() { return viewport.height; },
    get outerWidth() { return viewport.width; },
    get outerHeight() { return viewport.height; },
    get devicePixelRatio() { return 1; },
    get scrollX() { return 0; },
    get scrollY() { return 0; },
    get pageXOffset() { return 0; },
    get pageYOffset() { return 0; },
    get screenX() { return 0; },
    get screenY() { return 0; }
  });

  defineGlobal("screen", Object.freeze({
    width: viewport.width, height: viewport.height,
    availWidth: viewport.width, availHeight: viewport.height,
    colorDepth: 24, pixelDepth: 24
  }));

  defineMethods(global, {
    scroll() {},
    scrollTo() {},
    scrollBy() {},
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
      /** \todo report the layout position once it is available */
      return new DOMRect(0, 0, 0, 0);
    },
    getClientRects() {
      return [];
    },
    scrollIntoView() {},
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

  /* Layout dependent geometry is not yet available */
  const zero = () => 0;
  defineAccessors(Element.prototype, {
    get clientWidth() { return 0; },
    get clientHeight() { return 0; },
    get clientTop() { return 0; },
    get clientLeft() { return 0; },
    get scrollWidth() { return 0; },
    get scrollHeight() { return 0; },
    get scrollTop() { return 0; },
    set scrollTop(v) {},
    get scrollLeft() { return 0; },
    set scrollLeft(v) {}
  });
  defineAccessors(HTMLElement.prototype, {
    get offsetWidth() { return zero(); },
    get offsetHeight() { return zero(); },
    get offsetTop() { return zero(); },
    get offsetLeft() { return zero(); },
    get offsetParent() { return null; }
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

  /** \todo return the computed style once NetSurf can provide it */
  defineMethods(global, {
    getComputedStyle(element) {
      if (!(element instanceof Element)) {
        throw new TypeError("getComputedStyle: argument is not an Element");
      }
      return new Proxy({
        getPropertyValue: () => "",
        getPropertyPriority: () => "",
        item: () => "",
        length: 0,
        cssText: ""
      }, {
        get(target, prop) {
          return prop in target ? target[prop] : (typeof prop === "string" ? "" : undefined);
        }
      });
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
})();
