/*
 * IEBridge engine bridge (module).
 *
 * One socket server serves the whole engine process, on a loopback-only port. Every browser window is
 * a "view": an Internet Explorer window claims one view (the engine opens a new window when none is
 * free) and then drives it with a simple line protocol.
 *
 * Client -> engine (one command per line):
 *   AUTH <token>        must be the first line
 *   GO <url>            navigate (adds http:// if no scheme)
 *   BACK | FWD | STOP | RELOAD | PING | QUIT (QUIT = release my view)
 *
 * Engine -> client:
 *   HELLO 1 | PID <n> | HWND <handle> | LOC <url> | TITLE <enc> | STATE loading|idle
 *   PROG <0-100> | NAVSTATE <back 0/1> <fwd 0/1> | STATUS <enc>
 *   KEY BACK|FWD | CLASSIC <enc url> | PONG | ERR <enc>
 *   CTX link=<enc> img=<enc> sel=0|1 edit=0|1   (right click in the page)
 * Client -> engine, extra: EDIT copy|cut|paste|selectall|undo|redo
 */
var EXPORTED_SYMBOLS = ["IEBridge"];

const Cc = Components.classes, Ci = Components.interfaces, Cr = Components.results, Cu = Components.utils;
Cu.import("resource://gre/modules/Services.jsm");

var env = Cc["@mozilla.org/process/environment;1"].getService(Ci.nsIEnvironment);
function envGet(name, def) {
  try { return env.exists(name) ? env.get(name) : def; } catch (e) { return def; }
}
var PORT = parseInt(envGet("IEBRIDGE_PORT", "38650"), 10) || 38650;
var TOKEN = envGet("IEBRIDGE_TOKEN", "dev");
var KIOSK = envGet("IEBRIDGE_KIOSK", "0") === "1";
var mainThread = Cc["@mozilla.org/thread-manager;1"].getService(Ci.nsIThreadManager).mainThread;

var server = null;
var conns = [];
var views = [];
var opening = 0;       // engine windows we asked for that have not registered yet
var idleTimer = null;

// ------------------------------------------------------------------ logging

function fileLog(msg) {
  try {
    var dir = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties)
                .get("XREExeF", Ci.nsIFile).parent;
    var f = dir.clone();
    f.append("iebridge-log.txt");
    var fs = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(Ci.nsIFileOutputStream);
    fs.init(f, 0x02 | 0x08 | 0x10, 438, 0); // write | create | append
    var line = "[bridge] " + msg + "\r\n";
    fs.write(line, line.length);
    fs.close();
  } catch (e) {}
}
function log(msg) { fileLog(msg); }
function enc(s) { return encodeURIComponent(s || ""); }

// ------------------------------------------------------------ idle shutdown

function quitApp() {
  Cc["@mozilla.org/toolkit/app-startup;1"].getService(Ci.nsIAppStartup).quit(Ci.nsIAppStartup.eForceQuit);
}

function cancelIdle() {
  if (idleTimer) { try { idleTimer.cancel(); } catch (e) {} idleTimer = null; }
}

// The engine ends itself when nobody is using it, so closing the last IE window frees the memory.
function scheduleIdle(ms) {
  cancelIdle();
  if (envGet("IEBRIDGE_NOIDLE", "0") === "1") return; // hosted inside IE: the host decides when to exit
  idleTimer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
  idleTimer.initWithCallback({
    notify: function () {
      idleTimer = null;
      if (conns.length === 0) { log("no IE window is using the engine, shutting down"); quitApp(); }
    }
  }, ms, Ci.nsITimer.TYPE_ONE_SHOT);
}

// ---------------------------------------------------------------- sending

function send(conn, line) {
  if (!conn || conn.closed) return;
  if (!conn.authed && line.indexOf("HELLO") !== 0) return;
  try {
    var s = line + "\r\n";
    conn.out.write(s, s.length);
  } catch (e) {
    log("send failed: " + e);
    closeConn(conn);
  }
}
function viewSend(v, line) { if (v.conn) send(v.conn, line); }

