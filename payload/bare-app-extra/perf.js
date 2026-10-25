// GeckoIE performance tuning for old computers (installer component "Performance tuning").
pref("browser.cache.disk.enable", false);            // slow hard disks: keep the cache in memory
pref("browser.cache.memory.enable", true);
pref("browser.cache.memory.capacity", 49152);
pref("browser.sessionhistory.max_total_viewers", 1); // fewer pages kept in memory for Back/Forward
pref("dom.max_script_run_time", 0);                  // no "unresponsive script" prompts
pref("dom.max_chrome_script_run_time", 0);
pref("dom.serviceWorkers.enabled", false);
pref("dom.push.enabled", false);
pref("dom.webnotifications.enabled", false);
pref("media.peerconnection.enabled", false);
pref("webgl.disabled", true);
pref("gfx.direct2d.disabled", true);
pref("layers.acceleration.disabled", true);
pref("media.hardware-video-decoding.enabled", false);
pref("network.prefetch-next", false);
pref("network.dns.disablePrefetch", true);
pref("network.http.speculative-parallel-limit", 0);
pref("browser.send_pings", false);
pref("toolkit.telemetry.enabled", false);
pref("datareporting.healthreport.uploadEnabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("app.update.enabled", false);
pref("extensions.update.enabled", false);
pref("extensions.blocklist.enabled", false);
pref("ui.prefersReducedMotion", 1);
pref("toolkit.cosmeticAnimations.enabled", false);
