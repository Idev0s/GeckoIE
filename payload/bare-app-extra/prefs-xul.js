// The one window this app opens: a plain content area with no browser interface.
pref("toolkit.defaultChromeURI", "chrome://geckoie/content/engine.xul");
pref("browser.dom.window.dump.enabled", true);
// Single process, no GPU/compositor guessing games on old hardware.
pref("browser.tabs.remote.autostart", false);
pref("extensions.e10sBlocksEnabling", false);
pref("layers.acceleration.disabled", true);
pref("app.update.enabled", false);
pref("toolkit.telemetry.enabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);

// Downloads are handed to the host.
pref("browser.download.useDownloadDir", false);
pref("browser.helperApps.alwaysAsk.force", true);
