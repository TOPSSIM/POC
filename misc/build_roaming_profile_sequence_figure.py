#!/usr/bin/env python3
"""Build a side-by-side Mermaid sequence-diagram figure.

The script renders two independent Mermaid sequence diagrams with Mermaid CLI
(`mmdc`) and combines the resulting SVGs into one slide-friendly SVG.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from xml.etree import ElementTree as ET


ORIGINAL_MMD = r"""
%%{init: {
  "theme": "default",
  "sequence": {
    "showSequenceNumbers": false,
    "mirrorActors": false,
    "wrap": true,
    "width": 135
  }
}}%%
sequenceDiagram
    participant UE
    participant VAMF as V-AMF
    participant VNRF as V-NRF
    participant HUDM as H-UDM
    participant VSMF as V-SMF

    rect rgb(245,245,245)
    note over UE,HUDM: Phase 1 - Registration
    UE->>VAMF: Registration Request
    VAMF->>VNRF: Nnrf_NFDiscovery<br/>discover H-UDM
    VNRF-->>VAMF: H-UDM profile
    VAMF->>HUDM: Nudm_SDM GET<br/>am-data, smf-select-data
    HUDM-->>VAMF: AM + SMF-selection data
    end

    rect rgb(245,245,245)
    note over UE,HUDM: Phase 2 - PDU Session
    UE->>VAMF: PDU Session Request<br/>DNN, S-NSSAI
    VAMF->>VNRF: Nnrf_NFDiscovery<br/>discover V-SMF
    VNRF-->>VAMF: V-SMF profile
    VAMF->>VSMF: Nsmf_PDUSession<br/>Create SM Context
    VSMF->>HUDM: Discover H-UDM +<br/>Nudm_SDM GET sm-data
    HUDM-->>VSMF: sm-data
    end
"""


TOPSSIM_MMD = r"""
%%{init: {
  "theme": "default",
  "sequence": {
    "showSequenceNumbers": false,
    "mirrorActors": false,
    "wrap": true,
    "width": 135
  }
}}%%
sequenceDiagram
    participant TOP as TOPSSIM
    participant CACHE as RCF / SDM-CF
    participant HUDM as H-UDM
    participant UE
    participant VAMF as V-AMF
    participant VSMF as V-SMF

    rect rgb(232,245,255)
    note over TOP,HUDM: Phase 0 - Before roaming
    TOP->>HUDM: Prefetch eligible Nudm_SDM<br/>am-data, smf-select-data,<br/>optional LBO sm-data
    HUDM-->>TOP: Authorized subscription data
    TOP->>CACHE: Install PreparedSdmProfile<br/>DiscoveryCacheEntry<br/>UDM_ROUTE_ENTRY
    end

    rect rgb(240,255,240)
    note over CACHE,VAMF: Phase 1 - Registration
    UE->>VAMF: Registration Request
    VAMF->>CACHE: Nnrf_NFDiscovery + Nudm_SDM<br/>am-data, smf-select-data
    CACHE-->>VAMF: Local prepared AM +<br/>SMF-selection data
    end

    rect rgb(240,255,240)
    note over CACHE,VSMF: Phase 2 - PDU Session
    UE->>VAMF: PDU Session Request<br/>DNN, S-NSSAI
    VAMF->>CACHE: Nnrf_NFDiscovery<br/>discover V-SMF
    CACHE-->>VAMF: Cached V-SMF profile
    VAMF->>VSMF: Nsmf_PDUSession<br/>Create SM Context
    VSMF->>CACHE: Nnrf_NFDiscovery + Nudm_SDM<br/>GET sm-data
    CACHE-->>VSMF: Local prepared sm-data
    end
