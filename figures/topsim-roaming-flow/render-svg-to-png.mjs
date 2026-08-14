import puppeteer from "/opt/homebrew/lib/node_modules/@mermaid-js/mermaid-cli/node_modules/puppeteer/lib/puppeteer/puppeteer.js";

const svgPath = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/open5gs-vs-topsim-sequence.svg";
const pngPath = "/Users/alexis/Documents/PhD/TOPSSIM/TestBed5G/open5gs/figures/open5gs-vs-topsim-sequence.png";

const browser = await puppeteer.launch({
  headless: "new",
  args: ["--no-sandbox", "--disable-setuid-sandbox", "--disable-dev-shm-usage"]
});
const page = await browser.newPage();
await page.goto("file://" + svgPath, { waitUntil: "networkidle0" });
const bbox = await page.evaluate(() => {
  const svg = document.querySelector("svg");
  const box = svg.viewBox.baseVal;
  return { width: Math.ceil(box.width), height: Math.ceil(box.height) };
});
await page.setViewport({ width: bbox.width, height: bbox.height, deviceScaleFactor: 2 });
await page.screenshot({ path: pngPath, type: "png", fullPage: true, omitBackground: false });
await browser.close();
