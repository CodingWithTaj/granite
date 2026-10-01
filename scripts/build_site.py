#!/usr/bin/env python3
"""Build the website into site/ (needs web/granite.wasm: run `make wasm` first).

    site/index.html + app.js + granite.wasm   the multi-file site
    site/granite.html                          one self-contained file
"""
import base64, pathlib, shutil, sys

root = pathlib.Path(__file__).resolve().parent.parent
web, site = root / "web", root / "site"
wasm = web / "granite.wasm"
if not wasm.exists():
    sys.exit("web/granite.wasm not found: run `make wasm` first")
if site.exists():
    shutil.rmtree(site)
site.mkdir()
html = (web / "index.html").read_text(encoding="utf-8")
js = (web / "app.js").read_text(encoding="utf-8")
shutil.copy(wasm, site / "granite.wasm")
(site / "app.js").write_text(js, encoding="utf-8")
(site / "index.html").write_text(html.replace("<!--WASM-->", ""), encoding="utf-8")
b64 = base64.b64encode(wasm.read_bytes()).decode()
solo = html.replace("<!--WASM-->", f'<script>window.GRANITE_WASM_B64 = "{b64}";</script>') \
           .replace('<script src="app.js"></script>', "<script>\n" + js.replace("</script", "<\\/script") + "\n</script>")
(site / "granite.html").write_text(solo, encoding="utf-8")
print(f"built site/ ({wasm.stat().st_size // 1024} KB wasm)")