function sendLoc(v) { try { viewSend(v, "LOC " + v.gb.currentURI.spec); } catch (e) {} }
function sendTitle(v) {
  var t = "";
  try { t = v.gb.selectedBrowser.contentTitle || ""; } catch (e) {}
  viewSend(v, "TITLE " + enc(t));
}
function canGo(v, prop) { try { return !!v.gb[prop]; } catch (e) { return false; } }
function sendNavState(v) {
  viewSend(v, "NAVSTATE " + (canGo(v, "canGoBack") ? 1 : 0) + " " + (canGo(v, "canGoForward") ? 1 : 0));
}
// Back/forward state can lag the navigation event, so send it again shortly after.
function delayedNavState(v) {
  try {
    if (v.navTimer) v.win.clearTimeout(v.navTimer);
    v.navTimer = v.win.setTimeout(function () { v.navTimer = null; sendNavState(v); }, 300);
  } catch (e) {}
}
function hwndOf(v) {
  try {
    var win = v.win;
    var ds = win.docShell || win.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIWebNavigation);
    return ds.QueryInterface(Ci.nsIDocShellTreeItem).treeOwner
             .QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIBaseWindow).nativeHandle;
  } catch (e) { log("hwnd unavailable: " + e); return ""; }
}

// -------------------------------------------------------------------- views

function makeListener(v) {
  return {
    QueryInterface: function (iid) {
      if (iid.equals(Ci.nsIWebProgressListener) ||
          iid.equals(Ci.nsISupportsWeakReference) ||
          iid.equals(Ci.nsISupports)) return this;
      throw Cr.NS_NOINTERFACE;
    },
    onStateChange: function (wp, req, flags, status) {
      if (wp && wp.isTopLevel === false) return;   // only the page itself, not frames inside it
      var L = Ci.nsIWebProgressListener;
      if (flags & L.STATE_IS_NETWORK) {
        if (flags & L.STATE_START) {
          viewSend(v, "STATE loading");
        } else if (flags & L.STATE_STOP) {
          viewSend(v, "STATE idle");
          sendTitle(v);
          sendNavState(v);
          delayedNavState(v);
        }
      }
    },
    onProgressChange: function (wp, req, cur, max, curTotal, maxTotal) {
      if (maxTotal > 0) viewSend(v, "PROG " + Math.min(100, Math.round(curTotal * 100 / maxTotal)));
    },
    onLocationChange: function (wp, req, loc, flags) {
      if (wp && wp.isTopLevel === false) return;
      if (loc) viewSend(v, "LOC " + loc.spec);
      sendNavState(v);
      delayedNavState(v);
    },
    onStatusChange: function (wp, req, status, msg) { viewSend(v, "STATUS " + enc(msg)); },
    onSecurityChange: function () {},
    onContentBlockingEvent: function () {}
  };
}

function hideChrome(v) {
  try {
    var doc = v.win.document;
    var tb = doc.getElementById("navigator-toolbox");
    if (tb) tb.collapsed = true;
    var sb = doc.getElementById("sidebar-box");
    if (sb) sb.hidden = true;
    var ids = ["goBackKb", "goForwardKb", "goBackKb2", "goForwardKb2"];
    for (var i = 0; i < ids.length; i++) {
      var k = doc.getElementById(ids[i]);
      if (k) k.setAttribute("disabled", "true");
    }
  } catch (e) { log("hideChrome failed: " + e); }
}

function installKeys(v) {
  var win = v.win;
  try {
    // Alt+Left / Alt+Right inside the engine drive IE's own history instead of the engine's.
    win.addEventListener("keydown", function (ev) {
      if (ev.altKey && !ev.ctrlKey && !ev.shiftKey && (ev.keyCode === 37 || ev.keyCode === 39)) {
        viewSend(v, "KEY " + (ev.keyCode === 37 ? "BACK" : "FWD"));
        ev.preventDefault();
        ev.stopPropagation();
      }
    }, true);
    // Back/Forward from anywhere in the engine (right-click menu, shortcuts) go through IE's history,
    // so IE's Back/Forward buttons and the engine always agree.
    win.BrowserBack = function () { viewSend(v, "KEY BACK"); };
    win.BrowserForward = function () { viewSend(v, "KEY FWD"); };
  } catch (e) { log("key hooks failed: " + e); }
}

