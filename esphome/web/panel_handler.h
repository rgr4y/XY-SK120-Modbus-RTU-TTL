#pragma once
#include <cstdio>
#include <string>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "panel_html_gz.h"

class PanelHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override {
    // Serve /panel AND /panel/* (config, entities, logs) so History pushState
    // deep-links survive a refresh — the SPA reads location.pathname to restore
    // the right view. url_to() replaces the deprecated url() (removed 2026.9.0).
    if (request->method() != HTTP_GET) return false;
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    std::string u(request->url_to(url_buf));
    return u == "/panel" || u.compare(0, 7, "/panel/") == 0;
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    if (request->url_to(url_buf) == "/panel/build.json") {
      char json[256];
      snprintf(json, sizeof(json),
               "{\"config_hash\":\"%08lx\",\"web_hash\":\"%s\","
               "\"build_time\":%lld,\"boot_id\":\"%08lx%08lx\"}",
               static_cast<unsigned long>(esphome::App.get_config_hash()), PANEL_BUILD_HASH,
               static_cast<long long>(esphome::App.get_build_time()),
               static_cast<unsigned long>(boot_id_high_), static_cast<unsigned long>(boot_id_low_));
      auto *response = request->beginResponse(200, "application/json", json);
      response->addHeader("Cache-Control", "no-store");
      request->send(response);
      return;
    }
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", PANEL_HTML_GZ, sizeof(PANEL_HTML_GZ));
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  }

 private:
  const uint32_t boot_id_high_{esphome::random_uint32()};
  const uint32_t boot_id_low_{esphome::random_uint32()};
};

class ESPHomeHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET) return false;
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    std::string u(request->url_to(url_buf));
    return u == "/esphome" || u == "/esphome/" || u == "/favicon.ico" ||
           (u == "/" && !request->hasParam("native"));
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    auto url = request->url_to(url_buf);
    if (url == "/favicon.ico") {
      request->send(request->beginResponse(204, "image/x-icon", ""));
      return;
    }
    if (url == "/") {
      request->redirect("/panel");
      return;
    }
    // The native app uses its pathname as the REST/SSE base; keep it at root.
    static const char INDEX[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESPHome</title><style>html,body{margin:0;height:100%}"
        "iframe{display:block;width:100%;height:100%;border:0}</style></head>"
        "<body><iframe title=\"Native ESPHome\" src=\"/?native=1\"></iframe></body></html>";
    request->send(request->beginResponse(200, "text/html", INDEX));
  }
};

// NOTE: PANEL_HTML_GZ is defined in the AUTO-GENERATED "panel_html_gz.h"
// (included above), produced from packages/web/panel.html by
// scripts/gen_panel.py. The Makefile runs it before every build (`make gen`,
// and as a prereq of compile/run/upload), so the embedded asset always matches
// panel.html. Build through `make`, not bare `esphome`, or the UI may be stale.
// This handler only depends on the array name (via sizeof()) — regen is a
// drop-in; no changes needed here.

// Registration helper for packages/web.yaml's on_boot lambda:
//   esphome::web_server_base::global_web_server_base->add_handler(new PanelHandler());
// Use add_handler(), not add_handler_without_auth(), for both handlers.
