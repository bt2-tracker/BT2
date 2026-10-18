/*! coi-serviceworker v0.1.7 - Guido Zuidhof and contributors, licensed under MIT */
/* Bernttracker II changes:
 *  - The app's own files (program, page, icons) are kept in a cache on the device, so the app
 *    starts without internet. Which version is used is decided by the page ("bt2-meta" cache),
 *    and a new version is only installed when the user says so (see index.html).
 *  - This file NEVER touches the user's own files (songs, samples, settings). Those live in
 *    IndexedDB, not in these caches.
 *  - The page is not reloaded by itself while the program is in use.
 */
let coepCredentialless = false;
if (typeof window === 'undefined') {
    const META_CACHE = 'bt2-meta';
    const META_KEY = '__bt2/current-app';
    let currentAppCache = null; // name of the cache with the installed app version
    let currentChecked = false;

    const readCurrent = async () => {
        try {
            const meta = await caches.open(META_CACHE);
            const r = await meta.match(META_KEY);
            currentAppCache = r ? (await r.json()).cache : null;
        } catch (e) {
            currentAppCache = null;
        }
        currentChecked = true;
    };

    self.addEventListener("install", () => self.skipWaiting());
    self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

    self.addEventListener("message", (ev) => {
        if (!ev.data) {
            return;
        } else if (ev.data.type === "deregister") {
            self.registration
                .unregister()
                .then(() => {
                    return self.clients.matchAll();
                })
                .then(clients => {
                    clients.forEach((client) => client.navigate(client.url));
                });
        } else if (ev.data.type === "coepCredentialless") {
            coepCredentialless = ev.data.value;
        } else if (ev.data.type === "bt2-app-changed") {
            currentChecked = false; // the page installed another app version
        }
    });

    const withHeaders = (response) => {
        if (response.status === 0) {
            return response;
        }

        const newHeaders = new Headers(response.headers);
        newHeaders.set("Cross-Origin-Embedder-Policy",
            coepCredentialless ? "credentialless" : "require-corp"
        );
        if (!coepCredentialless) {
            newHeaders.set("Cross-Origin-Resource-Policy", "cross-origin");
        }
        newHeaders.set("Cross-Origin-Opener-Policy", "same-origin");

        return new Response(response.body, {
            status: response.status,
            statusText: response.statusText,
            headers: newHeaders,
        });
    };

    // the app's own files, from the installed version on the device
    const fromAppCache = async (request) => {
        if (request.method !== 'GET') return null;
        const url = new URL(request.url);
        const scope = new URL(self.registration.scope);
        if (url.origin !== scope.origin || !url.pathname.startsWith(scope.pathname)) return null;

        const rel = url.pathname.slice(scope.pathname.length);
        if (rel === 'version.json' || rel.startsWith('demo/')) return null; // always from the network
        if (url.searchParams.has('v') || url.searchParams.has('t')) return null; // the page downloading a version

        if (!currentChecked) await readCurrent();
        if (!currentAppCache) return null;

        const cache = await caches.open(currentAppCache);
        const key = (rel === '' || rel.endsWith('/')) ? scope.pathname + 'index.html' : url.pathname;
        return (await cache.match(key)) || null;
    };

    self.addEventListener("fetch", function (event) {
        const r = event.request;
        if (r.cache === "only-if-cached" && r.mode !== "same-origin") {
            return;
        }

        const request = (coepCredentialless && r.mode === "no-cors")
            ? new Request(r, {
                credentials: "omit",
            })
            : r;
        event.respondWith((async () => {
            try {
                const cached = await fromAppCache(r);
                if (cached) return withHeaders(cached);
            } catch (e) {
                console.warn('bt2 cache', e);
            }
            return fetch(request).then(withHeaders);
        })().catch((e) => console.error(e)));
    });

} else {
    (() => {
        const reloadedBySelf = window.sessionStorage.getItem("coiReloadedBySelf");
        window.sessionStorage.removeItem("coiReloadedBySelf");
        const coepDegrading = (reloadedBySelf == "coepdegrade");

        // You can customize the behavior of this script through a global `coi` variable.
        const coi = {
            shouldRegister: () => !reloadedBySelf,
            shouldDeregister: () => false,
            coepCredentialless: () => true,
            coepDegrade: () => true,
            doReload: () => window.location.reload(),
            quiet: false,
            ...window.coi
        };

        // BerntTracker: never reload the page by itself once the program is in use
        const mayReload = () => !(window.BT2 && window.BT2.started);

        const n = navigator;
        const controlling = n.serviceWorker && n.serviceWorker.controller;

        // Record the failure if the page is served by serviceWorker.
        if (controlling && !window.crossOriginIsolated) {
            window.sessionStorage.setItem("coiCoepHasFailed", "true");
        }
        const coepHasFailed = window.sessionStorage.getItem("coiCoepHasFailed");

        if (controlling) {
            // Reload only on the first failure.
            const reloadToDegrade = coi.coepDegrade() && !(
                coepDegrading || window.crossOriginIsolated
            );
            n.serviceWorker.controller.postMessage({
                type: "coepCredentialless",
                value: (reloadToDegrade || coepHasFailed && coi.coepDegrade())
                    ? false
                    : coi.coepCredentialless(),
            });
            if (reloadToDegrade) {
                !coi.quiet && console.log("Reloading page to degrade COEP.");
                window.sessionStorage.setItem("coiReloadedBySelf", "coepdegrade");
                coi.doReload("coepdegrade");
            }

            if (coi.shouldDeregister()) {
                n.serviceWorker.controller.postMessage({ type: "deregister" });
            }
        }

        if (!window.isSecureContext) {
            !coi.quiet && console.log("COOP/COEP Service Worker not registered, a secure context is required.");
            return;
        }

        // In some environments (e.g. Firefox private mode) this won't be available
        if (!n.serviceWorker) {
            !coi.quiet && console.error("COOP/COEP Service Worker not registered, perhaps due to private mode.");
            return;
        }

        // BerntTracker: always register (it also keeps the app on the device), but only reload the
        // page when that is needed to turn on isolation (the low-latency sound engine needs it)
        const needIsolation = window.crossOriginIsolated === false && coi.shouldRegister();

        n.serviceWorker.register(window.document.currentScript.src).then(
            (registration) => {
                if (!registration) return;
                !coi.quiet && console.log("COOP/COEP Service Worker registered", registration.scope);

                registration.addEventListener("updatefound", () => {
                    if (!needIsolation || !mayReload()) return;
                    !coi.quiet && console.log("Reloading page to make use of updated COOP/COEP Service Worker.");
                    window.sessionStorage.setItem("coiReloadedBySelf", "updatefound");
                    coi.doReload();
                });

                // If the registration is active, but it's not controlling the page
                if (needIsolation && registration.active && !n.serviceWorker.controller && mayReload()) {
                    !coi.quiet && console.log("Reloading page to make use of COOP/COEP Service Worker.");
                    window.sessionStorage.setItem("coiReloadedBySelf", "notcontrolling");
                    coi.doReload();
                }
            },
            (err) => {
                !coi.quiet && console.error("COOP/COEP Service Worker failed to register:", err);
            }
        );
    })();
}