// Adds "Open in classic Internet Explorer window" to the engine's right-click menu.
function installContextItem(v) {
  try {
    var doc = v.win.document;
    var menu = doc.getElementById("contentAreaContextMenu");
    if (!menu) return;
    var make = function (name) {
      return doc.createXULElement ? doc.createXULElement(name) : doc.createElement(name);
    };
    var sep = make("menuseparator");
    var item = make("menuitem");
    item.setAttribute("id", "iebridge-classic");
    item.setAttribute("label", "Open this page in a classic Internet Explorer window");
    item.addEventListener("command", function () {
      var url = "";
      try { url = v.gb.currentURI.spec; } catch (e) {}
      viewSend(v, "CLASSIC " + enc(url));
    }, false);
    menu.appendChild(sep);
    menu.appendChild(item);
  } catch (e) { log("context menu item failed: " + e); }
}

function installContextHook(v) {
  try {
    var mm = v.gb.selectedBrowser.messageManager;
    mm.addMessageListener("geckoie:ctx", function (m) {
      try {
        var d = m.data || {};
        if (d.prevented) { viewSend(v, "CTX prevented=1"); return; }
        viewSend(v, "CTX link=" + enc(d.link) + " img=" + enc(d.img) + " st=" + enc(d.st) + " lt=" + enc(d.lt) + " sel=" + (d.sel ? 1 : 0) + " edit=" + (d.edit ? 1 : 0));
      } catch (e) { log("ctx message failed: " + e); }
    });
    mm.loadFrameScript("chrome://geckoie/content/ctx.js", true);
  } catch (e) { log("context menu hook failed: " + e); }
}

function editCmd(v, name) {
  var map = { copy: "copy", cut: "cut", paste: "paste", selectall: "selectAll", undo: "undo", redo: "redo", "delete": "delete" };
  if (!map[name]) return;
  var b = v.gb.selectedBrowser, ok = false;
  try { ok = b.contentDocument.execCommand(map[name], false, null); } catch (e) { log("execCommand " + name + " failed: " + e); }
  if (ok) return;
  try {
    var ed = b.docShell.contentViewer.QueryInterface(Ci.nsIContentViewerEdit);
    if (name === "copy") ed.copySelection();
    else if (name === "cut") ed.cutSelection();
    else if (name === "paste") ed.paste();
    else if (name === "selectall") ed.selectAll();
  } catch (e) { log("edit " + name + " failed: " + e); }
}

// Page <select> drop-downs, shown by the host as a native menu.
function installSelectHook(v) {
  try {
    var mm = v.gb.selectedBrowser.messageManager;
    mm.addMessageListener("geckoie:select", function (m) {
      try {
        var d = m.data || {}, parts = ["SELECT", "x=" + d.x, "y=" + d.y, "w=" + d.w, "sel=" + d.sel], items = d.items || [];
        for (var i = 0; i < items.length; i++) parts.push((items[i].t === "g" ? "g" : (items[i].dis ? "d" : "o")) + enc(items[i].label));
        viewSend(v, parts.join(" "));
      } catch (e) { log("select message failed: " + e); }
    });
  } catch (e) { log("select hook failed: " + e); }
}

