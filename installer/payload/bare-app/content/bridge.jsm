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
 *   PROG <0-100> | NAVSTATE <back 0/1> <fwd 0/1> | STATUS <enc> | ZOOMED <factor> | FAVICON <ico path>|<page url>
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
    if ((++bridgeLogN % 100) === 1 && f.exists() && f.fileSize > 400000) f.remove(false);   // never let the log grow without limit
    var fs = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(Ci.nsIFileOutputStream);
    fs.init(f, 0x02 | 0x08 | 0x10, 438, 0); // write | create | append
    var d = new Date(), line = "[bridge] " + d.toTimeString().substring(0, 8) + "." + ("00" + d.getMilliseconds()).slice(-3) + " " + msg + "\r\n";
    fs.write(line, line.length);
    fs.close();
  } catch (e) {}
}
var bridgeLogN = 0;
function log(msg) { fileLog(msg); if (devLogOn) dlog(msg); }

// Developer-tools log: %TEMP%\GeckoIE\devtools-log.txt (rewritten every time Inspect Element is used).
var devLogOn = false, devLogLines = 0, devConsoleListener = null;
function devLogFile() {
  var f = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties).get("TmpD", Ci.nsIFile);
  f.append("GeckoIE");
  if (!f.exists()) f.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt("755", 8));
  f.append("devtools-log.txt");
  return f;
}
function dlog(msg, reset) {
  try {
    if (!reset && ++devLogLines > 600) return;
    var fs = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(Ci.nsIFileOutputStream);
    fs.init(devLogFile(), reset ? 0x02 | 0x08 | 0x20 : 0x02 | 0x08 | 0x10, 438, 0);
    var t = new Date(), ts = ("0" + t.getMinutes()).slice(-2) + ":" + ("0" + t.getSeconds()).slice(-2) + "." + ("00" + t.getMilliseconds()).slice(-3);
    var line = "[" + ts + "] " + String(msg).replace(/\r?\n/g, " | ") + "\r\n";
    fs.write(line, line.length);
    fs.close();
  } catch (e) {}
}
function startDevLog() {
  devLogOn = true; devLogLines = 0;
  var info = "";
  try { info = Services.appinfo.name + " " + Services.appinfo.version + " platform " + Services.appinfo.platformVersion + " build " + Services.appinfo.appBuildID; } catch (e) {}
  dlog("GeckoIE developer tools log - " + info, true);
  if (devConsoleListener) return;
  try {
    devConsoleListener = {
      observe: function (m) {
        try {
          var txt = m.message || String(m);
          if (/devtools|toolbox|geckoie|TypeError|ReferenceError|NS_ERROR|Exception/i.test(txt)) dlog("[console] " + txt.substring(0, 700));
        } catch (e) {}
      },
      QueryInterface: function () { return this; }
    };
    Services.console.registerListener(devConsoleListener);
  } catch (e) { dlog("console listener failed: " + e); }
  // devtools code reports most failures through console.error()/warn() (Console.jsm): copy those into the log too.
  try {
    var cm = {}; Cu.import("resource://gre/modules/Console.jsm", cm);
    var c = cm.console;
    if (c && !c.__geckoieWrapped) {
      c.__geckoieWrapped = true;
      ["error", "warn", "exception"].forEach(function (k) {
        var orig = c[k]; if (typeof orig !== "function") return;
        c[k] = function () {
          try {
            var parts = [];
            for (var i = 0; i < arguments.length; i++) {
              var a = arguments[i];
              parts.push(a && a.stack ? String(a) + " STACK " + String(a.stack).split("\n").slice(0, 6).join(" < ") : String(a));
            }
            dlog("[console." + k + "] " + parts.join(" ").substring(0, 900));
          } catch (e) {}
          return orig.apply(this, arguments);
        };
      });
    }
  } catch (e) { dlog("console wrap failed: " + e); }
}
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
          sendZoom(v);
          if (!v.favTimer) try { v.favTimer = v.win.setTimeout(function () { v.favTimer = null; sendFavicon(v, false); }, 1500); } catch (e) {}   // after the page has settled
          delayedNavState(v);
        }
      }
    },
    onProgressChange: function () {},   // PROG lines used to be sent hundreds of times per page and nobody read them
    onLocationChange: function (wp, req, loc, flags) {
      if (wp && wp.isTopLevel === false) return;
      if (loc) viewSend(v, "LOC " + loc.spec);
      sendNavState(v);
      delayedNavState(v);
    },
    onStatusChange: function (wp, req, status, msg) {   // "Waiting for..." changes many times a second while loading: send the latest every 150 ms
      v.pendingStatus = msg || "";
      if (v.statusTimer) return;
      try {
        v.statusTimer = v.win.setTimeout(function () {
          v.statusTimer = null;
          if (v.pendingStatus !== v.lastStatus) { v.lastStatus = v.pendingStatus; viewSend(v, "STATUS " + enc(v.lastStatus)); }
        }, 150);
      } catch (e) { viewSend(v, "STATUS " + enc(msg)); }
    },
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
    win.addEventListener("keydown", function (ev) {
      if (ev.defaultPrevented || !ev.ctrlKey || ev.altKey) return;
      var k = ev.key, done = true;
      if (!ev.shiftKey && (k === "p" || k === "P")) viewSend(v, "KEY PRINT");
      else if (!ev.shiftKey && (k === "s" || k === "S")) viewSend(v, "KEY SAVEAS");
      else if (k === "+" || k === "=") zoomCmd(v, "in");
      else if (k === "-" || k === "_") zoomCmd(v, "out");
      else if (k === "0" && !ev.shiftKey) zoomCmd(v, "reset");
      else done = false;
      if (done) { ev.preventDefault(); ev.stopPropagation(); }
    }, false);
    // Ctrl + mouse wheel: Gecko asks its chrome to zoom through these events
    win.addEventListener("DoZoomEnlargeBy10", function () { zoomCmd(v, "in"); }, true);
    win.addEventListener("DoZoomReduceBy10", function () { zoomCmd(v, "out"); }, true);
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
// The developer tools are part of Firefox's own browser app (<engine>/browser/omni.ja). This bare app never loads that
// jar, so the tools' resource:// and chrome:// entries and their default preferences are registered here, on demand.
function ensureDevTools() {
  var res = Cc["@mozilla.org/network/protocol;1?name=resource"].getService(Ci.nsIResProtocolHandler);
  if (res.hasSubstitution("devtools")) return;
  var omni = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties).get("GreD", Ci.nsIFile);
  omni.append("browser"); omni.append("omni.ja");
  if (!omni.exists()) throw new Error("browser/omni.ja not found: " + omni.path);
  var ios = Cc["@mozilla.org/network/io-service;1"].getService(Ci.nsIIOService);
  var base = "jar:" + ios.newFileURI(omni).spec + "!/";
  var zr = Cc["@mozilla.org/libjar/zip-reader;1"].createInstance(Ci.nsIZipReader);
  zr.open(omni);
  var out = [];
  try {
    var readText = function (name) {
      var sis = Cc["@mozilla.org/scriptableinputstream;1"].createInstance(Ci.nsIScriptableInputStream);
      sis.init(zr.getInputStream(name));
      var s = sis.read(sis.available()); sis.close(); return s;
    };
    var walk = function (name, depth) {
      if (depth > 4 || !zr.hasEntry(name)) return;
      var dir = name.indexOf("/") >= 0 ? name.substring(0, name.lastIndexOf("/") + 1) : "";
      var lines = readText(name).split(/\r?\n/);
      for (var i = 0; i < lines.length; i++) {
        var l = lines[i].replace(/#.*$/, "").trim();
        if (!l) continue;
        var t = l.split(/\s+/), kw = t[0];
        if (kw === "manifest" && t[1]) { walk(dir + t[1], depth + 1); continue; }
        var pi = (kw === "content" || kw === "resource") ? 2 : ((kw === "skin" || kw === "locale") ? 3 : -1);
        if (pi < 0 || !t[pi] || !/devtools/i.test(l)) continue;
        if (!/^[a-z][a-z0-9+.\-]*:/i.test(t[pi])) t[pi] = base + dir + t[pi];
        out.push(t.join(" "));
      }
    };
    walk("chrome.manifest", 0);
    var prefs = Cc["@mozilla.org/preferences-service;1"].getService(Ci.nsIPrefService).getDefaultBranch("");
    var en = zr.findEntries("defaults/preferences/*.js"), nPref = 0;
    while (en.hasMore()) {
      var txt = readText(en.getNext());
      var re = /^\s*(?:sticky_)?pref\(\s*"([^"]+)"\s*,\s*([^;]*?)\s*\)\s*;/gm, mm;
      while ((mm = re.exec(txt))) {
        if (!/^devtools[.\-]/.test(mm[1])) continue;
        var v = mm[2];
        try {
          if (v === "true" || v === "false") prefs.setBoolPref(mm[1], v === "true");
          else if (/^-?\d+$/.test(v)) prefs.setIntPref(mm[1], parseInt(v, 10));
          else if (v.charAt(0) === '"') prefs.setCharPref(mm[1], JSON.parse(v));
          else continue;
          nPref++;
        } catch (e) {}
      }
    }
    log("devtools: " + out.length + " manifest entries, " + nPref + " default prefs from browser/omni.ja");
  } finally { zr.close(); }
  if (!out.length) throw new Error("no developer tools entries in browser/omni.ja");
  var f = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties).get("TmpD", Ci.nsIFile);
  f.append("GeckoIE"); if (!f.exists()) f.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt("755", 8));
  f.append("devtools.manifest");
  var fos = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(Ci.nsIFileOutputStream);
  fos.init(f, 0x02 | 0x08 | 0x20, parseInt("644", 8), 0);
  var data = out.join("\n") + "\n"; fos.write(data, data.length); fos.close();
  Components.manager.QueryInterface(Ci.nsIComponentRegistrar).autoRegister(f);
  if (!res.hasSubstitution("devtools")) throw new Error("resource://devtools is still not registered");
}

