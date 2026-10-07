# XY-SK120 Web UI Port — Design Spec

**Date:** 2026-10-07
**Branch:** `feat/esphome-xy-sk120`
**Status:** Design only — no implementation
**Goal:** Replace the stock ESPHome `web_server` page on the XY-SK120 with a self-hosted custom panel, reusing D3806's embed-and-serve pattern. Re-skin for the SK120 entity set and add charger-specific controls (charge stage, status, sim-cycle, STOP).

---

## 1. Source mechanism (D3806) — verbatim facts

D3806 does **not** use `web_server:` `css_url`/`js_url` or a `local_file`. It embeds a gzip'd single-page app as a C array and registers a C++ `AsyncWebHandler` that intercepts the routes before the native web server's root handler.

### 1.1 Serving chain

- **`web_server: local: true`** embeds the native ESPHome assets (so `/esphome` works offline). D3806 keeps the native `web_server` running at `version: 3`; the custom handler sits *in front of* it. See `/Users/rob/workspace/D3806/packages/web.yaml:5-6` and base config in `chicken-base.yaml` (auth/port/version/include_internal owned there, per the comment at `web.yaml:1-4`).
- **Asset bake:** `panel.html` → gzip → C `uint8_t[]` header by `/Users/rob/workspace/D3806/scripts/gen_panel.py:19-37`. The script also injects a 12-char SHA-256 fingerprint over `panel.html + panel_handler.h`, replacing the literal `__PANEL_BUILD_HASH__` in the HTML (`gen_panel.py:21-22`).
- **C++ handler:** `/Users/rob/workspace/D3806/packages/web/panel_handler.h`
  - `PanelHandler` serves `/panel` and `/panel/*` (SPA deep links) as `text/html` + `Content-Encoding: gzip`, `Cache-Control: no-store` (`panel_handler.h:9-39`).
  - `/panel/build.json` returns `{config_hash, web_hash, build_time, boot_id}` from `App.get_config_hash()` / `App.get_build_time()` + the embedded `PANEL_BUILD_HASH` + a random per-boot id (`panel_handler.h:22-33,42-43`).
  - `ESPHomeHandler` redirects `/` → `/panel`, serves a 204 favicon, and frames the native page at `/?native=1` so the native REST/SSE base stays rooted at `/` (`panel_handler.h:46-74`).
- **Registration:** `esphome: includes:` lists **both** `panel_handler.h` and `panel_html_gz.h` (the second is `#include`d by the first; omitting it fails the build — `web.yaml:8-14`). An `on_boot: priority: 300` lambda calls `global_web_server_base->add_handler(new PanelHandler())` and `...(new ESPHomeHandler())` — `add_handler()` (keeps auth), not `add_handler_without_auth()` (`web.yaml:15-20`, `panel_handler.h:86-87`).
- **Build order:** the Makefile regenerates the header before every `esphome` invocation; incremental on `panel.html`/`panel_handler.h`/`gen_panel.py` mtime (`/Users/rob/workspace/D3806/Makefile:9-29`). Building with bare `esphome` risks a stale UI.

### 1.2 How the page talks to entities

Pure **ESPHome `web_server` v3 REST + SSE** — no native API, no websockets, no custom endpoints beyond `/panel/build.json`.

- **State in — SSE:** `new EventSource('/events')` (`panel.html:840`). On connect web_server pushes each entity once as `state_detail_all` (carries domain/name/entity_category), then live `state` events; the panel listens to both (`panel.html:860-879`). URLs/matching use the entity **NAME**, not `object_id` — web_server v3 (`panel.html:404-487`, `resolveKey` at `472-481`).
- **Priority snapshot:** before opening the stream it `fetch()`es a few entities via `GET /<domain>/<EncodedName>?detail=all` with a 2 s timeout (`panel.html:896-929`).
- **Commands out — REST POST:**
  - numbers: `POST /<domain>/<EncodedName>/set?value=X`, debounced ~350 ms (`panel.html:489-519`).
  - switches: `POST /.../turn_on` | `/turn_off`, then poll `?detail=all` up to 10×100 ms to confirm (`panel.html:1074-1096`).
  - buttons: `POST /.../press` (same `post()` helper, `panel.html:489-493,727`).
- **Build/version footer + hot-reload:** polls `/panel/build.json`; a changed `web_hash`/`config_hash`/`boot_id`/`build_time` triggers `location.reload()` (`panel.html:797-834`).
- **Lifecycle:** telemetry runs only while the tab is visible+focused; SSE closed on blur, priority snapshot reloaded on refocus (README §Web pages).

### 1.3 UI structure / aesthetic