// Downloads are handed to the host (Internet Explorer's download window) instead of the engine's own download UI.
function viewForContext(ctx) {
  try {
    var win = ctx.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindow).top;
    for (var i = 0; i < views.length; i++) if (views[i].gb.selectedBrowser.contentWindow === win) return views[i];
  } catch (e) {}
  for (var j = 0; j < views.length; j++) if (views[j].conn) return views[j];
  return null;
}
var HelperDialog = {
  classID: Components.ID("{b6a2c5e1-3d2f-4a77-9c53-1f6d0a9e7b24}"),
  QueryInterface: function (iid) {
    if (iid.equals(Ci.nsISupports) || iid.equals(Ci.nsIFactory) || iid.equals(Ci.nsIHelperAppLauncherDialog)) return this;
    throw Cr.NS_ERROR_NO_INTERFACE;
  },
  createInstance: function (outer, iid) { if (outer) throw Cr.NS_ERROR_NO_AGGREGATION; return this.QueryInterface(iid); },
  lockFactory: function () {},
  show: function (launcher, ctx, reason) { handOffDownload(launcher, ctx); },
  promptForSaveToFileAsync: function (launcher, ctx, defName, ext, force) { handOffDownload(launcher, ctx); }
};
function handOffDownload(launcher, ctx) {
  var url = "", name = "", mime = "", size = -1;
  try { url = launcher.source.spec; } catch (e) {}
  try { name = launcher.suggestedFileName; } catch (e) {}
  try { mime = launcher.MIMEInfo.MIMEType; } catch (e) {}
  try { size = launcher.contentLength; } catch (e) {}
  try { launcher.cancel(Cr.NS_BINDING_ABORTED); } catch (e) { log("cancel failed: " + e); }
  var v = viewForContext(ctx);
  log("download hand-off: " + url);
  if (v) viewSend(v, "DOWNLOAD url=" + enc(url) + " name=" + enc(name) + " mime=" + enc(mime) + " size=" + size);
}
try {
  Components.manager.QueryInterface(Ci.nsIComponentRegistrar).registerFactory(HelperDialog.classID, "GeckoIE helper app dialog", "@mozilla.org/helperapplauncherdialog;1", HelperDialog);
  log("download hand-off installed");
} catch (e) { log("download hand-off failed: " + e); }

// Developer tools (only offered when they were installed): best effort.
function openDevTools(v) {
  try {
    var m = {};
    Cu.import("resource://devtools/shared/Loader.jsm", m);
    var req = m.require || (m.devtools && m.devtools.require);
    if (!req) throw new Error("devtools loader not found");
    var dt = req("devtools/client/framework/devtools").gDevTools;
    var tf = req("devtools/client/framework/target").TargetFactory;
    Promise.resolve(tf.forTab(v.gb.selectedTab)).then(function (target) { return dt.showToolbox(target, "inspector"); })
      .then(null, function (e) { log("devtools failed: " + e); viewSend(v, "DEVFAIL " + enc(String(e))); });
  } catch (e) { log("devtools unavailable: " + e); viewSend(v, "DEVFAIL " + enc(String(e))); }
}

function addView(win) {
  for (var i = 0; i < views.length; i++) if (views[i].win === win) return;

  // After the first window, only windows we opened ourselves become views (not page popups).
  if (views.length > 0) {
    if (opening <= 0) { log("ignoring an extra window the page opened"); return; }
    opening--;
  }

  var v = { win: win, gb: win.gBrowser, conn: null, navTimer: null };
  if (!v.gb) { log("gBrowser missing in a new window"); return; }
  v.listener = makeListener(v);
  try { v.gb.addProgressListener(v.listener); } catch (e) { log("progress listener failed: " + e); }
  try {
    v.gb.tabContainer.addEventListener("TabAttrModified", function (ev) {
      if (ev.target === v.gb.selectedTab) sendTitle(v);
    }, false);
    v.gb.tabContainer.addEventListener("TabSelect", function () {
      sendLoc(v); sendTitle(v); sendNavState(v);
    }, false);
  } catch (e) { log("tab hooks failed: " + e); }

  installKeys(v);
  installContextItem(v);
  installContextHook(v);
  installSelectHook(v);
  if (KIOSK) hideChrome(v);
  win.addEventListener("unload", function () { removeView(v); }, false);

  views.push(v);
  log("view added (" + views.length + " engine window(s))");
  startServer();
  cancelIdle();
  scheduleIdle(120000); // nobody connected yet: give the first IE window two minutes
  serveWaiting();
}

function removeView(v) {
  var idx = views.indexOf(v);
  if (idx >= 0) views.splice(idx, 1);
  if (v.conn) { var c = v.conn; v.conn = null; c.view = null; closeConn(c); }
  log("view removed (" + views.length + " engine window(s) left)");
}

