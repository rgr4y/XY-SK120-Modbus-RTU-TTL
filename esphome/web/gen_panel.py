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
