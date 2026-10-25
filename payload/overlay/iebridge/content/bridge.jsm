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
                ? opener.getBrowserURL() : "chrome://browser/content/browser.xul";
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

  var v = conn.view;
  if (!v) return; // still waiting for a window

  switch (cmd) {
    case "GO":     if (arg) navigate(v, fixupUrl(arg)); break;
    case "BACK":   if (canGo(v, "canGoBack")) v.gb.goBack(); break;
    case "FWD":    if (canGo(v, "canGoForward")) v.gb.goForward(); break;
    case "STOP":   v.gb.stop(); break;
    case "RELOAD": v.gb.reload(); break;
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
  addView: addView
};