function freeView() {
  for (var i = 0; i < views.length; i++) if (!views[i].conn) return views[i];
  return null;
}

function openViewWindow() {
  try {
    if (views.length === 0) { log("no engine window to open a new one from"); return; }
    var opener = views[0].win;
    var url = (typeof opener.getBrowserURL === "function")
                ? opener.getBrowserURL() : Services.prefs.getCharPref("toolkit.defaultChromeURI");
    opening++;
    opener.openDialog(url, "_blank",
        "chrome,all,dialog=no,left=-32000,top=-32000,width=1000,height=700", "about:blank");
    log("opened a new engine window");
  } catch (e) {
    opening = Math.max(0, opening - 1);
    log("opening a new engine window failed: " + e);
  }
}

function bindView(conn, v) {
  conn.view = v;
  conn.waiting = false;
  v.conn = conn;
  send(conn, "HELLO 1");
  try {
    send(conn, "PID " + Cc["@mozilla.org/xre/app-info;1"].getService(Ci.nsIXULRuntime).processID);
  } catch (e) {}
  var h = hwndOf(v);
  if (h) send(conn, "HWND " + h);
  sendLoc(v);
  sendNavState(v);
  log("a view was handed to an IE window");
}

function assignView(conn) {
  var v = freeView();
  if (v) { bindView(conn, v); return; }
  conn.waiting = true;
  if (!conn.requested) { conn.requested = true; openViewWindow(); }
}

function serveWaiting() {
  for (var i = 0; i < conns.length; i++) {
    var c = conns[i];
    if (c.authed && !c.view && !c.closed) {
      var v = freeView();
      if (!v) return;
      bindView(c, v);
    }
  }
}

// ---------------------------------------------------------------- commands

function fixupUrl(u) {
  if (/^[a-z][a-z0-9+.\-]*:/i.test(u)) return u;
  return "http://" + u;
}

// Load a URL in the view's tab. The browser's own helpers already know the right loadURI form for this
// engine version, so try those first.
function navigate(v, url) {
  var principal = null;
  try {
    principal = Cc["@mozilla.org/scriptsecuritymanager;1"].getService(Ci.nsIScriptSecurityManager).getSystemPrincipal();
  } catch (e) {}
  var win = v.win, gb = v.gb;
  var attempts = [
    function () { win.openTrustedLinkIn(url, "current"); },
    function () { gb.loadURI(url, { triggeringPrincipal: principal }); },
    function () { gb.selectedBrowser.loadURI(url, { triggeringPrincipal: principal }); },
    function () { gb.loadURI(url); }
  ];
  var errs = [];
  for (var i = 0; i < attempts.length; i++) {
    try { attempts[i](); return; }
    catch (e) { errs.push((i + 1) + ": " + e); }
  }
  throw new Error("all navigation forms failed: " + errs.join(" || "));
}

// Drop telnet negotiation bytes and normalise line endings (CRLF, CR NUL, CR).
function normalize(t) {
  t = t.replace(/\xFF\xFA[\s\S]*?\xFF\xF0/g, "")
       .replace(/\xFF[\xFB-\xFE][\s\S]/g, "")
       .replace(/\xFF[\s\S]/g, "");
  return t.replace(/\r\n/g, "\n").replace(/\r\x00?/g, "\n");
}

// Keep printable ASCII only; backspace/delete erase the previous character.
function cleanLine(line) {
  var out = "";
  for (var i = 0; i < line.length; i++) {
    var ch = line.charCodeAt(i);
    if (ch === 8 || ch === 127) out = out.slice(0, -1);
    else if (ch >= 32 && ch < 127) out += line.charAt(i);
  }
  return out;
}

