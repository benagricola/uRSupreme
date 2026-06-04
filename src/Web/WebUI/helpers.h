// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static bool require_physical_auth(AsyncWebServerRequest* req, const JsonVariant& body) {
      if (req->hasHeader("Authorization")) {
        String h = req->header("Authorization");
        if (h.startsWith("Bearer ")) {
          String hex_str = h.substring(7); hex_str.trim();
          LXMF::IdentityId acc = AuthTokens::validate(std::string(hex_str.c_str()));
          if (!acc.empty()) return true;
        }
      }
      const char* code = body["identity_code"] | "";
      const char* code_err = explain_identity_code_failure(code);
      if (code_err == nullptr) return true;
      send_error_with_message(req, 401, "identity_code_required", code_err);
      return false;
    }

