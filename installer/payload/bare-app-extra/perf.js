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

// ---- wave 2: less work per page on slow processors
pref("image.high_quality_downscaling.enabled", false);   // cheaper image scaling
pref("general.smoothScroll", false);                     // no scroll animation to repaint
pref("accessibility.force_disabled", 1);                 // no accessibility hooks into every window
pref("network.dns.disableIPv6", true);                   // no IPv6 look-ups on XP, which has no IPv6 by default
pref("network.predictor.enabled", false);
pref("network.predictor.enable-prefetch", false);
pref("network.http.rcwn.enabled", false);                // no racing of cache and network
pref("security.OCSP.timeoutMilliseconds.soft", 1500);    // a slow certificate check blocks a page for at most 1.5 s
pref("content.notify.interval", 250000);                 // fewer intermediate layouts while a page loads
pref("content.notify.ontimer", true);
pref("gfx.canvas.azure.accelerated", false);
pref("network.buffer.cache.size", 65536);                // bigger network reads, fewer wake-ups
pref("network.buffer.cache.count", 24);

// ---- wave 3: less disk, less memory, less CPU
pref("places.history.enabled", false);                   // no history database writes (Internet Explorer keeps the history)
pref("browser.formfill.enable", false);
pref("browser.cache.offline.enable", false);
pref("browser.cache.disk.smart_size.enabled", false);
pref("browser.sessionhistory.max_total_viewers", 0);     // no whole pages kept in memory for Back
pref("image.mem.surfacecache.max_size_kb", 65536);       // decoded images: at most 64 MB (the default can use a gigabyte)
pref("image.mem.surfacecache.min_expiration_ms", 30000);
pref("image.mem.max_decoded_image_kb", 51200);
pref("image.animation_mode", "once");                    // an animated picture plays once instead of eating CPU forever
pref("media.cache_size", 16384);
pref("network.dnsCacheEntries", 400);
pref("network.dnsCacheExpiration", 3600);
pref("layout.frame_rate", 30);                           // animations at 30 frames a second instead of 60
pref("javascript.options.spectre.index_masking", false);        // Spectre protections cost JavaScript speed on old processors
pref("javascript.options.spectre.value_masking", false);
pref("javascript.options.spectre.object_mitigations.barriers", false);
pref("javascript.options.spectre.object_mitigations.misc", false);
pref("javascript.options.spectre.string_mitigations", false);
pref("javascript.options.spectre.jit_to_C++_calls", false);
pref("dom.ipc.processPrelaunch.enabled", false);

// ---- wave 3b: do less while the engine starts (services of the Firefox browser that a page view never uses)
pref("toolkit.telemetry.unified", false);
pref("toolkit.telemetry.archive.enabled", false);
pref("toolkit.telemetry.newProfilePing.enabled", false);
pref("toolkit.telemetry.shutdownPingSender.enabled", false);
pref("toolkit.telemetry.updatePing.enabled", false);
pref("toolkit.telemetry.bhrPing.enabled", false);
pref("toolkit.telemetry.firstShutdownPing.enabled", false);
pref("browser.ping-centre.telemetry", false);
pref("identity.fxaccounts.enabled", false);
pref("network.captive-portal-service.enabled", false);
pref("network.connectivity-service.enabled", false);
pref("media.gmp-provider.enabled", false);
pref("media.gmp-manager.updateEnabled", false);
pref("media.gmp.trial-create.enabled", false);
pref("extensions.systemAddon.update.enabled", false);
pref("extensions.getAddons.cache.enabled", false);
pref("extensions.webservice.discoverURL", "");
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);
pref("browser.safebrowsing.downloads.enabled", false);
pref("security.pki.crlite_mode", 0);