"""


SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)


def parse_svg_viewbox(path: Path) -> tuple[float, float, float, float]:
    root = ET.parse(path).getroot()
    view_box = root.attrib.get("viewBox")
    if view_box:
        parts = [float(x) for x in re.split(r"[\s,]+", view_box.strip())]
        if len(parts) == 4:
            return parts[0], parts[1], parts[2], parts[3]

    width = root.attrib.get("width")
    height = root.attrib.get("height")
    if width and height:
        return (
            0.0,
            0.0,
            float(re.sub(r"[^0-9.]", "", width)),
            float(re.sub(r"[^0-9.]", "", height)),
        )

    raise ValueError(f"Cannot determine SVG size for {path}")


def parse_svg_size(path: Path) -> tuple[float, float]:
    _, _, width, height = parse_svg_viewbox(path)
    return width, height


def find_text_y(path: Path, needle: str) -> float:
    root = ET.parse(path).getroot()
    for element in root.iter():
        if not element.tag.endswith("text"):
            continue
        text = "".join(element.itertext()).strip()
        if needle in text:
            y = element.attrib.get("y")
            if y is None:
                break
            return float(y)
    raise ValueError(f"Cannot find text anchor {needle!r} in {path}")


def read_inner_svg(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(r"<svg\b[^>]*>(.*)</svg>\s*$", text, flags=re.S)
    if not match:
        raise ValueError(f"Cannot extract SVG body from {path}")
    return match.group(1)


def run_mmdc(
    input_path: Path,
    output_path: Path,
    background: str,
    puppeteer_config: Path,
) -> None:
    command = [
        "mmdc",
        "-i",
        str(input_path),
        "-o",
        str(output_path),
        "-p",
        str(puppeteer_config),
        "--backgroundColor",
        background,
        "--scale",
        "1",
    ]
    subprocess.run(command, check=True)


def combine_svgs(left_svg: Path, right_svg: Path, output_svg: Path) -> None:
    left_w, left_h = parse_svg_size(left_svg)
    right_w, right_h = parse_svg_size(right_svg)
    left_anchor_y = find_text_y(left_svg, "Registration Request")
    right_anchor_y = find_text_y(right_svg, "Registration Request")

    gap = 32
    margin = 24
    title_h = 44
    label_h = 34
    left_scale = 1.0
    right_scale = 1.0

    if left_anchor_y < right_anchor_y:
        left_y_offset = right_anchor_y - left_anchor_y
        right_y_offset = 0.0
    else:
        left_y_offset = 0.0
        right_y_offset = left_anchor_y - right_anchor_y

    panel_w = max(left_w, right_w)
    content_h = max(left_y_offset + left_h, right_y_offset + right_h)
    total_w = margin * 2 + panel_w * 2 + gap
    total_h = margin * 2 + title_h + label_h + content_h

    left_x = margin + (panel_w - left_w) / 2
    right_x = margin + panel_w + gap + (panel_w - right_w) / 2
    diagram_y = margin + title_h + label_h
    aligned_runtime_y = diagram_y + max(left_anchor_y + left_y_offset,
                                        right_anchor_y + right_y_offset)

    left_body = read_inner_svg(left_svg)
    right_body = read_inner_svg(right_svg)

    output = f"""<svg xmlns="{SVG_NS}" width="{total_w:.0f}" height="{total_h:.0f}" viewBox="0 0 {total_w:.0f} {total_h:.0f}">
  <rect width="100%" height="100%" fill="white"/>
  <text x="{total_w / 2:.1f}" y="{margin + 20}" text-anchor="middle" font-family="Arial" font-size="22" font-weight="700" fill="#111827">Roaming user-profile retrieval: Open5GS baseline vs TOPSSIM baseline</text>
  <text x="{margin + panel_w / 2:.1f}" y="{margin + title_h + 20}" text-anchor="middle" font-family="Arial" font-size="18" font-weight="700" fill="#374151">Original Open5GS</text>
  <text x="{margin + panel_w + gap + panel_w / 2:.1f}" y="{margin + title_h + 20}" text-anchor="middle" font-family="Arial" font-size="18" font-weight="700" fill="#1d4ed8">TOPSSIM SDM-CF / RCF baseline</text>
  <line x1="{margin + panel_w + gap / 2:.1f}" y1="{margin + title_h:.1f}" x2="{margin + panel_w + gap / 2:.1f}" y2="{total_h - margin:.1f}" stroke="#d1d5db" stroke-width="2"/>
  <line x1="{margin:.1f}" y1="{aligned_runtime_y:.1f}" x2="{total_w - margin:.1f}" y2="{aligned_runtime_y:.1f}" stroke="#9ca3af" stroke-width="1.5" stroke-dasharray="6 5"/>
  <text x="{total_w - margin:.1f}" y="{aligned_runtime_y - 8:.1f}" text-anchor="end" font-family="Arial" font-size="13" fill="#6b7280">first common runtime exchange</text>
  <g transform="translate({left_x:.1f},{diagram_y + left_y_offset:.1f}) scale({left_scale:.5f})">
{left_body}
  </g>
  <g transform="translate({right_x:.1f},{diagram_y + right_y_offset:.1f}) scale({right_scale:.5f})">
{right_body}
  </g>
