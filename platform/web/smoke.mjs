// Smoke test for the web build: serves the build folder, opens it in headless Chromium (WebGL2 via
// SwiftShader), starts a spectated bot match through the URL (?map=0&spectate), and fails on
// page/console errors, a missing WebGL2 context, or a blank frame.
//
//   npm install --no-save playwright && npx playwright install chromium
//   node platform/web/smoke.mjs <build dir with index.html> [out dir] [seconds]
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(process.argv[2] ?? 'build/web');
const outDir = path.resolve(process.argv[3] ?? 'web-smoke');
const seconds = Number(process.argv[4] ?? 20);
fs.mkdirSync(outDir, { recursive: true });

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream' };
const server = http.createServer((req, res) => {
  const file = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] ?? 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/index.html?map=0&spectate`;

const errors = [];
const log = [];
const browser = await chromium.launch({
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
});
let failed = false;
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 760 } });
  page.on('console', (m) => { log.push(`[${m.type()}] ${m.text()}`); if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push(`pageerror: ${e.message}`));
  page.on('requestfailed', (r) => errors.push(`request failed: ${r.url()}`));

  await page.goto(url);
  // Loading is done when the shell hides its status line (content preloaded, runtime started).
  await page.waitForFunction(() => getComputedStyle(document.getElementById('status')).display === 'none', null, { timeout: 60000 });
  await page.waitForTimeout(seconds * 1000);

  const shot = path.join(outDir, 'web.png');
  await page.screenshot({ path: shot });

  // Analyse the screenshot in a 2D canvas: a rendered match has many distinct colours.
  const b64 = fs.readFileSync(shot).toString('base64');
  const probe = await browser.newPage();
  const stats = await probe.evaluate(async (src) => {
    const img = new Image(); img.src = src; await img.decode();
    const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
    const g = c.getContext('2d'); g.drawImage(img, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height).data;
    const colours = new Set(); let lum = 0, n = 0;
    for (let y = 0; y < c.height; y += Math.max(1, c.height >> 6))
      for (let x = 0; x < c.width; x += Math.max(1, c.width >> 6)) {
        const i = (y * c.width + x) * 4;
        colours.add((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]);
        lum += d[i] * 0.3 + d[i + 1] * 0.59 + d[i + 2] * 0.11; n++;
      }
    return { colours: colours.size, lum: lum / n };
  }, `data:image/png;base64,${b64}`);
  console.log(`screenshot: ${stats.colours} distinct sampled colours, mean luminance ${stats.lum.toFixed(0)}`);
  if (stats.colours < 32 || stats.lum < 8) errors.push('frame is blank — WebGL2 rendering is broken');
} catch (e) {
  errors.push(String(e));
} finally {
  fs.writeFileSync(path.join(outDir, 'console.txt'), log.join('\n') + '\n');
  await browser.close();
  server.close();
}
if (errors.length) {
  failed = true;
  for (const e of errors) console.log(`::error::web: ${e}`);
}
console.log(failed ? 'Web smoke test FAILED' : 'Web smoke test passed');
process.exit(failed ? 1 : 0);