var ZOOMS = [0.3, 0.5, 0.67, 0.8, 0.9, 1, 1.1, 1.2, 1.33, 1.5, 1.7, 2, 2.4, 3];
function zoomCmd(v, how) {
  try {
    var b = v.gb.selectedBrowser, z = b.fullZoom || 1, i;
    if (how === "reset") z = 1;
    else if (how === "in") { for (i = 0; i < ZOOMS.length; i++) if (ZOOMS[i] > z + 0.001) { z = ZOOMS[i]; break; } }
    else { for (i = ZOOMS.length - 1; i >= 0; i--) if (ZOOMS[i] < z - 0.001) { z = ZOOMS[i]; break; } }
    b.fullZoom = z;
  } catch (e) { log("zoom failed: " + e); }
}

// The engine downloads (it has the modern TLS and the cookies) into a temp folder and reports progress; the host then
// opens the finished file in Internet Explorer, which shows its File Download window.
function fetchCmd(v, arg) {
  var parts = arg.split(" ");
  try {
    var url = decodeURIComponent(parts[0]), name = decodeURIComponent(parts[1] || "") || "download";
    name = name.replace(/[\\\/:*?"<>|]/g, "_");
    var m = {};
    Cu.import("resource://gre/modules/Downloads.jsm", m);
    var dir = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties).get("TmpD", Ci.nsIFile);
    dir.append("GeckoIE"); if (!dir.exists()) dir.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt("755", 8));
    dir.append("dl"); if (!dir.exists()) dir.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt("755", 8));
    dir.append(String(Date.now())); dir.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt("755", 8));
    var f = dir.clone(); f.append(name); var path = f.path;
    m.Downloads.createDownload({ source: { url: url }, target: { path: path } }).then(function (dl) {
      var lastPct = -2, lastT = 0;
      dl.onchange = function () {
        var pct = dl.hasProgress ? dl.progress : -1, now = Date.now();
        if (pct !== lastPct && now - lastT > 300) { lastPct = pct; lastT = now; viewSend(v, "DLPROG " + pct + " " + dl.currentBytes + " " + enc(name)); }
      };
      return dl.start().then(function () { viewSend(v, "DLDONE " + enc(path)); },
                             function (e) { log("download failed: " + e); viewSend(v, "DLFAIL " + enc(String(e))); });
    }, function (e) { log("download failed: " + e); viewSend(v, "DLFAIL " + enc(String(e))); });
  } catch (e) { log("fetch failed: " + e); viewSend(v, "DLFAIL " + enc(String(e))); }
}

function saveCmd(v, arg) {
  var parts = arg.split(" ");
  try {
    var url = decodeURIComponent(parts[0]), path = decodeURIComponent(parts[1] || "");
    var m = {};
    Cu.import("resource://gre/modules/Downloads.jsm", m);
    m.Downloads.fetch(url, path).then(
      function () { viewSend(v, "SAVED ok " + enc(path)); },
      function (e) { log("save failed: " + e); viewSend(v, "SAVED err " + enc(String(e))); });
  } catch (e) { log("save failed: " + e); viewSend(v, "SAVED err " + enc(String(e))); }
}

function handle(conn, line) {
  line = cleanLine(line);
  if (!line) return;
  var sp = line.indexOf(" ");
  var cmd = (sp < 0 ? line : line.substring(0, sp)).toUpperCase();
  var arg = sp < 0 ? "" : line.substring(sp + 1).replace(/^\s+|\s+$/g, "");

  if (!conn.authed) {
    if (cmd === "AUTH" && arg === TOKEN) {
      conn.authed = true;
      assignView(conn);
    } else {
      log("auth failed (got: " + cmd + "), closing");
      closeConn(conn);
    }
    return;
  }

  if (cmd === "PING") { send(conn, "PONG"); return; }
  if (cmd === "QUIT") { closeConn(conn); return; }
  if (cmd === "EXIT") { log("exit requested"); quitApp(); return; }

  var v = conn.view;
  if (!v) return; // still waiting for a window

  switch (cmd) {
    case "GO":     if (arg) navigate(v, fixupUrl(arg)); break;
    case "BACK":   if (canGo(v, "canGoBack")) v.gb.goBack(); break;
    case "FWD":    if (canGo(v, "canGoForward")) v.gb.goForward(); break;
    case "STOP":   v.gb.stop(); break;
    case "RELOAD": v.gb.reload(); break;
    case "EDIT":   editCmd(v, arg.toLowerCase()); break;
    case "ZOOM":   zoomCmd(v, arg.toLowerCase()); break;
    case "SAVE":   saveCmd(v, arg); break;
    case "FETCH":  fetchCmd(v, arg); break;
    case "SELECTED": try { v.gb.selectedBrowser.messageManager.sendAsyncMessage("geckoie:selected", { index: parseInt(arg, 10) }); } catch (e) { log("selected failed: " + e); } break;
    case "DEVTOOLS": openDevTools(v); break;
    case "COPYIMG": try { v.gb.selectedBrowser.messageManager.sendAsyncMessage("geckoie:cmd", { cmd: "copyimage" }); } catch (e) { log("copyimage failed: " + e); } break;
    default:       send(conn, "ERR " + enc("unknown command: " + cmd));
  }
}

