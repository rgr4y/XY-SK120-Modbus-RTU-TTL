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