// toolbox.js grabs @mozilla.org/devtools/startup-clh;1 (Firefox's DevToolsStartup.jsm, which lives in browser/omni.ja and is
// never registered by this bare app) while it loads, and only reads .KeyShortcuts from it. Provide a stand-in when it is missing.
var devStartupStub = null;
function ensureDevStartupStub() {
  var contract = "@mozilla.org/devtools/startup-clh;1";
  try { Cc[contract].getService(Ci.nsISupports); return; } catch (e) {}
  var obj = {
    KeyShortcuts: [],
    QueryInterface: function () { return this; }
  };
  obj.wrappedJSObject = obj;
  var factory = {
    createInstance: function (outer, iid) {
      if (outer) throw Cr.NS_ERROR_NO_AGGREGATION;
      return obj;
    },
    lockFactory: function () {},
    QueryInterface: function () { return this; }
  };
  var reg = Components.manager.QueryInterface(Ci.nsIComponentRegistrar);
  reg.registerFactory(Components.ID("{5b2f7a64-8d1c-4e39-9a0b-7c3e1d6f2a48}"), "GeckoIE devtools startup stand-in", contract, factory);
  devStartupStub = factory;   // keep it alive
  log("devtools: registered stand-in for " + contract);
}

// Toolbox.open() starts _buildTabs() without waiting for it. If anything in it throws (a tool that needs a browser-window
// feature this bare app lacks), panelDefinitions is never set and the later selectTool() dies with
// "this.panelDefinitions is undefined". This version builds each tool on its own, skips (and logs) the ones that fail,
// and always sets panelDefinitions.
var devNotes = [];
function devNote(msg) { log("devtools: " + msg); if (devNotes.length < 8) devNotes.push(msg); }
function patchToolbox(req, dt) {
  var Toolbox = req("devtools/client/framework/toolbox").Toolbox;
  if (!Toolbox || !Toolbox.prototype || Toolbox.prototype.__geckoieTabs) return;
  Toolbox.prototype.__geckoieTabs = true;
  var origOpen = Toolbox.prototype.open, origDestroy = Toolbox.prototype.destroy;
  if (typeof origOpen === "function") Toolbox.prototype.open = function () {
    dlog("Toolbox.open start, host=" + this.hostType);
    var p;
    try { p = origOpen.apply(this, arguments); } catch (e) { dlog("Toolbox.open threw: " + e + " " + (e && e.stack)); throw e; }
    return Promise.resolve(p).then(function (r) { dlog("Toolbox.open finished OK"); return r; },
      function (e) { dlog("Toolbox.open rejected: " + e + " " + (e && e.stack)); throw e; });
  };
  if (typeof origDestroy === "function") Toolbox.prototype.destroy = function () {
    var st = ""; try { st = new Error().stack.split("\n").slice(1, 7).join(" < "); } catch (e) {}
    dlog("Toolbox.destroy called, window closed=" + (this.win ? this.win.closed : "n/a") + " from: " + st);
    return origDestroy.apply(this, arguments);
  };
  // Anything that reads the list before _buildTabs has filled it (e.g. a tool-registered event) must not crash.
  if (!Toolbox.prototype._panelDefinitions) Toolbox.prototype._panelDefinitions = [];
  Toolbox.prototype._buildTabs = function () {
    var self = this;
    var defs = [];
    try { defs = dt.getToolDefinitionArray(); } catch (e) { devNote("tool list: " + e); }
    defs.forEach(function (d) {
      try { self._buildPanelForTool(d); } catch (e) { devNote("panel " + d.id + ": " + e); }
    });
    var supported = defs.filter(function (d) {
      try { return d.isTargetSupported(self.target) && d.id !== "options"; }
      catch (e) { devNote("support " + d.id + ": " + e); return false; }
    });
    log("devtools: tools available: " + supported.map(function (d) { return d.id; }).join(","));
    self.panelDefinitions = supported;
    return Promise.resolve().then(function () {
      if (self.disableAutohideAvailable)
        return self._isDisableAutohideEnabled().then(function (d) { self.component.setDisableAutohide(d); });
    }).then(null, function (e) { devNote("autohide: " + e); });
  };
}