// ------------------------------------------------------------------ sockets

function onData(conn, text) {
  conn.buf += normalize(text);
  var i;
  while (!conn.closed && (i = conn.buf.indexOf("\n")) >= 0) {
    var line = conn.buf.substring(0, i);
    conn.buf = conn.buf.substring(i + 1);
    try { handle(conn, line); }
    catch (e) { log("handler error: " + e); send(conn, "ERR " + enc(String(e))); }
  }
  if (!conn.closed && conn.buf.length > 65536) closeConn(conn);
}

function closeConn(conn) {
  if (conn.closed) return;
  conn.closed = true;
  var idx = conns.indexOf(conn);
  if (idx >= 0) conns.splice(idx, 1);
  try { conn.in.close(); } catch (e) {}
  try { conn.out.close(); } catch (e) {}
  try { conn.transport.close(Cr.NS_OK); } catch (e) {}

  var v = conn.view;
  conn.view = null;
  if (v) {
    v.conn = null;
    // The IE window using this view is gone. Close the engine window, unless it is the last one:
    // the idle timer below ends the engine instead.
    if (views.length > 1) {
      try { v.win.close(); } catch (e) { log("closing a view failed: " + e); }
    } else {
      try { navigate(v, "about:blank"); } catch (e) {}
    }
  }
  if (conns.length === 0) scheduleIdle(30000);
}

function waitForData(conn) {
  conn.in.asyncWait({
    onInputStreamReady: function (stream) {
      if (conn.closed) return;
      var n = 0;
      try { n = stream.available(); }
      catch (e) { closeConn(conn); return; }   // closed by the other side
      if (n > 0) onData(conn, conn.sis.read(n));
      if (!conn.closed) waitForData(conn);
    }
  }, 0, 0, mainThread);
}

function accept(serv, transport) {
  var conn = { transport: transport, buf: "", authed: false, closed: false, view: null, waiting: false, requested: false };
  conn.in = transport.openInputStream(0, 0, 0);
  conn.out = transport.openOutputStream(Ci.nsITransport.OPEN_BLOCKING, 0, 0);
  conn.sis = Cc["@mozilla.org/scriptableinputstream;1"].createInstance(Ci.nsIScriptableInputStream);
  conn.sis.init(conn.in);
  conns.push(conn);
  cancelIdle();
  waitForData(conn);
}

function startServer() {
  if (server) return;
  try {
    server = Cc["@mozilla.org/network/server-socket;1"].createInstance(Ci.nsIServerSocket);
    server.init(PORT, true, -1); // loopback only
    server.asyncListen({ onSocketAccepted: accept, onStopListening: function () {} });
    log("listening on 127.0.0.1:" + PORT + " (kiosk=" + KIOSK + ")");
  } catch (e) {
    server = null;
    log("could not start server: " + e);
  }
}

var IEBridge = {
  // A page asked for a new window (target=_blank, window.open): the host opens it as a new Internet Explorer window.
  openInNewWindow: function (win, url) {
    for (var i = 0; i < views.length; i++) if (views[i].win === win && views[i].conn) { viewSend(views[i], "NEWWIN url=" + enc(url)); return true; }
    return false;
  },
  addView: addView
};
