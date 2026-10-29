import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createWriteStream } from 'node:fs';
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer as socketServer } from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';
import { openSelect } from './select-control.mjs';

assert.equal(process.argv.length, 5, 'Usage: node tools/native-sampler-smoke.mjs SERVER YAMAHA_CORPUS OUTPUT');
const [executable, corpus, output] = process.argv.slice(2).map((path) => resolve(path));
const inputs = [
    join(corpus, 'axk/floppy/a4k-demos/a4k-demo-disk01.ima'),
    join(corpus, 'tx16w/sd/yamaha-format/sd003.img'),
    join(corpus, 'tx16w/sd/yamaha-format/sd004.img'),
];
const digest = async (path) =>
    createHash('sha256')
        .update(await readFile(path))
        .digest('hex');
const before = await Promise.all(inputs.map(digest));
await mkdir(output, { recursive: true });
const temporary = await mkdtemp(join(tmpdir(), 'axklib-native-ui-'));
await chmod(temporary, 0o700);
const log = createWriteStream(join(output, 'server.log'));
const results = {
    server: executable,
    serverSha256: await digest(executable),
    sources: inputs,
    sourceSha256: before,
    checks: [],
};
let vite,
    native,
    nativeExit,
    browser,
    page,
    connection,
    port,
    expired = false;
const errors = [];
const responses = [];

async function released(value) {
    await new Promise((resolve, reject) => {
        const socket = socketServer();
        socket.once('error', reject);
        socket.listen(value, '127.0.0.1', () => socket.close((error) => (error ? reject(error) : resolve())));
    });
}

async function stop() {
    const closed = await Promise.allSettled([browser?.close(), vite?.close()]);
    if (native && native.exitCode === null && native.signalCode === null) {
        native.kill('SIGTERM');
        const timer = setTimeout(() => native.kill('SIGKILL'), 5000);
        try {
            await nativeExit;
        } finally {
            clearTimeout(timer);
        }
    }
    const failed = closed.find((result) => result.status === 'rejected');
    if (failed) throw failed.reason;
}
const interrupt = () => {
    expired = true;
    void stop().catch((error) => {
        results.cleanupFailure = String(error);
    });
};
process.once('SIGTERM', interrupt);
process.once('SIGINT', interrupt);
const deadline = setTimeout(interrupt, 180000);