// The native host (GeckoHost.dll) hides every top-level engine window whose title is empty or contains "GeckoIE", because
// those are docked into Internet Explorer. The toolbox window is created with an empty title, so it was shown and then
// hidden again right away. Once the toolbox has set its own title the hook no longer matches it, so show it again then.
var devTimers = [];
function devAfter(ms, fn) {
  var t = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
  devTimers.push(t);
  t.initWithCallback({ notify: function () { try { fn(); } catch (e) { dlog("timer: " + e); } } }, ms, Ci.nsITimer.TYPE_ONE_SHOT);
}
function reshowToolbox(tb, tries) {
  var top = null;
  try { top = tb.win && tb.win.top; } catch (e) {}
  if (!top) { dlog("reshow: no toolbox window"); return; }
  var title = "";
  try { title = top.document.title || ""; } catch (e) {}
  dlog("reshow try " + tries + ": title='" + title + "' closed=" + top.closed + " pos=" + top.screenX + "," + top.screenY + " size=" + top.outerWidth + "x" + top.outerHeight);
  if ((!title || /GeckoIE/i.test(title)) && tries < 8) {
    if (tries >= 3 && !title) { try { top.document.title = "Developer Tools"; } catch (e) {} }
    devAfter(400, function () { reshowToolbox(tb, tries + 1); });
    return;
  }
  try {
    if (top.outerWidth < 600 || top.outerHeight < 400) top.resizeTo(1000, 700);
    if (top.screenX < -1000 || top.screenY < -1000 || top.screenX > 5000) top.moveTo(80, 60);
  } catch (e) { dlog("reshow size: " + e); }
  try {
    var owner = top.docShell.treeOwner;
    var bw = owner.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIBaseWindow);
    bw.visibility = false;
    bw.visibility = true;
    dlog("reshow: window shown again");
  } catch (e) { dlog("reshow show failed: " + e); }
  try { top.focus(); } catch (e) {}
}