</svg>
"""
    output_svg.write_text(output, encoding="utf-8")


def render_svg_to_png(svg_path: Path, png_path: Path, work_dir: Path) -> None:
    npm_root = subprocess.check_output(["npm", "root", "-g"], text=True).strip()
    puppeteer_import = (
        Path(npm_root)
        / "@mermaid-js"
        / "mermaid-cli"
        / "node_modules"
        / "puppeteer"
        / "lib"
        / "puppeteer"
        / "puppeteer.js"
    )
    if not puppeteer_import.exists():
        raise FileNotFoundError(f"Cannot find Mermaid CLI puppeteer module at {puppeteer_import}")

    renderer = work_dir / "render-svg-to-png.mjs"
    renderer.write_text(
        f"""
import puppeteer from {json.dumps(str(puppeteer_import))};

const svgPath = {json.dumps(str(svg_path.resolve()))};
const pngPath = {json.dumps(str(png_path.resolve()))};

const browser = await puppeteer.launch({{
  headless: "new",
  args: ["--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage"]
}});
const page = await browser.newPage();
await page.goto("file://" + svgPath, {{ waitUntil: "networkidle0" }});
const bbox = await page.evaluate(() => {{
  const svg = document.querySelector("svg");
  const box = svg.viewBox.baseVal;
  return {{ width: Math.ceil(box.width), height: Math.ceil(box.height) }};
}});
await page.setViewport({{ width: bbox.width, height: bbox.height, deviceScaleFactor: 2 }});
await page.screenshot({{ path: pngPath, type: "png", fullPage: true, omitBackground: false }});
await browser.close();
""".lstrip(),
        encoding="utf-8",
    )
    subprocess.run(["node", str(renderer)], check=True)


def render_with_mermaid_live(
    original_mmd: Path,
    topssim_mmd: Path,
    output_png: Path,
    work_dir: Path,
) -> None:
    npm_root = subprocess.check_output(["npm", "root", "-g"], text=True).strip()
    puppeteer_import = (
        Path(npm_root)
        / "@mermaid-js"
        / "mermaid-cli"
        / "node_modules"
        / "puppeteer"
        / "lib"
        / "puppeteer"
        / "puppeteer.js"
    )
    if not puppeteer_import.exists():
        raise FileNotFoundError(f"Cannot find Mermaid CLI puppeteer module at {puppeteer_import}")

    renderer = work_dir / "render-via-mermaid-live.mjs"
    original_png = work_dir / "original-open5gs-live.png"
    topssim_png = work_dir / "topsim-sdmcf-rcf-live.png"

    renderer.write_text(
        f"""
import fs from "node:fs/promises";
import zlib from "node:zlib";
import puppeteer from {json.dumps(str(puppeteer_import))};

const originalMmd = {json.dumps(str(original_mmd.resolve()))};
const topssimMmd = {json.dumps(str(topssim_mmd.resolve()))};
const originalPng = {json.dumps(str(original_png.resolve()))};
const topssimPng = {json.dumps(str(topssim_png.resolve()))};
const outputPng = {json.dumps(str(output_png.resolve()))};