Single file, **inline `<style>` + inline vanilla JS, no framework, no external fetch** (gzip'd, served from flash). Dark theme, monospace, token palette on `:root` (`panel.html:8`: `--bg:#0d1117`, metric colors `--v` green / `--a` amber / `--w` blue / `--hz` red). Layout is a centered `max-width:560px` column (`panel.html:13`), mobile-first (`@media (max-width:600px)` fullscreen modals, `panel.html:44-46`).

Three views toggled client-side (`panel.html:192-386`):
- **FACE** — skeuomorphic "PSU face" card: SET row, giant 74px V / A / W digits each with an inline SVG sparkline (ring buffer off the SSE stream, hover-to-inspect, no lib — `panel.html:527-606`), a status bar (Vin, temps, CC/CV + stage badges), a fault card + thermal-lockout overlay, then controls: presets/profile row, V & A steppers (±1/±.1/±.01), OUTPUT button, mode button.
- **CONFIG** — dozens of runtime `cfg_*`/`cal_*` number fields as ± steppers + toggle pills (charge profile, source gate, safety limits, thermal, calibration). **Largely N/A for SK120** (see §4).
- **DIAGNOSTICS/LOGS** — auto-renders every entity from the stream + a live log console fed by `/events` `log` lines.

---

## 2. Target entity inventory (XY-SK120)

From `/Users/rob/workspace/dps150/XY-SK120-Modbus-RTU-TTL/esphome/xy-sk120.yaml`. Note `web_server: include_internal: false` (`xy-sk120.yaml:84-85`) hides `internal: true` plumbing (`sk_onoff`, `vset_dev`, `sk_r06`–`sk_r10`) — they are **not** on the stream and must not be referenced by the panel.

**web_server URL uses the `name:` string** (v3). Column "URL name" is what the panel's NAME map keys on.

### Readouts — sensors
| name: | id: | URL name | unit | group | line |
|---|---|---|---|---|---|
| Output Voltage | `sk_vout` | `Output Voltage` | V | primary readout | `241-252` |
| Output Current | `sk_iout` | `Output Current` | A | primary readout | `254-264` |
| Output Power | `sk_power` | `Output Power` | W | primary readout | `266-276` |
| Input Voltage | `sk_vin` | `Input Voltage` | V | source panel | `279-289` |
| Internal Temp | `sk_temp_int` | `Internal Temp` | °C | status bar | `293-303` |
| External NTC Temp | `sk_temp_ext` | `External NTC Temp` | °C | status bar (pack temp) | `307-317` |
| Set Wattage | `set_wattage` | `Set Wattage` | W | setpoint echo | `356-363` |
| Device V_SET | `device_vset` | `Device V_SET` | V | setpoint echo | `367-387` |
| Model ID | `sk_model` | `Model ID` | — | diag only | `335-343` |
| Firmware Version | `sk_version` | `Firmware Version` | — | diag only | `345-353` |

### Readouts — binary_sensor
| name: | id: | URL name | meaning | line |
|---|---|---|---|---|
| CV Mode | `sk_cv` | `CV Mode` | 1=CV, 0=CC | `391-397` |
| Output State | *(none)* | `Output State` | REG_ONOFF read mirror | `399-404` |

### Readouts — text_sensor
| name: | id: | URL name | meaning | line |
|---|---|---|---|---|
| Charge Stage | `charge_stage_text` | `Charge Stage` | IDLE/CC/CV/FLOAT/DONE/FAULT | `546-559` |
| Charge Status | `charge_status_text` | `Charge Status` | free-text fault/status | `560-564` |

### Controls — number (both `mode: box`)
| name: | id: | URL name | range | line |
|---|---|---|---|---|
| Set Voltage | `v_target` | `Set Voltage` | 0–`${vset_max}` (36.5), step .01 | `486-511` |
| Set Current | `set_current` | `Set Current` | 0–`${iset_max}` (6.0), step .001 | `528-541` |

### Controls — switch (template)
| name: | id: | URL name | action | line |
|---|---|---|---|---|
| Output | `output_sw` | `Output` | ON = soft-start ramp; OFF = kill output + abort charge | `434-443` |
| Charge | `charge_sw` | `Charge` | ON = start CC/CV/FLOAT engine | `446-455` |
| Sim Cycle | `sim_sw` | `Sim Cycle` | dry-run engine on synthetic inputs | `459-480` |

### Controls — button
| name: | id: | URL name | action | line |
|---|---|---|---|---|
| STOP | `estop` | `STOP` | kills charge+sim+ramp+output | `412-420` |
| Device Restart | `switch_restart` | `Device Restart` | reboot (from `common.yaml:61-65`) | base |

**User-facing entity count: 20** in `xy-sk120.yaml` (10 sensor, 2 binary_sensor, 2 text_sensor, 2 number, 3 switch, 1 button) + base diagnostics (Device Restart, Uptime, WiFi Signal, ESPHome Version, IP Address, Connected SSID from `common.yaml`).

---

## 3. Entity → widget mapping

**FACE view (single unified charger/PSU — no mode toggle, see §4):**

- **Primary digits panel:** `sk_vout` → big V, `sk_iout` → big A, `sk_power` → big W. Keep D3806's 3-line digit block + per-line sparklines verbatim (freeze on output-off as D3806 does, `panel.html:569-575`). Sparkline "outputOn" gate reads the `Output State` binary_sensor.
- **SET echo row:** `Set Voltage` + `Set Current` as the commanded setpoints; `Set Wattage` and `Device V_SET` as secondary readbacks (Device V_SET surfaces front-panel edits + the device's OVP clamp).
- **Status bar:** `Input Voltage` (U-IN), `Internal Temp` (amb), `External NTC Temp` (pack), `CV Mode` → CV/CC badge, `Output State` → output indicator.
- **Charge-stage indicator:** `Charge Stage` text → colored stage badge (IDLE grey / CC amber / CV green / FLOAT blue / DONE green / FAULT red). Reuse D3806's `.stage-badge` + `.fault` card styling; FAULT stage drives the fault card, `Charge Status` text fills the message.
- **Setpoint inputs:** `Set Voltage` + `Set Current` as **free-input numeric boxes**, matching the YAML's `mode: box` (`xy-sk120.yaml:490,536`). This diverges from D3806's ±steppers — keep a text `<input>` (D3806 already supports typed entry in `.stepper .val input`, `panel.html:91-92). Optional: retain small ± nudge buttons around the box.
- **Charger controls block (new, charger-specific):**
  - `Output` toggle — big ON/OFF (reuse `.onoff`, with D3806's confirm-poll pattern since output is soft-start-deferred).
  - `Charge` toggle — start/stop the CC/CV/FLOAT engine.
  - `Sim Cycle` toggle — dry-run; show a "SIM" banner when active (reuse `.charge-msg.warn`).
  - **`STOP`** — prominent full-width red button, always visible, above the fold. New vs D3806 (which had no master e-stop). `POST /button/STOP/press`.

**DIAGNOSTICS/LOGS view:** reuse verbatim — it auto-renders whatever is on the stream, so it needs zero per-entity wiring. Device Restart reboot button in diag or a small control.

**CONFIG view:** **drop it** (see §4) — or reduce to a read-only profile summary.

---

## 4. Charger-specific UI beyond D3806's PSU UI

1. **No runtime config.** The SK120 charge profile (absorb/float/taper/temp-comp/source gate) is **compile-time `substitutions`** (`xy-sk120.yaml:36-61`), not `number`/`switch` entities. D3806's entire CONFIG view (`panel.html:296-366`) maps to entities that **do not exist** here. → **Remove the CONFIG view and all `cfg_*`/`cal_*` wiring.** Optionally render a static read-only "Profile" card (absorb V, float V, charge A) computed client-side from nothing — or better, surface the live `Set Wattage`/`Device V_SET` readbacks instead. Decision needed (§7).
2. **Charge stage machine.** D3806 had a charge stage too, but SK120 adds explicit DONE/FAULT terminal states and a `Charge Status` free-text reason string — give both a dedicated badge + message line.
3. **Sim-cycle control.** Brand new. A toggle that walks IDLE→CC→CV→FLOAT→DONE on synthetic inputs in ~20 s, output never fires. UI must clearly distinguish SIM from a real run (persistent banner; the engine refuses sim while real output/charge is live — `xy-sk120.yaml:464-469`).
4. **Source / guard state.** Surface `Input Voltage` prominently (the PD-trigger source that can fold under load) and reflect guard trips (source out of range / depleted / hot abort / overvoltage) via the `Charge Status` text into the fault card.
5. **No PSU/Charger mode toggle.** D3806's `bench_psu_mode` switch (`panel.html:421`) has no SK120 equivalent — the SK120 is always a charger with a manual Output override. Single unified FACE, mode button removed.
6. **Sim seed inputs are globals, not entities** (`sim_v/sim_i/sim_t/sim_vin`, `xy-sk120.yaml:167-171`) — **not web-settable** as-is. If web-driven guard testing is wanted, they must be promoted to `number` entities (§7).

---

## 5. Data flow, cadence, responsive

- **Transport:** identical to D3806 — SSE `/events` for all state, REST POST for commands, `/panel/build.json` for build identity. No new transport code.
- **Refresh cadence:** device side — telemetry polls at `${poll_ms}` = 500 ms (`xy-sk120.yaml:22,139`); text/template sensors update 1 s (`charge_stage_text`/`charge_status_text`/`set_wattage`). SSE pushes on change. Panel sparkline buffer `SPARK_MAX=120` (`panel.html:534`).
- **Command debounce:** keep D3806's ~350 ms number debounce; box inputs commit on blur/Enter + debounced keystroke.
- **Output confirm:** reuse the poll-until-confirmed pattern (`panel.html:1070-1096`) — important because `output_sw` is soft-start-deferred (ON returns before the ramp completes).
- **Responsive/mobile:** reuse the 560 px column + `@media (max-width:600px)` rules verbatim.

---

## 6. Reuse vs new

**Reuse verbatim (copy, path-adjust only):**
- `panel_handler.h` — `PanelHandler` + `ESPHomeHandler` + `/panel/build.json`. Zero logic change (depends only on `PANEL_HTML_GZ` array name).
- `gen_panel.py` — path constants only.
- Makefile/`gen` build step pattern.
- `web.yaml` serving block: `web_server: local: true`, `esphome: includes:` + `on_boot: priority:300` handler registration.
- panel.html shell: `:root` palette, FACE card, digit block, sparklines, SSE connect/reconnect/watchdog, build-hash hot-reload, DIAG + LOG views, output confirm-poll, visibility lifecycle.

**New / changed:**
- panel.html `NAME` map → the 20 SK120 names from §2 (replaces D3806's map at `panel.html:407-460`).
- FACE markup: drop PSU preset row + mode button + chemistry picker; add Charge toggle, Sim Cycle toggle, STOP button, stage/status badges.
- **Delete** CONFIG view + all `cfg_*`/`cal_*`/`led_brightness`/`display_test`/`buck_ntc_override` wiring.
- Remove `bench_psu_mode` logic, thermal-lockout `binary_sensor` dependency (D3806-specific — SK120 has none).
- Setpoint widget: box input instead of stepper (match `mode: box`).

---

## 7. Files to add / change + build steps

**Add (under `esphome/`, since `includes:` paths resolve relative to the yaml):**
- `esphome/web/panel.html` — ported SPA (new content, D3806 shell).
- `esphome/web/panel_handler.h` — copy of D3806's, unchanged.
- `esphome/web/panel_html_gz.h` — **generated, git-ignored** (add to `esphome/.gitignore`).
- `esphome/web/gen_panel.py` — copy, repoint `SRC`/`OUT` to `esphome/web/`.
- `esphome/web.yaml` — new package: `web_server: local: true` + `include_internal: false` (already in `xy-sk120.yaml:84-85`; consolidate) + `esphome: includes:` + `on_boot` handler registration. **Note:** `xy-sk120.yaml` already has an `esphome: on_boot` (`:91-107`) and `web_server:` block — the package merge must be additive (ESPHome merges lists), verify no key collision on `on_boot` priority.
- Build glue: either a `Makefile` in `esphome/` or an `npm`/shell `gen` step. Simplest: a `Makefile` mirroring D3806's `gen` prerequisite so `make run` regenerates the header first.

**Change:**
- `esphome/xy-sk120.yaml` — add `packages: web: !include web.yaml`; move the `web_server: include_internal:false` into `web.yaml` or leave and let merge; confirm the existing `on_boot` coexists with the handler-registration `on_boot`.
- `esphome/.gitignore` — ignore `web/panel_html_gz.h`.

**Build steps:**
1. `python3 esphome/web/gen_panel.py` (bakes + fingerprints the header).
2. `esphome run esphome/xy-sk120.yaml` (or `make run` wrapping step 1).
3. Flash with `--no-logs` + `timeout` per the ESPHome no-logs-flash memory rule.
4. Verify: `/` redirects to `/panel`; `/esphome` native page loads; SSE populates digits; Set V/I POST and read back; Charge/Sim/STOP act; `/panel/build.json` footer renders.

---

## 8. Decisions (resolved 2026-10-07)

1. **CONFIG view:** **DELETE** entirely — all config is compile-time YAML `substitutions`; no runtime-config entities to bind. No profile-summary card.
2. **Sim seed inputs:** **Promote `sim_t` + `sim_vin` to `number` entities** (web-driven guard testing: cold inhibit / hot abort / source depletion). Leave `sim_v` / `sim_i` as globals (pack-model-driven, not user inputs).
3. **Set V/I widget:** plain **box** (matches existing `mode: box`).
4. **Build glue:** standalone **`esphome/Makefile`** (parent-repo `npm`/`build-css.sh` are for the old Arduino webUI — unrelated).
5. **`on_boot` merge:** keep the existing boot-policy `on_boot` and add the handler-registration `on_boot` as a **second list entry**, priorities spaced **-100 / -99** (both run late, after `web_server` base is up; boot policy first). ESPHome merges the list.
6. **Auth:** **off** (as-is, `common.yaml:56-58` commented). LAN-only.
7. **STOP placement:** **full-width, above the readouts** (max visibility / safety).