// The toolbox UI is loaded into its host window as about:devtools-toolbox. That about: page is registered by Firefox's
// browser app (AboutDevToolsToolboxRegistration.jsm), which this bare app never loads, so the window showed
// "The file about:devtools-toolbox cannot be found". Register the same handler here, under our own CID: the original CID
// is already known to the engine (its module just fails to load), so reusing it gives NS_ERROR_FACTORY_EXISTS.
var devAboutModule = null;
function ensureAboutToolbox() {
  var contract = "@mozilla.org/network/protocol/about;1?what=devtools-toolbox";
  try { Cc[contract].getService(Ci.nsISupports); dlog("about:devtools-toolbox already available"); return; } catch (e) {}
  var mod = {
    newChannel: function (uri, loadInfo) {
      var target = Services.io.newURI("chrome://devtools/content/framework/toolbox.xhtml");
      var chan = Services.io.newChannelFromURIWithLoadInfo(target, loadInfo);
      chan.owner = Services.scriptSecurityManager.getSystemPrincipal();
      return chan;
    },
    getURIFlags: function (uri) {
      return Ci.nsIAboutModule.ALLOW_SCRIPT | Ci.nsIAboutModule.ENABLE_INDEXED_DB | Ci.nsIAboutModule.HIDE_FROM_ABOUTABOUT;
    },
    QueryInterface: function () { return this; }
  };
  var factory = {
    createInstance: function (outer, iid) { if (outer) throw Cr.NS_ERROR_NO_AGGREGATION; return mod; },
    lockFactory: function () {},
    QueryInterface: function () { return this; }
  };
  Components.manager.QueryInterface(Ci.nsIComponentRegistrar)
    .registerFactory(Components.ID("{7d3a9c52-4e18-4b6f-a1d0-2f8b5c9e6a73}"), "about:devtools-toolbox", contract, factory);
  devAboutModule = factory;   // keep alive
  dlog("registered about:devtools-toolbox");
}