try {
    vite = await createServer({ server: { host: '127.0.0.1', port: 0, strictPort: true } });
    await vite.listen();
    port = vite.httpServer.address().port;
    const origin = `http://127.0.0.1:${port}`;
    const state = join(temporary, 'state');
    await mkdir(state, { mode: 0o700 });
    const connectionFile = join(state, 'connection.json');
    const store = join(temporary, 'workspaces.json');
    await writeFile(
        store,
        JSON.stringify({
            schemaVersion: 1,
            revision: 1,
            workspaces: [
                { id: 'a-series', displayName: 'A-series qualification', path: dirname(inputs[0]), writable: false },
                { id: 'tx16w', displayName: 'TX16W qualification', path: dirname(inputs[1]), writable: false },
            ],
        }),
    );
    native = spawn(
        executable,
        [
            '--port',
            '0',
            '--workspace-store',
            store,
            '--state-directory',
            state,
            '--connection-file',
            connectionFile,
            '--allow-origin',
            origin,
            '--workers',
            '2',
            '--job-workers',
            '1',
            '--write-job-workers',
            '1',
        ],
        { stdio: ['ignore', log, log] },
    );
    nativeExit = new Promise((resolve, reject) => {
        native.once('exit', resolve);
        native.once('error', reject);
    });
    // Observe spawn failure immediately, including during connection-file polling.
    let spawnError;
    nativeExit.catch((error) => {
        spawnError = error;
    });
    for (let attempt = 0; attempt < 150; ++attempt) {
        if (spawnError) throw spawnError;
        assert(!expired && native.exitCode === null, 'Native server exited before readiness');
        try {
            connection = JSON.parse(await readFile(connectionFile, 'utf8'));
            break;
        } catch (error) {
            if (error.code !== 'ENOENT') throw error;
        }
        await sleep(100);
    }
    assert(connection, 'Native server did not become ready');
    const { chromium } = await import(
        process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
    );
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    page = await browser.newPage({ viewport: { width: 1366, height: 900 } });
    page.setDefaultTimeout(15000);
    page.on('pageerror', (error) => errors.push(error.message));
    page.on('response', (response) => {
        const url = new URL(response.url());
        if (url.origin === new URL(connection.baseUrl).origin)
            responses.push({ method: response.request().method(), path: url.pathname, status: response.status() });
    });
    await page.addInitScript((value) => {
        window.__AXKLIB_SERVER__ = { ...value, mode: 'remote' };
    }, connection);
    await page.goto(origin);
    await page.getByRole('button', { name: 'I understand', exact: true }).click();

    async function selectSources(selections) {
        await page.getByRole('button', { name: 'Open', exact: true }).click();
        const dialog = page.getByRole('dialog', { name: 'Open', exact: true });
        for (const [workspace, names] of selections) {
            await dialog.locator('[aria-label="Storage entries"][aria-busy="false"]').waitFor();
            const home = dialog.getByRole('button', { name: 'Go to all workspaces' });
            if (await home.isEnabled()) await home.click();
            await dialog.getByRole('option', { name: new RegExp(`^${workspace} qualification`) }).click();
            for (const name of names)
                await dialog
                    .getByRole('option')
                    .filter({ has: page.locator('strong', { hasText: name }) })
                    .click();
        }
        const count = selections.reduce((total, [, names]) => total + names.length, 0);
        assert.equal(await dialog.getByRole('region', { name: 'Source set' }).getByRole('listitem').count(), count);
        await dialog.getByRole('button', { name: 'Open', exact: true }).click();
        await dialog.waitFor({ state: 'detached' });
        await page.getByRole('combobox', { name: 'Image', exact: true }).waitFor();
    }

    await selectSources([
        ['A-series', ['a4k-demo-disk01.ima']],
        ['TX16W', ['sd003.img', 'sd004.img']],
    ]);
    const selector = page.getByRole('combobox', { name: 'Image', exact: true });
    const popup = await openSelect(selector);
    assert.deepEqual(
        (await popup.getByRole('option').allTextContents()).map((text) => text.trim()),
        ['a4k-demo-disk01.ima', 'sd003.img', 'sd004.img'],
    );
    await popup.getByRole('option', { name: 'sd003.img', exact: true }).click();
    const tabs = page.getByRole('navigation', { name: 'Workspace views' });
    assert.deepEqual(
        await tabs.getByRole('button').evaluateAll((nodes) => nodes.map((node) => node.getAttribute('aria-label'))),
        ['Performances', 'Voices', 'Timbres', 'Waves', 'Setups'],
    );
    await page
        .getByRole('region', { name: 'TX16W native objects' })
        .locator('[data-collection-object-id]')
        .first()
        .waitFor();
    await page.screenshot({ path: join(output, 'tx16w-open.png') });
    results.checks.push('Production Open and mixed A-series/TX16W sources');

    const objects = page.getByRole('region', { name: 'TX16W native objects' });
    const inspector = page.getByRole('complementary', { name: 'TX16W inspector' });
    for (const label of ['Performances', 'Voices', 'Timbres', 'Setups']) {
        await tabs.getByRole('button', { name: label, exact: true }).click();
        const first = objects.locator('[data-collection-object-id]').first();
        await first.waitFor();
        const name = await first.locator('strong').textContent();
        await first.press('Enter');
        await inspector.getByRole('heading', { name, exact: true }).waitFor();
        assert.equal(await first.getAttribute('aria-pressed'), 'true');
        assert.equal(await inspector.getByRole('textbox').count(), 0, 'Native details must remain read-only');
    }
    results.checks.push('All four non-wave native collections and typed read-only details');

    async function selectImage(name) {
        const options = await openSelect(selector);
        await options.getByRole('option', { name, exact: true }).click();
        await page.waitForFunction(() => !document.querySelector('[aria-label="Image"]').disabled);
    }
    await tabs.getByRole('button', { name: 'Voices', exact: true }).click();
    const voice = objects.locator('[data-collection-object-id]').first();
    const voiceName = await voice.locator('strong').textContent();
    await voice.press('Enter');
    await inspector.getByRole('heading', { name: voiceName, exact: true }).waitFor();
    await page.getByRole('searchbox', { name: 'Search Voices' }).fill(voiceName);
    await selectImage('a4k-demo-disk01.ima');
    await tabs.getByRole('button', { name: 'Programs', exact: true }).waitFor();
    assert.equal(await inspector.count(), 0);
    await selectImage('sd004.img');
    await inspector.getByRole('heading', { name: voiceName, exact: true }).waitFor();
    assert.equal(await page.getByRole('searchbox', { name: 'Search Voices' }).inputValue(), voiceName);
    await selectImage('sd003.img');
    await inspector.getByRole('heading', { name: voiceName, exact: true }).waitFor();
    assert.equal(await page.getByRole('searchbox', { name: 'Search Voices' }).inputValue(), voiceName);
    results.checks.push('A-series/TX16W switching and shared companion document state');

    await tabs.getByRole('button', { name: 'Waves', exact: true }).click();
    const wave = objects.getByRole('group', { name: / Wave Data$/ }).first();
    const waveName = (await wave.getAttribute('aria-label')).replace(/ Wave Data$/, '');
    await wave.getByRole('button', { name: `Inspect ${waveName}`, exact: true }).press('Enter');
    await inspector.getByRole('heading', { name: waveName, exact: true }).waitFor();
    await page.waitForFunction(() => {
        const canvas = document.querySelector('.wave-data-row canvas');
        if (!canvas || !canvas.width || !canvas.height) return false;
        const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
        let painted = 0;
        for (let offset = 3; offset < pixels.length; offset += 4) if (pixels[offset]) ++painted;
        return painted > canvas.width * 2;
    });
    assert.match(await wave.textContent(), /12-bit/);
    await inspector.getByRole('button', { name: 'To Files', exact: true }).click();
    const selectedFile = page
        .getByRole('treegrid', { name: 'Filesystem entries' })
        .locator('[data-file-entry][aria-selected="true"]');
    await selectedFile.waitFor();
    assert.equal(await selectedFile.locator('.file-name span[title]').textContent(), waveName);
    await page.getByRole('button', { name: 'Device', exact: true }).click();
    await inspector.getByRole('heading', { name: waveName, exact: true }).waitFor();
    await page.screenshot({ path: join(output, 'tx16w-wave.png') });
    results.checks.push('Real native PCM waveform pixels and exact Show in Files binding');
    assert.equal(
        responses.filter((response) => response.method === 'POST' && /\/auditions$/.test(response.path)).length,
        0,
        'Inspecting a Wave must not start audition',
    );

    const [audioResponse] = await Promise.all([
        page.waitForResponse(
            (response) =>
                new URL(response.url()).pathname.match(/\/auditions\/[^/]+\/content$/) && response.status() === 206,
        ),
        wave.getByRole('button', { name: `Play ${waveName}`, exact: true }).click(),
    ]);
    assert((await audioResponse.body()).length > 44, 'Audition must deliver PCM, not only a WAV header');
    await wave.locator('button[title="Stop"]').waitFor();
    await wave.getByRole('button', { name: `Seek ${waveName}`, exact: true }).click();
    await wave.getByRole('button', { name: `Stop ${waveName}`, exact: true }).click();
    await wave.getByRole('button', { name: `Play ${waveName}`, exact: true }).waitFor();
    await wave.getByRole('button', { name: `Play ${waveName}`, exact: true }).click();
    await wave.locator('button[title="Stop"]').waitFor();
    await selectImage('a4k-demo-disk01.ima');
    await tabs.getByRole('button', { name: 'Programs', exact: true }).waitFor();
    await selectImage('sd003.img');
    await wave.getByRole('button', { name: `Play ${waveName}`, exact: true }).waitFor();
    results.checks.push('Real audition range/decode, Play/Stop/Seek and no restart after switching');
    await page.getByRole('button', { name: 'Eject all images', exact: true }).click();
    await selector.waitFor({ state: 'detached' });
    await selectSources([['TX16W', ['sd004.img', 'sd003.img']]]);
    await tabs.getByRole('button', { name: 'Performances', exact: true }).waitFor();
    await objects.locator('[data-collection-object-id]').first().waitFor();
    assert.equal(await objects.locator('[data-collection-object-id]').count(), 32);
    await page.getByRole('button', { name: 'Eject all images', exact: true }).click();
    await selector.waitFor({ state: 'detached' });
    results.checks.push('Whole-set close and reversed-order reopen retain all Performance slots');
    assert.deepEqual(errors, []);
    assert(
        responses.some(
            (response) => response.method === 'GET' && /\/jobs\//.test(response.path) && response.status === 200,
        ),
        'Production Open must retrieve completed job results',
    );
    assert.deepEqual(
        responses.filter((response) => response.status >= 400),
        [],
    );
    assert(!expired, 'Qualification exceeded its deadline');
} catch (error) {
    results.failure = String(error);
    if (page && !page.isClosed()) {
        await page.screenshot({ path: join(output, 'failure.png') }).catch(() => {});
        await writeFile(join(output, 'failure.html'), await page.content()).catch(() => {});
    }
    throw error;
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', interrupt);
    process.removeListener('SIGINT', interrupt);
    await stop();
    log.end();
    results.browserStopped = !browser?.isConnected();
    results.nativeStopped = !native || native.exitCode !== null || native.signalCode !== null;
    if (port) await released(port);
    if (connection) await released(Number(new URL(connection.baseUrl).port));
    results.portsReleased = true;
    results.sourcesUnchanged = JSON.stringify(before) === JSON.stringify(await Promise.all(inputs.map(digest)));
    results.responses = responses;
    results.pageErrors = errors;
    await rm(temporary, { recursive: true, force: true });
    await writeFile(join(output, 'results.json'), `${JSON.stringify(results, null, 2)}\n`);
    assert(results.sourcesUnchanged, 'Read-only corpus bytes changed');
}
