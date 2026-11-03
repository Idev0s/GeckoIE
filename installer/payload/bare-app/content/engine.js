/*
 * GeckoIE bare engine test.
 * One content area and nothing else: no tabs, no toolbars, no Firefox browser window.
 * Loads the address in the GECKOIE_URL environment variable and logs what happens to
 * geckoie-bare-log.txt in the engine folder.
 */
"use strict";

(function () {
  try { document.title = "GeckoIE bare engine"; } catch (e) {}   // known to the host before the window is ever shown
  try { window.moveTo(-32000, -32000); } catch (e) {}   // never visible: the host docks this window into Internet Explorer
  const Cc = Components.classes, Ci = Components.interfaces, Cu = Components.utils;

  function logPath() {
    try {
      const dir = Cc["@mozilla.org/file/directory_service;1"].getService(Ci.nsIProperties)
                    .get("XREExeF", Ci.nsIFile).parent;
      dir.append("geckoie-bare-log.txt");
      return dir;
    } catch (e) { return null; }
  }

  let logN = 0;
  function log(msg) {
    dump("[geckoie] " + msg + "\n");
    if (/^title: /.test(msg)) return;   // pages change their title all the time: not worth a disk write each
    try {
      const f = logPath();
      if (!f) return;
      if ((++logN % 50) === 1 && f.exists() && f.fileSize > 300000) f.remove(false);
      const s = Cc["@mozilla.org/network/file-output-stream;1"].createInstance(Ci.nsIFileOutputStream);
      s.init(f, 0x02 | 0x08 | 0x10, 438, 0); // write | create | append
      const line = "[bare] " + msg + "\r\n";
      s.write(line, line.length);
      s.close();
    } catch (e) {}
  }

  function describe(b) {
    const bits = [];
    for (const name of ["loadURI", "webNavigation", "webProgress", "docShell", "browsingContext", "currentURI"]) {
      let v;
      try { v = typeof b[name] === "function" ? "function" : (b[name] ? "yes" : "no"); }
      catch (e) { v = "error: " + e; }
      bits.push(name + "=" + v);
    }
    try { bits.push("remote=" + b.isRemoteBrowser); } catch (e) {}
    return bits.join(" ");
  }

  function readEnv(name, def) {
    try {
      const env = Cc["@mozilla.org/process/environment;1"].getService(Ci.nsIEnvironment);
      return env.exists(name) ? env.get(name) : def;
    } catch (e) { return def; }
  }

  function navigate(browser, url) {
    let principal = null;
    try {
      principal = Cc["@mozilla.org/scriptsecuritymanager;1"].getService(Ci.nsIScriptSecurityManager)
                    .getSystemPrincipal();
    } catch (e) {}
    const attempts = [
      () => browser.loadURI(url, { triggeringPrincipal: principal }),
      () => browser.webNavigation.loadURI(url, { triggeringPrincipal: principal, loadFlags: 0 }),
      () => browser.setAttribute("src", url)
    ];
    for (let i = 0; i < attempts.length; i++) {
      try { attempts[i](); log("navigating via form " + (i + 1) + " to " + url); return true; }
      catch (e) { log("form " + (i + 1) + " failed: " + e); }
    }
    return false;
  }

  // The bridge was written for Firefox's tabbed browser window (gBrowser). This gives it the same
  // surface on top of our single content area.
  function makeGBrowserShim(browser) {
    // DevTools' TargetFactory.forTab() expects a real tab: it reads tab.linkedBrowser (plus its owner window/document).
    const tab = {
      get linkedBrowser() { return browser; },
      get ownerGlobal() { return window; },
      get ownerDocument() { return document; },
      get selected() { return true; },
      get label() { try { return browser.contentTitle; } catch (e) { return ""; } },
      get pinned() { return false; },
      getAttribute() { return null; },
      hasAttribute() { return false; },
      addEventListener() {}, removeEventListener() {}
    };
    return {
      get tabs() { return [tab]; },
      get browsers() { return [browser]; },
      get visibleTabs() { return [tab]; },
      get ownerGlobal() { return window; },
      get ownerDocument() { return document; },
      getBrowserForTab() { return browser; },
      getTabForBrowser() { return tab; },
      get currentURI() { return browser.currentURI; },
      get selectedBrowser() { return browser; },
      get selectedTab() { return tab; },
      get canGoBack() { return browser.canGoBack; },
      get canGoForward() { return browser.canGoForward; },
      goBack() { browser.goBack(); },
      goForward() { browser.goForward(); },
      stop() { browser.stop(); },
      reload() { browser.reload(); },
      loadURI(url, params) { browser.loadURI(url, params); },
      addProgressListener(l) {   // only page-level load state, location and status text: not every request, progress tick and security change
        const W = Ci.nsIWebProgress; browser.webProgress.addProgressListener(l, W.NOTIFY_STATE_NETWORK | W.NOTIFY_LOCATION | W.NOTIFY_STATUS);
      },
      tabContainer: {
        addEventListener(type, fn) {
          if (type === "TabAttrModified")
            browser.addEventListener("DOMTitleChanged", function () { fn({ target: tab }); }, true);
        }
      }
    };
  }

  function start() {
    log("window loaded");
    const browser = document.getElementById("content");
    log("browser element: " + describe(browser));

    try {
      const listener = {
        QueryInterface: ChromeUtils.generateQI([Ci.nsIWebProgressListener, Ci.nsISupportsWeakReference]),
        onStateChange(wp, req, flags, status) {
          const L = Ci.nsIWebProgressListener;
          if ((flags & L.STATE_IS_NETWORK) && (flags & L.STATE_STOP)) log("page finished loading");
          else if ((flags & L.STATE_IS_NETWORK) && (flags & L.STATE_START)) log("page started loading");
        },
        onLocationChange(wp, req, loc) { if (loc) log("location: " + loc.spec); },
        onProgressChange() {}, onStatusChange() {}, onSecurityChange() {}, onContentBlockingEvent() {}
      };
      browser.webProgress.addProgressListener(listener, Ci.nsIWebProgress.NOTIFY_STATE_NETWORK | Ci.nsIWebProgress.NOTIFY_LOCATION);
      log("progress listener attached");
    } catch (e) { log("progress listener failed: " + e); }

    browser.addEventListener("DOMTitleChanged", function () {
      let t = "";
      try { t = browser.contentTitle; } catch (e) {}
      document.title = t ? t + " - GeckoIE bare" : "GeckoIE bare engine";
      log("title: " + t);
    }, true);

    window.addEventListener("unload", function () { log("window closing"); }, false);

    // Links that want a new window load in this content area instead.
    window.browserDOMWindow = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIBrowserDOMWindow]),
      // window.open(): giving the page this very window made scripts overwrite the page the user is on.
      // An address becomes a new Internet Explorer window; the page gets null, exactly like a blocked pop-up.
      createContentWindow(uri) {
        log("window.open for " + (uri && uri.spec));
        try { if (uri && /^https?:/i.test(uri.spec)) ChromeUtils.import("chrome://geckoie/content/bridge.jsm").IEBridge.openInNewWindow(window, uri.spec); } catch (e) { log("new IE window failed: " + e); }
        return null;
      },
      openURI(uri, opener, where) {
        log("new window for " + (uri && uri.spec));
        if (!uri) return null;
        if (where === 1 || !/^https?:/i.test(uri.spec)) { navigate(browser, uri.spec); return browser.contentWindow; }
        let sent = false;
        try { sent = ChromeUtils.import("chrome://geckoie/content/bridge.jsm").IEBridge.openInNewWindow(window, uri.spec); } catch (e) { log("new IE window failed: " + e); }
        if (!sent) { navigate(browser, uri.spec); return browser.contentWindow; }
        return null;   // opened as a new Internet Explorer window
      },
      createContentWindowInFrame() { return browser; },
      openURIInFrame(uri) { if (uri) navigate(browser, uri.spec); return browser; },
      isTabContentWindow() { return false; },
      canClose() { return true; },
      get tabCount() { return 1; }
    };

    const bridgeMode = readEnv("GECKOIE_BRIDGE", "0") === "1";
    if (bridgeMode) {
      try {
        window.gBrowser = makeGBrowserShim(browser);
        const m = {};
        Cu.import("chrome://geckoie/content/bridge.jsm", m);
        m.IEBridge.addView(window);
        log("bridge attached to this window");
      } catch (e) { log("bridge failed to start: " + e); }
    }

    // Give the content area a moment to get its docshell, then load the page.
    setTimeout(function () {
      log("after 500 ms: " + describe(browser));
      const url = readEnv("GECKOIE_URL", "");
      if (url) navigate(browser, url);
      else if (!bridgeMode) navigate(browser, "https://www.google.com/");
      else log("waiting for commands from the bridge");
    }, 500);
  }

  window.addEventListener("load", start, false);
})();