function openDevTools(v) {
  var step = "start";
  startDevLog();
  dlog("Inspect Element requested");
  var fail = function (e) {
    dlog("FAILED at: " + step + " - " + e + " " + (e && e.stack ? e.stack : ""));
    var where = "";
    try {
      if (e && e.stack) where = " @ " + String(e.stack).split("\n").slice(0, 3).join(" < ")
        .replace(/resource:\/\/devtools\/(client\/)?/g, "").replace(/resource:\/\/gre\/modules\//g, "");
    } catch (e5) {}
    var msg = step + " - " + String(e) + where + (devNotes.length ? " [" + devNotes.join("; ") + "]" : "");
    log("devtools failed at: " + msg);
    viewSend(v, "DEVFAIL " + enc(msg.substring(0, 600)));
  };
  try {
    step = "making the developer tools available (browser/omni.ja)";
    ensureDevTools();
    step = "registering the devtools startup stand-in";
    ensureDevStartupStub();
    step = "registering about:devtools-toolbox";
    ensureAboutToolbox();
    step = "loading resource://devtools/shared/Loader.jsm";
    var m = {};
    Cu.import("resource://devtools/shared/Loader.jsm", m);
    var req = m.require || (m.devtools && m.devtools.require);
    if (!req) throw new Error("the loader has no require()");
    step = "loading devtools/client/framework/devtools";
    var dt = req("devtools/client/framework/devtools").gDevTools;
    step = "registering the tools";
    try { req("devtools/client/framework/devtools-browser"); } catch (e2) { log("devtools-browser: " + e2); }
    step = "making the toolbox fault-tolerant";
    devNotes = [];
    patchToolbox(req, dt);
    step = "closing leftovers of an earlier attempt";
    try {
      if (dt._toolboxes) dt._toolboxes.forEach(function (tb) { try { tb.destroy(); } catch (e3) {} });
      if (dt._toolboxes) dt._toolboxes.clear();
      if (dt._creatingToolboxes) dt._creatingToolboxes.clear();
    } catch (e4) { log("cleanup: " + e4); }
    step = "getting a debug target for the page";
    var tf = req("devtools/client/framework/target").TargetFactory;
    Promise.resolve(tf.forTab(v.gb.selectedTab)).then(function (target) {
      step = "opening the toolbox window";
      dlog("showToolbox: target ready, localTab=" + !!target.localTab + " isLocalTab=" + target.isLocalTab);
      return dt.showToolbox(target, "inspector", "window");
    }).then(function (tb) {
      dlog("showToolbox resolved: " + (tb ? "toolbox object" : String(tb)));
      if (tb) devAfter(300, function () { reshowToolbox(tb, 0); });
    }, fail);
  } catch (e) { fail(e); }
}

// The engine window sits inside Internet Explorer, so Gecko never gets the usual "window activated" signal; without it the
// text caret stays invisible. The host sends the activation itself and tells the page side here.
function activeCmd(v, on) {
  try {
    var fm = Cc["@mozilla.org/focus-manager;1"].getService(Ci.nsIFocusManager);
    if (on) {
      if (fm.activeWindow !== v.win) { try { if (typeof fm.windowRaised === "function") fm.windowRaised(v.win); } catch (e1) { log("windowRaised: " + e1); } }
      try { v.gb.selectedBrowser.docShell.isActive = true; } catch (e2) {}
    } else { try { fm.windowLowered(v.win); } catch (e3) {} }
  } catch (e) { log("active failed: " + e); }
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
      sendLoc(v); sendTitle(v); sendNavState(v); sendZoom(v);
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
  try { if (v.gb.selectedBrowser.fullZoom !== 1) v.gb.selectedBrowser.fullZoom = 1; } catch (e) {}   // every IE window starts at 100%
  send(conn, "HELLO 1");
  try {
    var hasDev = Cc["@mozilla.org/network/protocol;1?name=resource"].getService(Ci.nsIResProtocolHandler).hasSubstitution("devtools");
    if (!hasDev) { var bo = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties).get("GreD", Ci.nsIFile); bo.append("browser"); bo.append("omni.ja"); hasDev = bo.exists(); }
    send(conn, "CAPS devtools=" + (hasDev ? 1 : 0));
  } catch (e) { log("caps failed: " + e); }
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
// ---- known advertising and tracking servers are not even contacted (less to download, less script to run).
// Switch: Settings window / BlockAds in settings.ini. Pages the user opens directly are never blocked.
var BLOCK_HOSTS = ["doubleclick.net","googlesyndication.com","googleadservices.com","google-analytics.com","2mdn.net","adnxs.com","adsrvr.org",
  "advertising.com","scorecardresearch.com","quantserve.com","quantcount.com","outbrain.com","taboola.com","criteo.com","criteo.net","rubiconproject.com",
  "pubmatic.com","openx.net","casalemedia.com","moatads.com","amazon-adsystem.com","adform.net","smartadserver.com","bluekai.com","krxd.net",
  "demdex.net","omtrdc.net","everesttech.net","mathtag.com","rlcdn.com","bidswitch.net","contextweb.com","yieldmo.com","sharethrough.com",
  "33across.com","lijit.com","sovrn.com","indexww.com","adsafeprotected.com","doubleverify.com","serving-sys.com","tapad.com","agkn.com",
  "mookie1.com","hotjar.com","mouseflow.com","crazyegg.com","chartbeat.com","adition.com","teads.tv","adscale.de","zemanta.com","revcontent.com"];
var BLOCK_PATHS = [["google.com", "/pagead"], ["youtube.com", "/pagead"], ["youtube.com", "/api/stats/ads"], ["facebook.com", "/tr"]];
function installBlocker() {
  try {
    var env = Cc["@mozilla.org/process/environment;1"].getService(Ci.nsIEnvironment);
    if (env.exists("IEBRIDGE_BLOCKADS") && env.get("IEBRIDGE_BLOCKADS") === "0") { log("ad and tracker blocking is off"); return; }
    var set = {}; BLOCK_HOSTS.forEach(function (h) { set[h] = 1; });
    Services.obs.addObserver({
      observe: function (subject) {
        try {
          var ch = subject.QueryInterface(Ci.nsIHttpChannel);
          var ct = ch.loadInfo ? ch.loadInfo.externalContentPolicyType : 0;
          if (ct === 6) return;   // TYPE_DOCUMENT: a page the user opened is never blocked (frames inside a page are)
          var host = ch.URI.host, h = host, i;
          for (;;) { if (set[h]) { ch.cancel(0x804B0002); return; } i = h.indexOf("."); if (i < 0) break; h = h.substring(i + 1); }
          var path = ch.URI.filePath;
          for (i = 0; i < BLOCK_PATHS.length; i++) {
            var hp = BLOCK_PATHS[i][0];
            if ((host === hp || host.length > hp.length && host.substring(host.length - hp.length - 1) === "." + hp) && path.indexOf(BLOCK_PATHS[i][1]) === 0) { ch.cancel(0x804B0002); return; }
          }
        } catch (e) {}
      }
    }, "http-on-modify-request");
    log("ad and tracker blocking is on (" + BLOCK_HOSTS.length + " servers)");
  } catch (e) { log("blocker failed: " + e); }
}
installBlocker();
function sendZoom(v) { try { viewSend(v, "ZOOMED " + (v.gb.selectedBrowser.fullZoom || 1).toFixed(2)); } catch (e) {} }
function zoomCmd(v, how) {   // how: in | out | reset | get | set <factor>
  try {
    var b = v.gb.selectedBrowser, z = b.fullZoom || 1, i;
    how = String(how || ""); log("zoom " + how + " (now " + z + ")");
    if (how === "get") { sendZoom(v); return; }
    if (how === "reset") z = 1;
    else if (how === "in") { for (i = 0; i < ZOOMS.length; i++) if (ZOOMS[i] > z + 0.001) { z = ZOOMS[i]; break; } }
    else if (how === "out") { for (i = ZOOMS.length - 1; i >= 0; i--) if (ZOOMS[i] < z - 0.001) { z = ZOOMS[i]; break; } }
    else if (how.indexOf("set ") === 0) { var f = parseFloat(how.substring(4)); if (!(f > 0)) return; z = Math.max(0.3, Math.min(3, f)); }
    else return;
    b.fullZoom = z;
    sendZoom(v);
  } catch (e) { log("zoom failed: " + e); }
}
// ---- favicon: find the page's icon, turn it into a 16x16 .ico that Windows XP can read, tell the IE side where it is
function icoBytes(d) {   // d = RGBA pixels of a 16x16 image
  var N = 16, b = [];
  function w16(x) { b.push(x & 255, (x >> 8) & 255); }
  function w32(x) { b.push(x & 255, (x >> 8) & 255, (x >> 16) & 255, (x >> 24) & 255); }
  w16(0); w16(1); w16(1); b.push(N, N, 0, 0); w16(1); w16(32); w32(40 + N * N * 4 + N * 4); w32(22);
  w32(40); w32(N); w32(N * 2); w16(1); w16(32); w32(0); w32(N * N * 4 + N * 4); w32(0); w32(0); w32(0); w32(0);
  for (var y = N - 1; y >= 0; y--) for (var x = 0; x < N; x++) { var i = (y * N + x) * 4; b.push(d[i + 2], d[i + 1], d[i], d[i + 3]); }
  for (var k = 0; k < N * 4; k++) b.push(0);
  return b;
}
function strHash(str) { var h = 5381; for (var i = 0; i < str.length; i++) h = ((h * 33) ^ str.charCodeAt(i)) >>> 0; return h.toString(16); }
function pageIconUrl(v) {
  try {
    var doc = v.gb.selectedBrowser.contentDocument, base = doc.documentURI;
    if (!/^https?:/i.test(base)) return "";
    var links = doc.querySelectorAll("link[rel]");
    for (var i = 0; i < links.length; i++) {
      var rel = (links[i].getAttribute("rel") || "").toLowerCase().split(/\s+/);
      if (rel.indexOf("icon") < 0) continue;
      var h = links[i].href; if (/^(https?:|data:image)/i.test(h)) return h;
    }
    return Services.io.newURI("/favicon.ico", null, Services.io.newURI(base)).spec;
  } catch (e) { return ""; }
}
function sendFavicon(v, force) {
  try {
    var page = v.gb.selectedBrowser.currentURI.spec, icon = pageIconUrl(v);
    if (!icon) return;
    if (!force && v.lastIconSrc === page + "\n" + icon) return;
    v.lastIconSrc = page + "\n" + icon;
    var dir = Services.dirsvc.get("ProfD", Ci.nsIFile); dir.append("favicons");
    if (!dir.exists()) dir.create(Ci.nsIFile.DIRECTORY_TYPE, parseInt("755", 8));
    var f = dir.clone(); f.append(strHash(icon) + ".ico");
    function done() { v.lastFav = f.path + "|" + page; viewSend(v, "FAVICON " + v.lastFav); }
    if (f.exists() && f.fileSize === 1128 && (Date.now() - f.lastModifiedTime) < 7 * 86400000) { done(); return; }
    var win = v.win, img = new win.Image();
    img.onload = function () {
      try {
        var c = win.document.createElementNS("http://www.w3.org/1999/xhtml", "canvas"); c.width = 16; c.height = 16;
        var cx = c.getContext("2d"); cx.drawImage(img, 0, 0, 16, 16);
        var bytes = icoBytes(cx.getImageData(0, 0, 16, 16).data);
        var fos = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(Ci.nsIFileOutputStream);
        fos.init(f, 0x02 | 0x08 | 0x20, parseInt("644", 8), 0);
        var bos = Cc["@mozilla.org/binaryoutputstream;1"].createInstance(Ci.nsIBinaryOutputStream);
        bos.setOutputStream(fos); bos.writeByteArray(bytes, bytes.length); bos.close(); fos.close();
        done();
      } catch (e) { log("favicon convert failed: " + e); }
    };
    img.onerror = function () { log("favicon load failed: " + icon); };
    img.src = icon;
  } catch (e) { log("favicon failed: " + e); }
}
function printCmd(v) { try { v.gb.selectedBrowser.contentWindow.print(); } catch (e) { log("print failed: " + e); } }

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
    case "PRINT":  printCmd(v); break;
    case "TITLE":  if (arg === "get") sendTitle(v); break;
    case "FAVICON": if (arg === "get" && v.lastFav) viewSend(v, "FAVICON " + v.lastFav); break;
    case "SAVE":   saveCmd(v, arg); break;
    case "FETCH":  fetchCmd(v, arg); break;
    case "SELECTED": try { v.gb.selectedBrowser.messageManager.sendAsyncMessage("geckoie:selected", { index: parseInt(arg, 10) }); } catch (e) { log("selected failed: " + e); } break;
    case "DEVTOOLS": openDevTools(v); break;
    case "ACTIVE": activeCmd(v, arg.charAt(0) === "1"); break;
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
    try { v.gb.selectedBrowser.fullZoom = 1; } catch (e) {}
    v.lastFav = ""; v.lastIconSrc = "";
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

var serverTries = 0, serverTimer = null;
function startServer() {
  if (server) return;
  try {
    server = Cc["@mozilla.org/network/server-socket;1"].createInstance(Ci.nsIServerSocket);
    server.init(PORT, true, -1); // loopback only
    server.asyncListen({ onSocketAccepted: accept, onStopListening: function () {} });
    log("listening on 127.0.0.1:" + PORT + " (kiosk=" + KIOSK + ")");
  } catch (e) {
    server = null;
    log("could not start server (attempt " + (serverTries + 1) + "): " + e);
    // the socket service can be a moment late right after a fresh start: try again instead of leaving IE without an engine
    if (++serverTries < 12) {
      serverTimer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
      serverTimer.initWithCallback({ notify: function () { startServer(); } }, 500, Ci.nsITimer.TYPE_ONE_SHOT);
    }
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
