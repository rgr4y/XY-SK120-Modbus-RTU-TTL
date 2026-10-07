# XY-SK120 Web UI Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stock ESPHome `web_server` page on the XY-SK120 with a self-hosted custom panel (D3806 embed-and-serve pattern), re-skinned for the SK120 entity set with charger-specific controls (charge stage, status, sim-cycle, STOP).

**Architecture:** Keep the native ESPHome `web_server` v3 running (`local: true`) and register a custom C++ `AsyncWebHandler` (`PanelHandler` + `ESPHomeHandler`) in front of it via `on_boot`. A single-file inline-CSS/JS SPA (`panel.html`) is gzip-baked into a C header by `gen_panel.py` and `#include`d. The page reads state over SSE `/events` and sends commands over REST POST — stock web_server endpoints only, no native API, no websockets.

**Tech Stack:** ESPHome 2026.8.2 (esp-idf), `web_server` v3, `AsyncWebHandler`, Python 3 (gzip bake), vanilla HTML/CSS/JS (no framework, no CDN), GNU Make.

**Spec:** `docs/2026-10-07-xy-sk120-webui-port-design.md` (decisions in §8 are final)

## Global Constraints

- All new web files live under `esphome/web/`; `esphome: includes:` paths resolve **relative to `esphome/xy-sk120.yaml`**, so include them as `web/panel_handler.h` and `web/panel_html_gz.h`.
- `esphome/web/panel_html_gz.h` is **generated, git-ignored** — never hand-edit, never commit.
- Flash ESPHome ONLY with `--no-logs` + `timeout`: `timeout 600 esphome run esphome/xy-sk120.yaml --device 10.0.0.11 --no-logs` (phantom-log-tail memory rule). Device IP: `10.0.0.11`.
- Always regenerate the gz header before building: `python3 esphome/web/gen_panel.py` (or `make -C esphome run`). Bare `esphome` risks a stale UI.
- The panel keys entities by their web_server v3 **`name:` string** (URL-encoded), NOT `object_id`. Exact names in §2 of the spec.
- `web_server: include_internal: false` is set (`xy-sk120.yaml:84-85`): internal entities (`sk_onoff`, `vset_dev`, `sk_r06`–`sk_r10`) are NOT on the stream — the panel must never reference them.
- Handlers register with `add_handler()` (auth-respecting), not `add_handler_without_auth()`. Auth is off (LAN-only) but keep the auth-respecting call.
- Two `esphome: on_boot:` entries after merge: existing boot-policy at **priority -100**, handler-registration at **priority 300** (RULING 4: -99 lost `/` to the native root handler — it registers first; 300 is D3806's proven value and the only one that passes the `/`→`/panel` gate).
- Do NOT restate `web_server:` auth/port/version in `web.yaml` — `common.yaml` owns those; `web.yaml` adds only `local: true` + includes + on_boot.

## Review Focus

- **Internal entity leak** — if the panel's NAME map or DIAG view references an `internal: true` entity (`sk_onoff`/`vset_dev`), it silently never updates (not on `/events`). Task 2 verifies every mapped name resolves against a live `/events` snapshot.
- **Output confirm race** — `output_sw` ON returns BEFORE the soft-start ramp finishes; a naive toggle that reads state immediately shows OFF and flaps. Task 3 keeps D3806's poll-until-confirmed pattern and tests a toggle reflects ON within the poll window.
- **STOP must always act** — STOP has to fire regardless of view/state (mid-sim, mid-ramp, disconnected SSE). Task 3 tests `POST /button/STOP/press` returns 2xx and the engine/output drop even with the SSE stream closed.
- **Stale UI after edit** — building with bare `esphome` serves the old gz header; the symptom is a UI that doesn't match `panel.html`. Every build task runs `gen_panel.py` first and Task 6 verifies the `/panel/build.json` `web_hash` changes when `panel.html` changes.
- **Name encoding** — names contain spaces (`Set Voltage`, `Output Voltage`); REST/SSE use URL-encoding. Task 2/3 test a spaced-name entity both reads (SSE) and writes (POST `/number/Set%20Voltage/set`).

---

### Task 1: Serving skeleton — handler, generator, build glue, stub panel

**Files:**
- Create: `esphome/web/panel_handler.h` (verbatim copy of D3806's)
- Create: `esphome/web/gen_panel.py` (copy, repointed paths)
- Create: `esphome/web/panel.html` (minimal stub for this task)
- Create: `esphome/web.yaml` (serving package)
- Create: `esphome/Makefile` (gen-before-build)
- Create/Modify: `esphome/.gitignore` (ignore `web/panel_html_gz.h`)
- Modify: `esphome/xy-sk120.yaml` (add `packages:`; fold `web_server:` into the package)

**Interfaces:**
- Produces: C header `web/panel_html_gz.h` defining `const uint8_t PANEL_HTML_GZ[]`, `const unsigned int PANEL_HTML_GZ_len`, `const char PANEL_BUILD_HASH[]`. Consumed by `panel_handler.h`.
- Produces: routes `/panel`, `/panel/*`, `/panel/build.json`, `/` → redirect `/panel`, `/esphome` native, `/?native=1` native-rooted.

- [ ] **Step 1: Copy the handler verbatim**

Copy `/Users/rob/workspace/D3806/packages/web/panel_handler.h` to `esphome/web/panel_handler.h` with NO changes. It depends only on the array name `PANEL_HTML_GZ` and `#include "panel_html_gz.h"` (same directory). Confirmed-correct content (87 lines) defines `PanelHandler` (serves `/panel`, `/panel/*`, `/panel/build.json` gzip) and `ESPHomeHandler` (`/`→`/panel`, `/favicon.ico` 204, `/esphome` + `/?native=1` native).

- [ ] **Step 2: Copy the generator, repoint paths**

Create `esphome/web/gen_panel.py` as a copy of D3806's, changing only the path constants so SRC/OUT sit in `esphome/web/`:

```python
#!/usr/bin/env python3
"""Regenerate esphome/web/panel_html_gz.h from esphome/web/panel.html.

Gzip-compresses the /panel SPA into a C uint8_t array (served Content-Encoding:
gzip). Embeds a 12-char SHA-256 fingerprint over panel.html + panel_handler.h,
replacing __PANEL_BUILD_HASH__ in the HTML. Run before every esphome build.
Never regenerate by hand.
"""
import gzip
import hashlib
import pathlib

HERE = pathlib.Path(__file__).resolve().parent          # esphome/web/
SRC = HERE / "panel.html"
OUT = HERE / "panel_html_gz.h"


def main() -> None:
    html = SRC.read_bytes()
    web_hash = hashlib.sha256(html + (HERE / "panel_handler.h").read_bytes()).hexdigest()[:12]
    html = html.replace(b"__PANEL_BUILD_HASH__", web_hash.encode("ascii"))
    gz = gzip.compress(html, compresslevel=9, mtime=0)  # mtime=0 -> reproducible
    lines = [
        "#pragma once",
        "#include <cstdint>",
        "// AUTO-GENERATED from panel.html by gen_panel.py -- do not edit.",
        f'const char PANEL_BUILD_HASH[] = "{web_hash}";',
        "const uint8_t PANEL_HTML_GZ[] = {",
    ]
    for i in range(0, len(gz), 12):
        row = ", ".join(f"0x{b:02x}" for b in gz[i:i + 12])
        lines.append(f"  {row},")
    lines.append("};")
    lines.append(f"const unsigned int PANEL_HTML_GZ_len = {len(gz)};")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"gen_panel: {SRC.name} {len(html)}B -> {OUT.name} {len(gz)}B gz")


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Write a minimal stub panel.html**

Create `esphome/web/panel.html` — just enough to prove serving + build-hash wiring. Replaced wholesale in Task 2.

```html
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XY-SK120</title></head>
<body style="font-family:monospace;background:#0d1117;color:#e6edf3">
<h1>XY-SK120 panel</h1>
<p>build <code>__PANEL_BUILD_HASH__</code></p>
<a href="/esphome">native</a>
</body></html>
```

- [ ] **Step 4: Create the serving package `esphome/web.yaml`**

No UI-support entities here (unlike D3806 — SK120's `Charge Stage`/`Charge Status`/sensors already live in `xy-sk120.yaml`). Only serving + handler registration. The handler-registration `on_boot` carries **priority -99**:

```yaml
# Web UI serving package. Adds ONLY web_server local-embed + the custom panel
# handler registration. Auth/port/version live in common.yaml — do not restate.
web_server:
  local: true   # embed native ESPHome page for /esphome; no CDN
  include_internal: false   # keep internal plumbing off the /events stream

esphome:
  includes:
    - web/panel_handler.h
    - web/panel_html_gz.h     # generated; both MUST be listed or the build fails
  on_boot:
    - priority: -99           # register after web_server base is up
      then:
        - lambda: |-
            esphome::web_server_base::global_web_server_base->add_handler(new PanelHandler());
            esphome::web_server_base::global_web_server_base->add_handler(new ESPHomeHandler());
```

- [ ] **Step 5: Wire the package + move web_server out of xy-sk120.yaml**

In `esphome/xy-sk120.yaml`: add the package include, and REMOVE the now-duplicated `web_server:` block (lines ~84-85, the `include_internal: false` one) since `web.yaml` owns it. Set the existing boot-policy `on_boot` (`:91-107`) to **priority: -100**.

Add near the top-level keys:
```yaml
packages:
  web: !include web.yaml
```
Change the existing boot-policy block from (bare) `on_boot:` list entry to carry `priority: -100`:
```yaml
esphome:
  name: "xy-sk120"
  # ...existing...
  on_boot:
    - priority: -100          # boot policy runs late; distinct from handler reg (-99)
      then:
        # ...existing boot-policy steps unchanged...
```

- [ ] **Step 6: Ignore the generated header**

Create or append `esphome/.gitignore`:
```
web/panel_html_gz.h
```

- [ ] **Step 7: Create `esphome/Makefile`**

```make
CONFIG ?= xy-sk120
DEVICE ?= 10.0.0.11

.DEFAULT_GOAL := run

PANEL := web/panel.html
GEN   := web/panel_html_gz.h

.PHONY: run logs compile upload clean gen

$(GEN): $(PANEL) web/panel_handler.h web/gen_panel.py
	python3 web/gen_panel.py

gen: $(GEN)

run: gen
	timeout 600 esphome run $(CONFIG).yaml --device $(DEVICE) --no-logs

compile: gen
	esphome compile $(CONFIG).yaml

logs:
	timeout 14 esphome logs $(CONFIG).yaml --device $(DEVICE)

clean:
	esphome clean $(CONFIG).yaml
```

- [ ] **Step 8: Validate config (pre-build gate)**

Run: `python3 esphome/web/gen_panel.py && cd esphome && esphome config xy-sk120.yaml`
Expected: `gen_panel: panel.html ...B -> panel_html_gz.h ...B gz` then a clean `esphome config` dump (no `on_boot` merge error, no missing-include error). If `esphome config` complains about duplicate `web_server` keys, Step 5's removal was incomplete — fix.

- [ ] **Step 9: Build + flash**

Run: `make -C esphome run`
Expected: compile succeeds, `INFO OTA successful`, `INFO Successfully uploaded program.`

- [ ] **Step 10: Verify serving on-device**

```bash
curl -s -o /dev/null -w "%{http_code} %{redirect_url}\n" http://10.0.0.11/            # 302 -> /panel (or 200 if curl follows)
curl -s --compressed http://10.0.0.11/panel | grep -o 'XY-SK120 panel'                # the stub H1
curl -s http://10.0.0.11/panel/build.json                                             # {config_hash,web_hash,build_time,boot_id}
curl -s -o /dev/null -w "%{http_code}\n" http://10.0.0.11/esphome                     # 200 native
```
Expected: `/` redirects to `/panel`, `/panel` serves the stub, `/panel/build.json` returns JSON with a non-empty `web_hash`, `/esphome` returns 200.

- [ ] **Step 11: Commit**

```bash
cd /Users/rob/workspace/dps150/XY-SK120-Modbus-RTU-TTL
git add esphome/web/panel_handler.h esphome/web/gen_panel.py esphome/web/panel.html esphome/web.yaml esphome/Makefile esphome/.gitignore esphome/xy-sk120.yaml
git commit -m "feat(esphome): custom web panel serving skeleton (D3806 pattern)"
```

---

### Task 2: Port the panel SPA shell + SK120 entity map

**Files:**
- Modify (replace): `esphome/web/panel.html` (from stub → ported D3806 SPA)

**Interfaces:**
- Consumes: the serving routes from Task 1; the 20 entity `name:` strings from spec §2.
- Produces: a FACE view that renders live `Output Voltage`/`Output Current`/`Output Power` from SSE, plus the DIAG/LOGS view. Readouts only this task; controls in Task 3.

- [ ] **Step 1: Copy D3806 panel.html as the base**

Copy `/Users/rob/workspace/D3806/packages/web/panel.html` (1534 lines) over `esphome/web/panel.html`. This brings the `:root` palette (`--bg:#0d1117`, `--v`/`--a`/`--w`/`--hz`), 560px mobile-first column, FACE digit block + SVG sparklines, SSE connect/reconnect/watchdog (`EventSource('/events')`, `state_detail_all`+`state`), priority snapshot fetch, `/panel/build.json` hot-reload footer, DIAG/LOGS views, output confirm-poll, and visibility lifecycle — all reused verbatim.

- [ ] **Step 2: Replace the NAME map with the SK120 entities**

Find D3806's entity NAME map (spec cites `panel.html:407-460`, the `resolveKey` map at `472-481`). Replace the entity entries with exactly these 20 SK120 names (keyed by web_server `name:`). Keep the map's existing shape/field names from D3806 — only the entries change:

Readouts (sensor): `Output Voltage` (V), `Output Current` (A), `Output Power` (W), `Input Voltage` (V), `Internal Temp` (°C), `External NTC Temp` (°C), `Set Wattage` (W), `Device V_SET` (V), `Model ID`, `Firmware Version`.
Binary: `CV Mode` (1=CV/0=CC), `Output State` (output on/off mirror).
Text: `Charge Stage`, `Charge Status`.
Numbers: `Set Voltage` (0–36.5, step .01), `Set Current` (0–6.0, step .001).
Switches: `Output`, `Charge`, `Sim Cycle`.
Button: `STOP`, plus base `Device Restart`.

Map `Output Voltage`→big V digit, `Output Current`→big A, `Output Power`→big W. The sparkline "output on" gate reads the `Output State` binary_sensor (replacing D3806's gate).

- [ ] **Step 3: Strip D3806-only dependencies from the shell**

Delete references that have no SK120 entity (they would dead-wire or error):
- the `Thermal Lockout` binary_sensor + its lockout overlay,
- the ADS raw diagnostic sensors,
- `bench_psu_mode` switch + the PSU/Charger mode button,
- chemistry picker + preset row,
- the entire CONFIG view (spec cites `panel.html:296-366`) and all `cfg_*`/`cal_*`/`led_brightness`/`display_test`/`buck_ntc_override` wiring and its nav entry.

Leave FACE (digits/sparklines/status bar) and DIAG/LOGS intact. Controls block is rebuilt in Task 3 — for now it's fine to leave the control container empty or with just the readouts.

- [ ] **Step 4: Fix the build hash placeholder**

Confirm the HTML still contains the literal `__PANEL_BUILD_HASH__` exactly once (the footer), so `gen_panel.py` can fingerprint it.

Run: `grep -c '__PANEL_BUILD_HASH__' esphome/web/panel.html`
Expected: `1`

- [ ] **Step 5: Build + flash**

Run: `make -C esphome run`
Expected: OTA successful.

- [ ] **Step 6: Verify readouts resolve against the live stream**

```bash
# capture a few seconds of the SSE snapshot and confirm every mapped name appears
curl -s --max-time 4 http://10.0.0.11/events | grep -oE 'Output Voltage|Output Current|Output Power|Input Voltage|Charge Stage|Charge Status|Set Voltage|Set Current|Output State|CV Mode' | sort -u
```
Expected: every mapped readout name that is non-internal appears in the stream. If `Output State` or any mapped name is MISSING, it is either internal (bug — must not be mapped) or misspelled — fix the NAME map.

- [ ] **Step 7: Verify in a browser**

Open `http://10.0.0.11/panel`. Expected: FACE renders; V/A/W digits show live values and move; status bar shows Vin + temps; no JS console errors (open devtools); DIAG view lists all stream entities; no CONFIG nav entry.

- [ ] **Step 8: Commit**

```bash
git add esphome/web/panel.html
git commit -m "feat(esphome): port panel SPA shell + SK120 entity map (readouts)"
```

---

### Task 3: Charger controls — setpoint boxes, Output/Charge/Sim toggles, STOP

**Files:**
- Modify: `esphome/web/panel.html` (FACE controls block)

**Interfaces:**
- Consumes: D3806's `post()` helper (`panel.html:489-493`), number debounce (`489-519`), switch confirm-poll (`1070-1096`).
- Produces: working command path for `Set Voltage`, `Set Current` (POST `/number/<Name>/set?value=`), `Output`/`Charge`/`Sim Cycle` (POST `/switch/<Name>/turn_on|off` + confirm-poll), `STOP` (POST `/button/STOP/press`).

- [ ] **Step 1: Setpoint inputs as boxes**

In the FACE controls block, render `Set Voltage` and `Set Current` as free-input numeric `<input>` boxes (match the YAML `mode: box`), reusing D3806's typed-entry input (`.stepper .val input`, `panel.html:91-92`). Commit on blur/Enter + keystroke debounce ~350 ms → `POST /number/Set%20Voltage/set?value=X` and `/number/Set%20Current/set?value=X`. Clamp client-side to 0–36.5 (V) / 0–6.0 (A). Optional small ± nudge buttons around each box.

- [ ] **Step 2: Output/Charge/Sim toggles with confirm-poll**

Three toggles reusing D3806's `.onoff` + confirm-poll pattern (POST then poll `?detail=all` up to 10×100 ms):
- `Output` → `/switch/Output/turn_on|turn_off`. MUST use confirm-poll (ON returns before the soft-start ramp completes — do not optimistically flip then re-read immediately).
- `Charge` → `/switch/Charge/turn_on|turn_off`.
- `Sim Cycle` → `/switch/Sim%20Cycle/turn_on|turn_off`; when its state is ON, show a persistent "SIM" banner (reuse `.charge-msg.warn`).

- [ ] **Step 3: STOP button — full-width, above the fold**

Add a prominent full-width red `STOP` button at the top of the FACE card, always visible in every view. On click → `POST /button/STOP/press`. It must NOT depend on SSE being connected (fire the POST unconditionally; don't gate on current state).

- [ ] **Step 4: Build + flash**

Run: `make -C esphome run`
Expected: OTA successful.

- [ ] **Step 5: Verify command round-trips (spaced names + confirm)**

```bash
# number write + readback (URL-encoded spaced name)
curl -s -X POST "http://10.0.0.11/number/Set%20Voltage/set?value=13.5"
sleep 1
curl -s --max-time 3 "http://10.0.0.11/number/Set%20Voltage?detail=all" | grep -o '"value":[0-9.]*'   # ~13.5
# button fires with SSE closed (STOP always acts)
curl -s -o /dev/null -w "STOP=%{http_code}\n" -X POST "http://10.0.0.11/button/STOP/press"             # 2xx
```
Expected: `Set Voltage` reads back ~13.5; STOP returns 2xx.

- [ ] **Step 6: Verify Output confirm in browser**

Set a voltage above Vin, toggle `Output` ON. Expected: the toggle settles to ON and stays (confirm-poll rides through the soft-start ramp delay without flapping to OFF). Toggle OFF → output drops. Watch the device: no reset, V ramps per the soft-start.

- [ ] **Step 7: Commit**

```bash
git add esphome/web/panel.html
git commit -m "feat(esphome): panel charger controls — setpoint boxes, Output/Charge/Sim, STOP"
```

---

### Task 4: Charge stage + status badges, fault card, SIM banner

**Files:**
- Modify: `esphome/web/panel.html` (stage indicator + fault card)

**Interfaces:**
- Consumes: `Charge Stage` text (`IDLE`/`CC`/`CV`/`FLOAT`/`DONE`/`FAULT` — matches `charge_stage_text`), `Charge Status` free-text, `Output State` binary, D3806's `.stage-badge`/`.fault` styles.

- [ ] **Step 1: Stage badge**

Bind `Charge Stage` text to a colored badge reusing D3806's `.stage-badge`: IDLE grey, CC amber, CV green, FLOAT blue, DONE green, FAULT red. Place it in the status bar next to CV/CC.

- [ ] **Step 2: Fault card + status line**

When `Charge Stage` == `FAULT` (or `Charge Status` is non-empty and names a fault), show D3806's `.fault` card with the `Charge Status` string as the message (covers: source out of range / depleted, hot abort, overvoltage, pack V out of range). Otherwise show `Charge Status` as a subdued status line.

- [ ] **Step 3: SIM banner polish**

Ensure the `Sim Cycle`-ON banner (Task 3) coexists with the stage badge — during a sim the badge walks CC→CV→FLOAT→DONE with the "SIM" banner visible, making clear the output is NOT live.

- [ ] **Step 4: Build + flash**

Run: `make -C esphome run`
Expected: OTA successful.

- [ ] **Step 5: Verify via a sim run (reuses the committed sim mode)**

Toggle `Sim Cycle` ON in the browser. Expected within ~25 s: SIM banner shows; stage badge walks IDLE→CC→CV→FLOAT→DONE; `Charge Status` ends "sim: complete"; `Set Voltage`/`Set Current` echo the stage setpoints (14.4 then 13.62 / 4.0); real output never turns on (Output State stays off).

- [ ] **Step 6: Commit**

```bash
git add esphome/web/panel.html
git commit -m "feat(esphome): panel charge-stage badge, fault card, SIM banner"
```

---

### Task 5: Promote sim_t / sim_vin to number entities + wire the panel

**Files:**
- Modify: `esphome/xy-sk120.yaml` (new `number` entities; engine already reads `sim_t`/`sim_vin` globals — repoint reads to the entities)
- Modify: `esphome/web/panel.html` (sim-seed inputs, shown only when Sim Cycle armed)

**Interfaces:**
- Consumes: the engine's sim reads (`float t = sim ? id(sim_t) : ...`, `float vin = sim ? id(sim_vin) : ...`).
- Produces: web-settable `Sim Pack Temp` and `Sim Source V` numbers that drive guard testing.

- [ ] **Step 1: Add two template `number` entities**

In `esphome/xy-sk120.yaml`, add web-settable numbers backed by the existing globals (`mode: box`). These replace the code-only seeding for `sim_t`/`sim_vin` (keep `sim_v`/`sim_i` as globals — pack-model-driven):

```yaml
number:
  - platform: template
    name: "Sim Pack Temp"
    id: sim_t_num
    unit_of_measurement: "°C"
    mode: box
    optimistic: true
    min_value: -40
    max_value: 100
    step: 1
    initial_value: 25
    set_action:
      - lambda: 'id(sim_t) = x;'
  - platform: template
    name: "Sim Source V"
    id: sim_vin_num
    unit_of_measurement: "V"
    mode: box
    optimistic: true
    min_value: 0
    max_value: 30
    step: 0.1
    initial_value: 13.2
    set_action:
      - lambda: 'id(sim_vin) = x;'
```

- [ ] **Step 2: Seed the entities (not just the globals) on sim start**

In the `Sim Cycle` switch `turn_on_action` lambda, keep seeding the globals AND publish the entity state so the panel shows the seed. Change the seed lambda so `sim_t`/`sim_vin` come from the numbers if the user set them, else defaults. Simplest: on sim-start, set the globals from the number entities:
```yaml
            - lambda: |-
                id(sim_v) = 12.6f; id(sim_i) = 0.0f;
                id(sim_t)   = id(sim_t_num).state;
                id(sim_vin) = id(sim_vin_num).state;
                id(g_sim) = true; id(cj_run) = true; id(cj_stage) = 0;
                id(cj_t0) = millis(); id(cj_stage_t0) = millis();
                id(cj_taper_t0) = -1; id(cj_srclow_t0) = -1;
                id(cj_status) = std::string("sim: starting");
```
(So the user sets `Sim Source V`=11.0 before arming to test depletion.)

- [ ] **Step 3: Add the two names to the panel NAME map + a sim-seed input group**

Add `Sim Pack Temp` and `Sim Source V` to the panel NAME map (box inputs). Show them in a small "SIM SEED" group that appears when `Sim Cycle` is available (or always, under the Sim toggle). POST `/number/Sim%20Source%20V/set?value=`.

- [ ] **Step 4: Build + flash**

Run: `make -C esphome run`
Expected: OTA successful.

- [ ] **Step 5: Verify a guard trips from the web**

In the browser: set `Sim Source V` = 11.0 (below `src_min_v` 12.0), then toggle `Sim Cycle` ON. Expected: the engine enters CC, then after the `src_low_dwell_s` (15 s) debounce the stage badge goes FAULT with `Charge Status` = "source depleted". Set it back to 13.2, set `Sim Pack Temp` = 60, arm again → "hot at start" / "hot abort" FAULT.

Verify via log too:
```bash
timeout 30 esphome logs esphome/xy-sk120.yaml --device 10.0.0.11 | grep -E '\[sim\]|STOPPED|depleted|hot'
```

- [ ] **Step 6: Commit**

```bash
git add esphome/xy-sk120.yaml esphome/web/panel.html
git commit -m "feat(esphome): web-settable sim seeds (Sim Pack Temp / Sim Source V) for guard testing"
```

---

### Task 6: Integration, hot-reload footer, responsive, acceptance

**Files:**
- Modify: `esphome/web/panel.html` (final polish only if gaps found)
- Verify: `esphome/xy-sk120.yaml`, `esphome/web.yaml` (on_boot merge)

**Interfaces:**
- Consumes: everything from Tasks 1–5.

- [ ] **Step 1: Confirm the on_boot merge produced two distinct entries**

Run: `cd esphome && esphome config xy-sk120.yaml | grep -A2 -iE 'on_boot|priority'`
Expected: both `priority: -100` (boot policy) and `priority: 300` (handler registration, per RULING 4) appear — the package merge concatenated the lists, no collision.

- [ ] **Step 2: Verify build-hash hot-reload**

Touch panel.html (add a comment), rebuild, and confirm the footer `web_hash` changes:
```bash
curl -s http://10.0.0.11/panel/build.json | grep -o '"web_hash":"[^"]*"'   # note value
# make a trivial edit to esphome/web/panel.html, then:
make -C esphome run
curl -s http://10.0.0.11/panel/build.json | grep -o '"web_hash":"[^"]*"'   # value changed
```
Expected: `web_hash` differs after the edit+rebuild. Open tabs auto-`location.reload()` per D3806's footer poller.

- [ ] **Step 3: Responsive check**

In the browser devtools, set a 375px-wide mobile viewport. Expected: the 560px column collapses to full width, modals go fullscreen (`@media (max-width:600px)`), STOP stays full-width and reachable, no horizontal scroll.

- [ ] **Step 4: Full acceptance pass (on-device)**

- `/` → `/panel`; `/esphome` native loads; `/?native=1` native REST base works.
- FACE: V/A/W live + sparklines; status bar Vin/temps/CV-CC/stage; Set V/I echo.
- Set `Set Voltage` from the box, output OFF, enable `Output` → ramps up, no reset.
- `Charge` ON with a safe source → CC badge; `STOP` → everything drops, toggles reset.
- `Sim Cycle` → SIM banner + stage walk + "sim: complete", output never fires.
- Guard test: `Sim Source V`=11 → "source depleted" FAULT.
- DIAG view lists all stream entities; LOG console shows `/events` log lines.
- `Device Restart` reboots.

- [ ] **Step 5: Final commit (only if Step 1–4 required edits)**

```bash
git add -A esphome/
git commit -m "feat(esphome): finalize web panel port — on_boot merge, hot-reload, responsive"
```

---

## Notes for the executor

- There is no unit-test harness for an embedded HTML panel; verification is `esphome config` validation + `curl` smoke tests + a browser pass. The `curl` checks in each task are the runnable gates — treat a missing/unexpected response as a failing test and fix before moving on.
- If `esphome config` errors on `web_server` duplicate keys, Task 1 Step 5 left the old block in `xy-sk120.yaml` — remove it.
- If `/panel` serves stale UI, you built with bare `esphome` instead of `make` — run `python3 esphome/web/gen_panel.py` then rebuild.
- `panel_html_gz.h` must never be committed (`.gitignore` from Task 1).
