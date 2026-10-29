// Drives a real Chromium to the Vcast cast page and shares the (virtual) screen.
// Usage: node tests/browser_cast_test.js <https-url> <seconds> [--audio]
const { chromium } = require('playwright');

(async () => {
  const url = process.argv[2];
  const seconds = parseInt(process.argv[3] || '8', 10);
  const browser = await chromium.launch({
    headless: false,
    args: [
      '--auto-select-desktop-capture-source=Entire screen',
      '--use-fake-ui-for-media-stream',
      '--use-fake-device-for-media-stream',
      '--enable-features=GetDisplayMediaSet',
      // Loopback never goes through an HTTP proxy, WebSockets included.
      '--proxy-bypass-list=<-loopback>',
    ],
  });
  const context = await browser.newContext({ ignoreHTTPSErrors: true });
  const page = await context.newPage();
  if (process.env.FORCE_CODEC) {
    await page.addInitScript((codec) => {
      const orig = MediaRecorder.isTypeSupported.bind(MediaRecorder);
      MediaRecorder.isTypeSupported = (t) => (t.includes(codec) || !t.includes('codecs=')) && orig(t) && !(t === 'video/webm' && false);
    }, process.env.FORCE_CODEC);
  }
  page.on('console', (m) => console.log('[page]', m.text()));
  await page.goto(url);
  console.log('SECURE', await page.evaluate(() => window.isSecureContext));
  await page.waitForFunction(() => document.getElementById('receiver').options.length > 0, null, { timeout: 10000 });
  console.log('RECEIVERS', await page.evaluate(() => [...document.getElementById('receiver').options].map((o) => o.textContent)));
  if (!process.argv.includes('--audio')) await page.uncheck('#audio');
  await page.click('#start');
  await page.waitForTimeout(2000);
  console.log('STATUS', await page.textContent('#status'));
  // Animate something so the encoder has changing content.
  await page.evaluate(() => {
    const d = document.createElement('div');
    d.style.cssText = 'position:fixed;left:0;top:0;width:200px;height:200px;background:red';
    document.body.appendChild(d);
    let x = 0;
    setInterval(() => { x = (x + 10) % 800; d.style.left = x + 'px'; }, 33);
  });
  await page.waitForTimeout(seconds * 1000);
  console.log('STATUS', await page.textContent('#status'));
  await page.click('#stop');
  await page.waitForTimeout(500);
  console.log('STATUS', await page.textContent('#status'));
  await browser.close();
})().catch((e) => { console.error('FAILED', e); process.exit(1); });
