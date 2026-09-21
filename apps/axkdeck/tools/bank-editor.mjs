import assert from 'node:assert/strict';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawn } from 'node:child_process';
import { createServer } from 'vite';
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const output = resolve(process.argv[2] ?? '../../../build/logs/sample-bank-editor/00001');
await mkdir(output, {recursive: true});
const checks = await readFile(new URL('./bank-editor-checks.js', import.meta.url), 'utf8');
const server = await createServer({server: {host: '127.0.0.1', port: 0, strictPort: true}});
let browser;
const results = [];
try {
    await server.listen();
    const base = `http://127.0.0.1:${server.httpServer.address().port}`;
    browser = await chromium.launch({executablePath: process.env.CHROMIUM_PATH, headless: true, args: ['--autoplay-policy=no-user-gesture-required']});
    for (const width of [390, 800, 1600]) for (const native of [false, true]) {
        const page = await browser.newPage({viewport: {width, height: 900}});
        const name = `${width}-${native ? 'native' : 'later'}`;
        const errors = [];
        page.on('pageerror', error => errors.push(error.message));
        try {
            await page.goto(`${base}/tools/layout-fixtures/bank-editor.html${native ? '?short-stereo' : ''}`);
            await page.addScriptTag({content: checks});
            const result = await page.evaluate(() => window.runBankEditorChecks());
            assert.deepEqual(result.failures, []);
            assert.deepEqual(errors, []);
            results.push({name, ...result});
            console.log(`PASS ${name}`);
        } catch (error) {
            results.push({name, failures: [error.message], errors});
            console.error(`FAIL ${name}: ${error.message}`); process.exitCode = 1;
        } finally { await page.screenshot({path: resolve(output, `${name}.png`)}); await page.close(); }
    }
    // Optional native validation uses the same checks and the same owned Vite server.
    if (process.env.BANK_EDITOR_WEBKIT === '1') {
        const child = spawn('/usr/bin/python3', ['tools/sample-editor-webkit.py', base, resolve(output, 'webkit'),
            '--zoom', '1', '--fixture', '/tools/layout-fixtures/bank-editor.html',
            '--checks', 'tools/bank-editor-checks.js', '--checks-function', 'runBankEditorChecks'],
            {stdio: 'inherit', detached: true});
        const stop = () => { try { process.kill(-child.pid, 'SIGTERM'); } catch (error) { if (error.code !== 'ESRCH') throw error; } };
        const timeout = setTimeout(stop, 150000);
        try {
            const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('exit', resolve); });
            if (code !== 0) process.exitCode = 1;
        } finally { clearTimeout(timeout); stop(); }
    }
} finally {
    await browser?.close();
    await server.close();
    await writeFile(resolve(output, 'results.json'), JSON.stringify(results, null, 2) + '\n');
}
