// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_spa(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      AsyncWebServerResponse* resp = req->beginResponse_P(
          200, "text/html",
          Web::SPA_HTML_GZ, Web::SPA_HTML_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      // no-store, not no-cache: no-cache lets the browser keep a copy and
      // revalidate, but our handler doesn't implement 304, so stale SPAs
      // get served. no-store guarantees a fresh fetch every load.
      resp->addHeader("Cache-Control", "no-store");
      req->send(resp);
    }

    static void handle_alpine_js(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      AsyncWebServerResponse* resp = req->beginResponse_P(
          200, "application/javascript",
          Web::SPA_ALPINE_JS_GZ, Web::SPA_ALPINE_JS_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      // Alpine doesn't change between SPA versions, so let the browser
      // cache it indefinitely — largest asset by far.
      resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
      req->send(resp);
    }

    static void handle_styles_css(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      AsyncWebServerResponse* resp = req->beginResponse_P(
          200, "text/css",
          Web::SPA_STYLES_CSS_GZ, Web::SPA_STYLES_CSS_GZ_LEN);
      resp->addHeader("Content-Encoding", "gzip");
      // The SPA HTML embeds the CSS version as a `?v=<hash>` query
      // string, so each firmware ship gives this URL a new identity
      // and the immutable cache header is safe.
      resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
      req->send(resp);
    }