function toBase64Url(buffer) {{
  return buffer.toString("base64").replace(/\\+/g, "-").replace(/\\//g, "_").replace(/=+$/g, "");
}}

function liveUrl(code) {{
  const state = {{
    code,
    mermaid: JSON.stringify({{ theme: "default" }}),
    autoSync: true,
    updateDiagram: true,
    rough: false
  }};
  const encoded = toBase64Url(zlib.deflateSync(Buffer.from(JSON.stringify(state), "utf8")));
  return `https://mermaid.live/view#pako:${{encoded}}`;
}}

async function renderLiveDiagram(browser, code, outPath) {{
  const page = await browser.newPage();
  await page.setViewport({{ width: 1800, height: 1400, deviceScaleFactor: 2 }});
  await page.goto(liveUrl(code), {{ waitUntil: "load", timeout: 60000 }});
  try {{
    await page.waitForFunction(() => {{
      const svgs = [...document.querySelectorAll("svg")];
      return svgs.some(svg =>
        svg.textContent.includes("Registration Request")
      );
    }}, {{ timeout: 60000 }});
  }} catch (error) {{
    const debugPng = outPath.replace(/\\.png$/, ".debug.png");
    const debugTxt = outPath.replace(/\\.png$/, ".debug.txt");
    await page.screenshot({{ path: debugPng, type: "png", fullPage: true, omitBackground: false }});
    const bodyText = await page.evaluate(() => document.body.innerText.slice(0, 4000));
    await fs.writeFile(debugTxt, bodyText, "utf8");
    throw new Error(`Mermaid Live did not render the expected diagram. Debug files: ${{debugPng}}, ${{debugTxt}}`);
  }}

  const metrics = await page.evaluate(() => {{
    const svg = [...document.querySelectorAll("svg")]
      .find(candidate => candidate.textContent.includes("Registration Request"));
    const svgRect = svg.getBoundingClientRect();
    const text = [...svg.querySelectorAll("text")]
      .find(node => node.textContent.includes("Phase 1 - Registration"));
    const textRect = text.getBoundingClientRect();
    const phaseTopY = Math.max(0, textRect.top - svgRect.top - 34);
    const actorLabels = ["UE", "V-AMF", "V-NRF", "H-UDM", "V-SMF"];
    const actorXs = actorLabels
      .map(label => {{
        const node = [...svg.querySelectorAll("text")]
          .find(candidate => candidate.textContent.trim() === label);
        if (!node) return null;
        const rect = node.getBoundingClientRect();
        return (rect.left + rect.width / 2) - svgRect.left;
      }})
      .filter(value => value !== null);
    const clone = svg.cloneNode(true);
    clone.setAttribute("width", String(Math.ceil(svgRect.width)));
    clone.setAttribute("height", String(Math.ceil(svgRect.height)));
    clone.style.maxWidth = "none";
    clone.style.overflow = "hidden";
    clone.style.backgroundColor = "white";
    return {{
      width: Math.ceil(svgRect.width),
      height: Math.ceil(svgRect.height),
      anchorY: (textRect.top + textRect.height / 2) - svgRect.top,
      phaseTopY,
      actorXs,
      svgHtml: clone.outerHTML
    }};
  }});
  await page.close();

  const cleanPage = await browser.newPage();
  await cleanPage.setViewport({{
    width: metrics.width,
    height: metrics.height,
    deviceScaleFactor: 2
  }});
  await cleanPage.setContent(`<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
  body {{ margin: 0; background: white; }}
  #wrap {{ width: ${{metrics.width}}px; height: ${{metrics.height}}px; overflow: hidden; background: white; }}
</style>
</head>
<body><div id="wrap">${{metrics.svgHtml}}</div></body>
</html>`, {{ waitUntil: "load" }});
  const wrap = await cleanPage.$("#wrap");
  await wrap.screenshot({{ path: outPath, omitBackground: false }});
  await cleanPage.close();
  delete metrics.svgHtml;
  return metrics;
}}

function dataUrl(path) {{
  return fs.readFile(path).then(buffer => `data:image/png;base64,${{buffer.toString("base64")}}`);
}}

const browser = await puppeteer.launch({{
  headless: "new",
  args: ["--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage"]
}});

const originalCode = await fs.readFile(originalMmd, "utf8");
const topssimCode = await fs.readFile(topssimMmd, "utf8");
const original = await renderLiveDiagram(browser, originalCode, originalPng);
const topssim = await renderLiveDiagram(browser, topssimCode, topssimPng);

const leftData = await dataUrl(originalPng);
const rightData = await dataUrl(topssimPng);

const page = await browser.newPage();
const W = 3000;
const margin = 82;
const gap = 48;
const titleH = 104;
const labelH = 52;
const panelW = (W - margin * 2 - gap) / 2;
const leftScale = Math.min(panelW / original.width, 1.15);
const rightScale = Math.min(panelW / topssim.width, 1.15);
const baseY = margin + titleH + labelH;
const rightTop = baseY;
const anchorY = rightTop + topssim.anchorY * rightScale;
const leftTop = baseY;
const leftBottomTop = anchorY - original.anchorY * leftScale;
const leftCropY = Math.max(0, original.phaseTopY);
const legendCropLift = 24;
const leftTopCropH = Math.max(0, leftCropY - legendCropLift) * leftScale;
const leftBottomVisibleTop = leftBottomTop + leftCropY * leftScale;
const leftBottomH = (original.height - leftCropY) * leftScale;
const leftH = original.height * leftScale;
const rightH = topssim.height * rightScale;
const H = Math.ceil(Math.max(leftTop + leftTopCropH, leftBottomVisibleTop + leftBottomH, rightTop + rightH) + margin);
const leftX = margin;
const rightX = margin + panelW + gap;
const leftGapTop = leftTop + leftTopCropH;
const leftGapBottom = leftBottomVisibleTop;
const runtimeBoundaryY = Math.min(leftBottomVisibleTop, rightTop + topssim.phaseTopY * rightScale) - 14;
const gapLineHtml = leftGapBottom > leftGapTop
  ? original.actorXs.map((x) =>
      `<div class="gap-line" style="left:${{leftX + x * leftScale}}px; top:${{leftGapTop}}px; height:${{leftGapBottom - leftGapTop}}px;"></div>`
    ).join("")
  : "";

const html = `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0;
    width: ${{W}}px;
    height: ${{H}}px;
    background: white;
    font-family: Arial, Helvetica, sans-serif;
    color: #111827;
    position: relative;
  }}
  .title {{
    position: absolute;
    top: 38px;
    left: 0;
    width: 100%;
    text-align: center;
    font-size: 30px;
    font-weight: 700;
  }}
  .label {{
    position: absolute;
    top: ${{margin + titleH}}px;
    width: ${{panelW}}px;
    text-align: center;
    font-size: 23px;
    font-weight: 700;
  }}
  .left-label {{ left: ${{leftX}}px; color: #374151; }}
  .right-label {{ left: ${{rightX}}px; color: #1d4ed8; }}
  .divider {{
    position: absolute;
    left: ${{margin + panelW + gap / 2}}px;
    top: ${{margin + titleH - 12}}px;
    height: ${{H - margin - titleH + 12}}px;
    border-left: 2px solid #d1d5db;
  }}
  .gap-line {{
    position: absolute;
    width: 3px;
    background: #8b5cf6;
    opacity: 0.75;
    transform: translateX(-1.5px);
    z-index: 1;
  }}
  .runtime-boundary {{
    position: absolute;
    left: ${{margin}}px;
    top: ${{runtimeBoundaryY}}px;
    width: ${{W - 2 * margin}}px;
    border-top: 3px dotted #7c3aed;
    opacity: 0.75;
    z-index: 6;
  }}
  .runtime-legend {{
    position: absolute;
    right: ${{margin}}px;
    top: ${{runtimeBoundaryY - 38}}px;
    padding: 8px 12px;
    border: 1px solid #c4b5fd;
    border-radius: 8px;
    background: rgba(255,255,255,0.94);
    color: #4c1d95;
    font-size: 17px;
    font-weight: 700;
    z-index: 7;
  }}
  img {{
    position: absolute;
    display: block;
    object-fit: contain;
  }}
  .clip {{
    position: absolute;
    overflow: hidden;
    background: white;
    z-index: 2;
  }}
  .clip img {{
    left: 0;
    width: ${{original.width * leftScale}}px;
    height: ${{leftH}}px;
  }}
</style>
</head>
<body>
  <div class="title">Roaming user-profile retrieval: Open5GS baseline vs TOPSSIM baseline</div>
  <div class="label left-label">Original Open5GS</div>
  <div class="label right-label">TOPSSIM SDM-CF / RCF baseline</div>
  <div class="divider"></div>
  ${{gapLineHtml}}
  <div class="clip" style="left:${{leftX}}px; top:${{leftTop}}px; width:${{original.width * leftScale}}px; height:${{leftTopCropH}}px;">
    <img src="${{leftData}}" style="top:0;">
  </div>
  <div class="clip" style="left:${{leftX}}px; top:${{leftBottomVisibleTop}}px; width:${{original.width * leftScale}}px; height:${{leftBottomH}}px;">
    <img src="${{leftData}}" style="top:${{-leftCropY * leftScale}}px;">
  </div>
  <img src="${{rightData}}" style="left:${{rightX}}px; top:${{rightTop}}px; width:${{topssim.width * rightScale}}px; height:${{rightH}}px;">
  <div class="runtime-boundary"></div>
  <div class="runtime-legend">Above: Phase 0 before runtime / roaming</div>
</body>
</html>`;

await page.setViewport({{ width: W, height: H, deviceScaleFactor: 2 }});
await page.setContent(html, {{ waitUntil: "load" }});
await page.screenshot({{ path: outputPng, type: "png", fullPage: true, omitBackground: false }});
await browser.close();
""".lstrip(),
        encoding="utf-8",
    )
    subprocess.run(["node", str(renderer)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("figures/topsim-roaming-flow"),
        help="Directory for generated .mmd and .svg files.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("figures/open5gs-vs-topsim-sequence.svg"),
        help="Final combined SVG path.",
    )
    parser.add_argument(
        "--png",
        type=Path,
        default=Path("figures/open5gs-vs-topsim-sequence.png"),
        help="Final combined PNG path.",
    )
    parser.add_argument(
        "--background",
        default="white",
        help="Background color passed to Mermaid CLI.",
    )
    parser.add_argument(
        "--renderer",
        choices=["live", "mmdc"],
        default="live",
        help="Renderer to use. 'live' automates mermaid.live; 'mmdc' uses Mermaid CLI locally.",
    )
    args = parser.parse_args()

    if not shutil.which("mmdc"):
        print(
            "Mermaid CLI was not found. Install it with:\n"
            "  npm install -g @mermaid-js/mermaid-cli\n",
            file=sys.stderr,
        )
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    original_mmd = args.out_dir / "original-open5gs.mmd"
    topssim_mmd = args.out_dir / "topsim-sdmcf-rcf.mmd"
    original_svg = args.out_dir / "original-open5gs.svg"
    topssim_svg = args.out_dir / "topsim-sdmcf-rcf.svg"
    puppeteer_config = args.out_dir / "puppeteer-config.json"

    original_mmd.write_text(ORIGINAL_MMD.strip() + "\n", encoding="utf-8")
    topssim_mmd.write_text(TOPSSIM_MMD.strip() + "\n", encoding="utf-8")
    puppeteer_config.write_text(
        json.dumps(
            {
                "args": [
                    "--no-sandbox",
                    "--disable-setuid-sandbox",
                    "--disable-dev-shm-usage",
                ]
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    if args.renderer == "live":
        render_with_mermaid_live(original_mmd, topssim_mmd, args.png, args.out_dir)
    else:
        run_mmdc(original_mmd, original_svg, args.background, puppeteer_config)
        run_mmdc(topssim_mmd, topssim_svg, args.background, puppeteer_config)
        combine_svgs(original_svg, topssim_svg, args.output)
        render_svg_to_png(args.output, args.png, args.out_dir)

    print(f"Wrote {args.png}")
    if args.renderer == "mmdc":
        print(f"Wrote {args.output}")
    print(f"Intermediate files are in {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
