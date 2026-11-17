/*
 * Service worker for the Teletext web client.
 * Keeps a copy of the app itself (page, manifest, icons) so it opens
 * instantly and still starts when the server can't be reached.
 * Page data (/api/...) always comes live from the server - never cached.
 */
const VERSION = "ttx-v4";
const SHELL = ["./", "index.html", "manifest.json", "icon-192.png", "icon-512.png", "icon-maskable.png", "edit.html"];

self.addEventListener("install", event => {
  event.waitUntil(caches.open(VERSION).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", event => {
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== VERSION).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", event => {
  const url = new URL(event.request.url);
  if (event.request.method !== "GET" || url.origin !== location.origin) return;
  if (url.pathname.includes("/api/")) return;               // live data: straight to the server

  // the app itself: try the network first (so updates arrive), fall back to the copy
  event.respondWith(
    fetch(event.request)
      .then(resp => {
        if (resp.ok) {
          const copy = resp.clone();
          caches.open(VERSION).then(c => c.put(event.request, copy));
        }
        return resp;
      })
      .catch(() => caches.match(event.request).then(r => r || caches.match("index.html")))
  );
});
