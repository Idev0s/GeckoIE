/* GeckoIE frame script: reports right clicks in the page so Internet Explorer can show its menu. */
"use strict";
var lastNode = null;
addEventListener("contextmenu", function (e) {
  if (e.defaultPrevented) { sendAsyncMessage("geckoie:ctx", { prevented: true }); return; }   // the page has its own menu
  var t = e.originalTarget || e.target, n = t, link = "", img = "", edit = false, sel = false, st = "", lt = "";
  lastNode = t;
  try {
    var ln = t.localName;
    if (ln === "img" || ln === "image") img = t.currentSrc || t.src || "";
    for (; n && n.nodeType === 1; n = n.parentNode) {
      var nn = n.localName;
      if ((nn === "a" || nn === "area") && n.href) { link = n.href; try { lt = (n.textContent || n.title || "").replace(/\s+/g, " ").replace(/^ | $/g, "").substring(0, 120); } catch (x) {} break; }
    }
    if (ln === "textarea" || (ln === "input" && /^(text|search|url|email|tel|password|number)?$/i.test(t.type || ""))) {
      edit = true;
      try { if (t.selectionStart !== t.selectionEnd) st = t.value.substring(t.selectionStart, t.selectionEnd); } catch (x) {}
    } else if (t.isContentEditable) edit = true;
    if (!st) { try { st = content.getSelection().toString(); } catch (x) {} }
    st = (st || "").replace(/\s+/g, " ").replace(/^ | $/g, "").substring(0, 200);
    sel = !!st;
  } catch (x) {}
  sendAsyncMessage("geckoie:ctx", { link: link, img: img, sel: sel, edit: edit, st: st, lt: lt });
}, false);
addMessageListener("geckoie:cmd", function (m) {
  try {
    if (m.data && m.data.cmd === "copyimage" && lastNode) {
      var ed = docShell.contentViewer.QueryInterface(Ci.nsIContentViewerEdit);
      ed.setCommandNode(lastNode);
      ed.copyImage(ed.COPY_IMAGE_ALL);
    }
  } catch (x) {}
});

// ---- <select> drop-downs: Gecko asks the host to show them (mozshowdropdown); the host shows a native menu.
var selTarget = null;
addEventListener("mozshowdropdown", function (e) {
  if (!e.isTrusted) return;
  var sel = e.target;
  try {
    if (!sel || sel.localName !== "select") return;
    selTarget = sel;
    var out = [];
    var walk = function (parent, dis) {
      for (var n = parent.firstElementChild; n; n = n.nextElementSibling) {
        if (n.localName === "option") out.push({ t: "o", label: String(n.label || n.text || "").replace(/\s+/g, " ").substring(0, 80), dis: !!(n.disabled || dis) });
        else if (n.localName === "optgroup") { out.push({ t: "g", label: String(n.label || "").substring(0, 80) }); walk(n, n.disabled); }
      }
    };
    walk(sel, false);
    if (out.length > 3000) out.length = 3000;
    var r = sel.getBoundingClientRect(), win = sel.ownerGlobal, x, y, w;
    try { var sr = win.windowUtils.toScreenRect(r.left, r.bottom, r.width, 0); x = sr.x; y = sr.y; w = sr.width; }
    catch (ex) { var d = win.devicePixelRatio || 1; x = (win.mozInnerScreenX + r.left) * d; y = (win.mozInnerScreenY + r.bottom) * d; w = r.width * d; }
    sendAsyncMessage("geckoie:select", { x: Math.round(x), y: Math.round(y), w: Math.round(w), sel: sel.selectedIndex, items: out });
  } catch (ex) {}
}, true);
addMessageListener("geckoie:selected", function (m) {
  try {
    var i = m.data ? m.data.index : -1, sel = selTarget; selTarget = null;
    if (!sel || i < 0) return;
    var opt = sel.options[i]; if (!opt || opt.disabled) return;
    sel.selectedIndex = i; try { sel.focus(); } catch (x) {}
    var W = sel.ownerGlobal;
    sel.dispatchEvent(new W.Event("input", { bubbles: true }));
    sel.dispatchEvent(new W.Event("change", { bubbles: true }));
  } catch (x) {}
});
