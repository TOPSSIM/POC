import fs from "node:fs/promises";
import zlib from "node:zlib";
import puppeteer from "/opt/homebrew/lib/node_modules/@mermaid-js/mermaid-cli/node_modules/puppeteer/lib/puppeteer/puppeteer.js";

const originalMmd = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/topsim-roaming-flow/original-open5gs.mmd";
const topssimMmd = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/topsim-roaming-flow/topsim-sdmcf-rcf.mmd";
const originalPng = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/topsim-roaming-flow/original-open5gs-live.png";
const topssimPng = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/topsim-roaming-flow/topsim-sdmcf-rcf-live.png";
const outputPng = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/open5gs-vs-topsim-sequence.png";

function toBase64Url(buffer) {
  return buffer.toString("base64").replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function liveUrl(code) {
  const state = {
    code,
    mermaid: JSON.stringify({ theme: "default" }),
    autoSync: true,
    updateDiagram: true,
    rough: false
  };
  const encoded = toBase64Url(zlib.deflateSync(Buffer.from(JSON.stringify(state), "utf8")));
  return `https://mermaid.live/view#pako:${encoded}`;
}

async function renderLiveDiagram(browser, code, outPath) {
  const page = await browser.newPage();
  await page.setViewport({ width: 1800, height: 1400, deviceScaleFactor: 2 });
  await page.goto(liveUrl(code), { waitUntil: "load", timeout: 60000 });
  try {
    await page.waitForFunction(() => {
      const svgs = [...document.querySelectorAll("svg")];
      return svgs.some(svg =>
        svg.textContent.includes("Registration Request")
      );
    }, { timeout: 60000 });
  } catch (error) {
    const debugPng = outPath.replace(/\.png$/, ".debug.png");
    const debugTxt = outPath.replace(/\.png$/, ".debug.txt");
    await page.screenshot({ path: debugPng, type: "png", fullPage: true, omitBackground: false });
    const bodyText = await page.evaluate(() => document.body.innerText.slice(0, 4000));
    await fs.writeFile(debugTxt, bodyText, "utf8");
    throw new Error(`Mermaid Live did not render the expected diagram. Debug files: ${debugPng}, ${debugTxt}`);
  }

  const metrics = await page.evaluate(() => {
    const svg = [...document.querySelectorAll("svg")]
      .find(candidate => candidate.textContent.includes("Registration Request"));
    const svgRect = svg.getBoundingClientRect();
    const text = [...svg.querySelectorAll("text")]
      .find(node => node.textContent.includes("Phase 1 - Registration"));
    const textRect = text.getBoundingClientRect();
    const phaseTopY = Math.max(0, textRect.top - svgRect.top - 34);
    const actorLabels = ["UE", "V-AMF", "V-NRF", "H-UDM", "V-SMF"];
    const actorXs = actorLabels
      .map(label => {
        const node = [...svg.querySelectorAll("text")]
          .find(candidate => candidate.textContent.trim() === label);
        if (!node) return null;
        const rect = node.getBoundingClientRect();
        return (rect.left + rect.width / 2) - svgRect.left;
      })
      .filter(value => value !== null);
    const clone = svg.cloneNode(true);
    clone.setAttribute("width", String(Math.ceil(svgRect.width)));
    clone.setAttribute("height", String(Math.ceil(svgRect.height)));
    clone.style.maxWidth = "none";
    clone.style.overflow = "hidden";
    clone.style.backgroundColor = "white";
    return {
      width: Math.ceil(svgRect.width),
      height: Math.ceil(svgRect.height),
      anchorY: (textRect.top + textRect.height / 2) - svgRect.top,
      phaseTopY,
      actorXs,
      svgHtml: clone.outerHTML
    };
  });
  await page.close();

  const cleanPage = await browser.newPage();
  await cleanPage.setViewport({
    width: metrics.width,
    height: metrics.height,
    deviceScaleFactor: 2
  });
  await cleanPage.setContent(`<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
  body { margin: 0; background: white; }
  #wrap { width: ${metrics.width}px; height: ${metrics.height}px; overflow: hidden; background: white; }
</style>
</head>
<body><div id="wrap">${metrics.svgHtml}</div></body>
</html>`, { waitUntil: "load" });
  const wrap = await cleanPage.$("#wrap");
  await wrap.screenshot({ path: outPath, omitBackground: false });
  await cleanPage.close();
  delete metrics.svgHtml;
  return metrics;
}

function dataUrl(path) {
  return fs.readFile(path).then(buffer => `data:image/png;base64,${buffer.toString("base64")}`);
}

const browser = await puppeteer.launch({
  headless: "new",
  args: ["--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage"]
});

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
      `<div class="gap-line" style="left:${leftX + x * leftScale}px; top:${leftGapTop}px; height:${leftGapBottom - leftGapTop}px;"></div>`
    ).join("")
  : "";

