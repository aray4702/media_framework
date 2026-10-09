// Cross-origin isolation for hosts that can't send headers (GitHub Pages). The pages need it for
// SharedArrayBuffer (the AudioWorklet's shared memory); tools/serve.py sends the headers itself,
// so there this does nothing. The same file runs twice:
// - in a page (a classic <script>, before mf.js): registers the copy of itself at the site's root
//   (the Pages workflow puts one there) as the service worker, then reloads once so the page comes
//   back through it. The root, so that its scope covers build-web/ too: the player's workers load
//   mf.js from there, and a worker outside the scope gets no headers and is refused;
// - as that service worker: passes every request on and adds COOP and COEP to the responses, and
//   CORP to same-origin ones, so the page and its workers are cross-origin isolated.

if (typeof window === 'undefined') {
  self.addEventListener('install', () => self.skipWaiting());
  self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
  self.addEventListener('fetch', (e) => {
    const r = e.request;
    if (r.cache === 'only-if-cached' && r.mode !== 'same-origin') return;  // fetch() would throw
    e.respondWith(fetch(r).then((response) => {
      if (response.status === 0) return response;  // opaque: its headers can't be read or set
      const headers = new Headers(response.headers);
      headers.set('Cross-Origin-Opener-Policy', 'same-origin');
      headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
      if (new URL(r.url).origin === self.location.origin) headers.set('Cross-Origin-Resource-Policy', 'same-origin');
      return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
    }));
  });
} else if (window.crossOriginIsolated) {
  sessionStorage.removeItem('coi-reloaded');
} else if (window.isSecureContext && 'serviceWorker' in navigator) {
  // One reload per try: if it didn't isolate the page (the worker refused, a private window), the
  // page runs without rather than reloading forever, and says why.
  if (sessionStorage.getItem('coi-reloaded')) {
    sessionStorage.removeItem('coi-reloaded');
    console.warn('coi.js: the page is still not cross-origin isolated; audio and threads will not work');
  } else {
    navigator.serviceWorker.register(new URL('../../coi.js', document.currentScript.src)).then((registration) => {
      const reload = () => { sessionStorage.setItem('coi-reloaded', '1'); location.reload(); };
      if (registration.active) reload();  // installed before (this load bypassed it, as a hard reload does)
      else navigator.serviceWorker.addEventListener('controllerchange', reload, { once: true });
    }, (e) => console.warn('coi.js: no service worker:', e));
  }
}