const html = `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
  * { box-sizing: border-box; }
  body {
    margin: 0;
    width: ${W}px;
    height: ${H}px;
    background: white;
    font-family: Arial, Helvetica, sans-serif;
    color: #111827;
    position: relative;
  }
  .title {
    position: absolute;
    top: 38px;
    left: 0;
    width: 100%;
    text-align: center;
    font-size: 30px;
    font-weight: 700;
  }
  .label {
    position: absolute;
    top: ${margin + titleH}px;
    width: ${panelW}px;
    text-align: center;
    font-size: 23px;
    font-weight: 700;
  }
  .left-label { left: ${leftX}px; color: #374151; }
  .right-label { left: ${rightX}px; color: #1d4ed8; }
  .divider {
    position: absolute;
    left: ${margin + panelW + gap / 2}px;
    top: ${margin + titleH - 12}px;
    height: ${H - margin - titleH + 12}px;
    border-left: 2px solid #d1d5db;
  }
  .gap-line {
    position: absolute;
    width: 3px;
    background: #8b5cf6;
    opacity: 0.75;
    transform: translateX(-1.5px);
    z-index: 1;
  }
  .runtime-boundary {
    position: absolute;
    left: ${margin}px;
    top: ${runtimeBoundaryY}px;
    width: ${W - 2 * margin}px;
    border-top: 3px dotted #7c3aed;
    opacity: 0.75;
    z-index: 6;
  }
  .runtime-legend {
    position: absolute;
    right: ${margin}px;
    top: ${runtimeBoundaryY - 38}px;
    padding: 8px 12px;
    border: 1px solid #c4b5fd;
    border-radius: 8px;
    background: rgba(255,255,255,0.94);
    color: #4c1d95;
    font-size: 17px;
    font-weight: 700;
    z-index: 7;
  }
  img {
    position: absolute;
    display: block;
    object-fit: contain;
  }
  .clip {
    position: absolute;
    overflow: hidden;
    background: white;
    z-index: 2;
  }
  .clip img {
    left: 0;
    width: ${original.width * leftScale}px;
    height: ${leftH}px;
  }
</style>
</head>
<body>
  <div class="title">Roaming user-profile retrieval: Open5GS baseline vs TOPSSIM baseline</div>
  <div class="label left-label">Original Open5GS</div>
  <div class="label right-label">TOPSSIM SDM-CF / RCF baseline</div>
  <div class="divider"></div>
  ${gapLineHtml}
  <div class="clip" style="left:${leftX}px; top:${leftTop}px; width:${original.width * leftScale}px; height:${leftTopCropH}px;">
    <img src="${leftData}" style="top:0;">
  </div>
  <div class="clip" style="left:${leftX}px; top:${leftBottomVisibleTop}px; width:${original.width * leftScale}px; height:${leftBottomH}px;">
    <img src="${leftData}" style="top:${-leftCropY * leftScale}px;">
  </div>
  <img src="${rightData}" style="left:${rightX}px; top:${rightTop}px; width:${topssim.width * rightScale}px; height:${rightH}px;">
  <div class="runtime-boundary"></div>
  <div class="runtime-legend">Above: Phase 0 before runtime / roaming</div>
</body>
</html>`;

await page.setViewport({ width: W, height: H, deviceScaleFactor: 2 });
await page.setContent(html, { waitUntil: "load" });
await page.screenshot({ path: outputPng, type: "png", fullPage: true, omitBackground: false });
await browser.close();
