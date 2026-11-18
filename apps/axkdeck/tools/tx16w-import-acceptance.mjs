import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { constants, createReadStream } from 'node:fs';
import { copyFile, mkdir, mkdtemp, open, readFile, rm, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as sleep } from 'node:timers/promises';
import { createServer as socketServer } from 'node:net';
import { createServer } from 'vite';

const [outputArg, targetArg, corpusArg, placement] = process.argv.slice(2);
assert(outputArg && targetArg && corpusArg && (!placement || placement === '--new-volume'), 'Usage: node tools/tx16w-import-acceptance.mjs OUTPUT ORIGINAL_TARGET SOURCE_FOLDER [--new-volume]');
const createVolume = placement === '--new-volume', newVolumeName = 'SDYE Focus';
const output = resolve(outputArg), original = resolve(targetArg), corpus = resolve(corpusArg);
const repo = resolve('../../..'), names = ['sd001ye.img', 'sd002ye.img'];
await mkdir(output, { recursive: true });
const scratchRoot = resolve(repo, 'build/tmp/tx16w-import-fixes');
await mkdir(scratchRoot, { recursive: true });
const scratch = await mkdtemp(`${scratchRoot}/http-`);
const guitarPair = ['sd023ye.img', 'sd024ye.img'];
const report = { original, sourcePaths: [...names, ...guitarPair].map(name => resolve(corpus, name)), cases: [] };
async function hash(path) { const h = createHash('sha256'); for await (const chunk of createReadStream(path)) h.update(chunk); return h.digest('hex'); }
report.originalSha256 = await hash(original);
report.sourceSha256 = [];
for (const path of report.sourcePaths) report.sourceSha256.push(await hash(path));
for (const folder of ['state', 'workspace', 'pair', 'tie']) await mkdir(`${scratch}/${folder}`, { mode: 0o700 });
await copyFile(original, `${scratch}/workspace/target.hds`, constants.COPYFILE_FICLONE);
for (const name of names) await copyFile(`${corpus}/${name}`, `${scratch}/pair/${name}`);
for (const name of names) await copyFile(`${corpus}/${name}`, `${scratch}/tie/${name}`);
await copyFile(`${corpus}/${names[1]}`, `${scratch}/tie/another-supplier.img`);
await writeFile(`${scratch}/workspaces.json`, JSON.stringify({ schemaVersion: 1, revision: 1, workspaces: [
    { id: 'corpus', displayName: 'Read-only original corpus', path: corpus, writable: false },
    { id: 'pair', displayName: 'Read-only isolated pair', path: `${scratch}/pair`, writable: false },
    { id: 'tie', displayName: 'Two eligible suppliers', path: `${scratch}/tie`, writable: false },
    { id: 'target', displayName: 'Disposable target', path: `${scratch}/workspace`, writable: true },
] }));
const stdout = await open(`${output}/server.log`, 'w'), stderr = await open(`${output}/server-stderr.log`, 'w');
const freePort = await new Promise((done, reject) => { const socket = socketServer(); socket.once('error', reject); socket.listen(0, '127.0.0.1', () => { const port = socket.address().port; socket.close(error => error ? reject(error) : done(port)); }); });
const child = spawn('prlimit', ['--as=3221225472', '--cpu=120', '--', resolve(repo, 'axklib/build/native/release/apps/server/axklib-server'), '--port', '0', '--allow-origin', `http://127.0.0.1:${freePort}`, '--workspace-store', `${scratch}/workspaces.json`, '--state-directory', `${scratch}/state`, '--connection-file', `${scratch}/state/connection.json`, '--workers', '2', '--job-workers', '1', '--write-job-workers', '1'], { stdio: ['ignore', stdout.fd, stderr.fd], detached: true });
await stdout.close(); await stderr.close();
const childExit = new Promise((done, reject) => { child.once('error', reject); child.once('exit', (code, signal) => done({ code, signal })); });
const vite = await createServer({ server: { host: '127.0.0.1', port: freePort, strictPort: true, hmr: false, watch: null } });
let browser, page, connection, vitePort, sequence = 0, stopping;
const ports = [];
async function api(method, path, body) {
    const response = await fetch(connection.baseUrl + path, { method, signal: AbortSignal.timeout(150000), headers: { Authorization: `Bearer ${connection.bearerToken}`, 'Content-Type': 'application/json', 'Idempotency-Key': `tx16w-ui-${++sequence}-${randomUUID()}` }, body: body === undefined ? undefined : JSON.stringify(body) });
    const envelope = await response.json();
    assert(response.ok, `${method} ${path}: ${response.status} ${JSON.stringify(envelope)}`);
    return envelope.data;
}
async function job(path, body) {
    const start = await api('POST', path, body), until = Date.now() + 150000;
    while (Date.now() < until) {
        const state = await api('GET', `/jobs/${start.jobId}`);
        if (['COMPLETED', 'FAILED', 'CANCELLED'].includes(state.state)) { assert.equal(state.state, 'COMPLETED', JSON.stringify(state)); return state.result; }
        await sleep(50);
    }
    throw new Error(`Job timed out: ${start.jobId}`);
}
async function countObjects(image, enumerate = false, visit = () => {}) {
    const document = image.documents.find(item => item.formatId === 'yamaha.a-series');
    assert(document);
    let cursor, count = 0, total;
    do {
        const result = await api('GET', `/images/${image.imageId}/documents/${document.id}/objects?limit=${enumerate ? 200 : 1}${cursor ? `&cursor=${encodeURIComponent(cursor)}` : ''}`);
        result.items.forEach(visit);
        total = result.totalCount; count += result.items.length; cursor = enumerate && result.nextCursor;
    } while (cursor);
    if (enumerate) assert.equal(count, total);
    return total;
}
async function volumeList(image) {
    const doc = image.documents.find(item => item.formatId === 'yamaha.a-series');
    const volumes = [], queue = [''];
    for (let visited = 0; queue.length; visited++) {
        assert(visited < 256, 'Volume traversal exceeds its bound');
        const parentId = queue.shift(), query = new URLSearchParams({limit:'200'});
        if (parentId) query.set('parentId', parentId);
        do {
            const page = await api('GET', `/images/${image.imageId}/documents/${doc.id}/content?${query}`);
            for (const node of page.items) {
                if (node.kind.toLowerCase() === 'volume') volumes.push(JSON.stringify([node.partitionIndex, node.name ?? node.displayName]));
                else if (node.childCount) queue.push(node.id);
            }
            if (page.nextCursor) query.set('cursor', page.nextCursor); else break;
        } while (true);
    }
    return volumes.sort();
}
function objectIdentity(item) {
    return JSON.stringify([item.partitionIndex, item.volumeName, item.type, item.entryName, item.name, item.sizeBytes]);
}
function stop() {
    return stopping ??= (async () => {
        try { if (page && !page.isClosed()) await page.evaluate(() => window.tx16wAcceptance?.shutdown()).catch(() => {}); }
        finally { await browser?.close(); await vite.close(); }
        if (connection && child.exitCode === null) await api('POST', '/system/shutdown', {}).catch(() => {});
        if (await Promise.race([childExit, sleep(15000).then(() => null)]) === null) process.kill(-child.pid, 'SIGTERM');
        report.serverExit = await childExit;
    })();
}
const interrupt = () => { process.exitCode = 1; void stop(); };
process.once('SIGINT', interrupt); process.once('SIGTERM', interrupt);
try {
    for (let attempt = 0; attempt < 400; attempt++) {
        try { connection = JSON.parse(await readFile(`${scratch}/state/connection.json`, 'utf8')); break; }
        catch (error) { if (error.code !== 'ENOENT') throw error; assert.equal(child.exitCode, null); await sleep(25); }
    }
    assert(connection, 'Server connection unavailable');
    ports.push(Number(new URL(connection.baseUrl).port));
    report.serverVersion = await api('GET', '/system/version');
    const before = await job('/images', { sources: [{ kind: 'FILE', file: { rootId: 'target', relativePath: 'target.hds' } }] });
    const oldObjects = [], oldVolumes = createVolume ? await volumeList(before) : [];
    report.beforeObjectCount = await countObjects(before, createVolume, item => oldObjects.push(objectIdentity(item)));
    const newVolume = JSON.stringify([0, newVolumeName]);
    if (createVolume) assert(!oldVolumes.includes(newVolume));
    await api('DELETE', `/images/${before.imageId}`);
    await vite.listen(); vitePort = vite.httpServer.address().port; ports.push(vitePort);
    const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    page = await browser.newPage({ viewport: { width: 1152, height: 820 } });
    page.setDefaultTimeout(30000);
    const errors = []; page.on('pageerror', error => errors.push(error.message));
    await page.addInitScript(value => { window.tx16wConnection = value; }, connection);
    await page.goto(`http://127.0.0.1:${vitePort}/tools/layout-fixtures/tx16w-http.html`);
    await page.waitForFunction(() => !!window.tx16wAcceptance);
    const begin = (rootId, selected) => page.evaluate(({ rootId, selected }) => window.tx16wAcceptance.begin(rootId, selected), { rootId, selected });
    const snapshot = () => page.evaluate(() => window.tx16wAcceptance.snapshot());
    async function editPlacement(dialog) {
        const mode = dialog.getByRole('group', { name: 'Destination volume type' });
        await mode.getByRole('button', { name: 'New' }).click();
        const field = dialog.getByRole('textbox', { name: 'New volume name' });
        await field.fill('test05');
        await dialog.getByText('Name already exists in this partition.').waitFor();
        assert(await dialog.getByRole('button', { name: 'Create volume and import' }).isDisabled());
        await field.fill(newVolumeName);
        await field.evaluate(input => input.setSelectionRange(4, 4));
        await page.waitForFunction(() => window.tx16wAcceptance.snapshot().request?.report?.executable);
        assert.deepEqual(await field.evaluate(input => [document.activeElement === input, input.selectionStart, input.selectionEnd]), [true, 4, 4]);
        assert(await dialog.getByRole('button', { name: 'Create volume and import' }).isEnabled());
        await page.screenshot({ path: `${output}/new-volume-${await dialog.getAttribute('aria-label')}.png` });
        await mode.getByRole('button', { name: 'Existing' }).click();
        assert.equal((await snapshot()).request.target.volumeName, 'test05');
    }
    await page.evaluate((names) => window.tx16wAcceptance.beginNative('pair', names), names);
    const conversion = page.getByRole('dialog', {name:'Convert'});
    await conversion.waitFor();
    await conversion.getByText(/sd001ye\.img.*sd002ye\.img|sd002ye\.img.*sd001ye\.img/i).waitFor();
    assert(await conversion.getByRole('button', {name:'Add to volume'}).isDisabled());
    await conversion.getByRole('button', {name:/Browse/}).click();
    await page.waitForFunction(() => !!window.tx16wAcceptance.snapshot().request?.destination);
    const partition = conversion.getByRole('combobox', {name:'Destination partition'});
    assert(await partition.isEnabled(), 'Browsing an existing image must populate the partition selector');
    await partition.click();
    await page.getByRole('option').first().click();
    const volume = conversion.getByRole('combobox', {name:'Destination volume', exact:true});
    await volume.click();
    await volume.fill('test05');
    await page.getByRole('option', {name:/test05/i}).click();
    await page.waitForFunction(() => window.tx16wAcceptance.snapshot().request?.report?.executable);
    assert(await conversion.getByRole('button', {name:'Add to volume'}).isEnabled());
    const nativePlan = await snapshot();
    assert.equal(nativePlan.request.report.eligible, 32);
    assert.equal(nativePlan.request.report.actionCount, 157);
    report.cases.push({ label: 'Opened TX16W document planned into an existing image after explicit Browse', ...nativePlan });
    await page.screenshot({path:`${output}/convert-open-document.png`});
    await editPlacement(conversion);
    await begin('corpus', [names[0]]);
    const library = await snapshot(); report.cases.push({ label: 'Original multi-set directory resolves the selected Setup', ...library });
    assert.equal(library.request.discovery.state, 'complete');
    assert.deepEqual(library.request.members, [{ name: names[0], origin: 'SELECTED' }, { name: names[1], origin: 'AUTO_ADDED' }]);
    assert.equal(library.request.report.counts.BLOCKER, 0);
    await begin('tie', [names[0]]);
    const ambiguous = await snapshot(); report.cases.push({ label: 'Two actual suppliers retain ambiguity', ...ambiguous });
    assert.equal(ambiguous.request.discovery.state, 'ambiguous');
    assert.equal(ambiguous.request.members.length, 1);
    assert(await page.getByRole('button', { name: 'Import', exact: true }).isDisabled());
    await begin('corpus', names);
    const explicit = await snapshot(); report.cases.push({ label: 'Explicit original pair prepares without blockers', ...explicit });
    assert.equal(explicit.request.report.counts.BLOCKER, 0);
    assert.equal(explicit.request.members.length, 2);
    await editPlacement(page.getByRole('dialog', { name: 'Import TX16W disk set' }));
    await page.waitForFunction(() => window.tx16wAcceptance.snapshot().request?.report?.executable);
    const contents = page.locator('[data-pane="contents"]');
    assert(explicit.request.report.eligible >= 1);
    await contents.getByRole('checkbox', { name: /All sources/ }).uncheck();
    await page.waitForFunction(() => { const request = window.tx16wAcceptance.snapshot().request; return request?.status === 'ready' && request.selected?.length === 0 && !request.report; });
    assert(await page.getByRole('button', { name: 'Import', exact: true }).isDisabled());
    await contents.getByRole('checkbox').nth(1).check();
    await page.waitForFunction(() => { const request = window.tx16wAcceptance.snapshot().request; return request?.report && request.selected?.length === 1 && request.status === 'ready'; });
    const subset = await snapshot();
    assert(subset.request.report.actionCount > 0 && subset.request.report.actionCount <= explicit.request.report.actionCount);
    report.cases.push({ label: 'One selected occurrence and required dependencies are replanned', ...subset });
    await begin('pair', [names[0]]);
    const discovered = await snapshot(); report.cases.push({ label: 'Remote FILE seed discovers same-folder companion', ...discovered });
    assert.equal(discovered.request.discovery.state, 'complete');
    assert.deepEqual(discovered.request.members, [{ name: names[0], origin: 'SELECTED' }, { name: names[1], origin: 'AUTO_ADDED' }]);
    assert.equal(discovered.request.report.counts.BLOCKER, 0);
    assert.equal(discovered.request.report.counts.LOSS, 64);
    assert.equal(discovered.request.report.counts.UNCERTAINTY, 79);
    await page.screenshot({ path: `${output}/companion-found.png` });
    assert.equal(await page.getByRole('checkbox', { name: 'Accept all losses' }).count(), 0);
    assert.deepEqual(discovered.request.report.acknowledged, []);
    const primary = page.getByRole('button', { name: 'Import', exact: true });
    const decisions = discovered.request.report.decisions;
    assert.equal(await primary.isDisabled(), decisions.length > 0);
    if (decisions.length) {
        do {
            for (const item of await page.locator('[data-pane="review"]').getByRole('checkbox').all()) await item.check();
            const next = page.getByRole('button', { name: 'Next mapping details page', exact: true });
            if (await next.isDisabled()) break;
            await next.click();
        } while (true);
    }
    assert.deepEqual((await snapshot()).request.report.acknowledged.toSorted(), decisions.toSorted());
    assert(await primary.isEnabled());
    await page.screenshot({ path: `${output}/ready-with-conversion-notes.png` });
    await begin('corpus', [guitarPair[0]]);
    const guitars = await snapshot();
    assert.deepEqual(guitars.request.members, [{ name: guitarPair[0], origin: 'SELECTED' }, { name: guitarPair[1], origin: 'AUTO_ADDED' }]);
    assert.equal(guitars.request.report.eligible, 32);
    assert.equal(guitars.request.report.counts.BLOCKER, 0);
    assert.equal(guitars.request.report.actionCount, 113);
    assert.deepEqual(guitars.request.report.decisions, []);
    assert(await primary.isEnabled());
    report.cases.push({ label: 'Electric Guitars keeps all 32 Performances with region losses', ...guitars });
    await page.screenshot({ path: `${output}/electric-guitars.png` });
    let expectedActionCount = guitars.request.report.actionCount;
    if (createVolume) {
        const dialog = page.getByRole('dialog', { name: 'Import TX16W disk set' });
        await dialog.getByRole('group', { name: 'Destination volume type' }).getByRole('button', { name:'New' }).click();
        await dialog.getByRole('textbox', { name:'New volume name' }).fill(newVolumeName);
        await page.waitForFunction(() => window.tx16wAcceptance.snapshot().request?.report?.executable);
        const ready = await snapshot();
        assert.deepEqual(ready.request.target, {kind:'CREATE_VOLUME', partitionIndex:0, volumeName:newVolumeName});
        expectedActionCount = ready.request.report.actionCount;
        report.cases.push({ label:'Create a new volume on the disposable existing image', ...ready });
        await dialog.getByRole('button', {name:'Create volume and import'}).click();
    } else await page.getByRole('button', { name: 'Import', exact: true }).click();
    await page.waitForFunction(() => { const state = window.tx16wAcceptance.snapshot(); return !state.request || ['warnings', 'refresh-failed', 'unconfirmed', 'failed'].includes(state.phase); }, null, { timeout: 150000 });
    report.completion = await snapshot();
    assert.equal(report.completion.request, null, JSON.stringify(report.completion));
    await page.evaluate(() => window.tx16wAcceptance.shutdown());
    const reopened = await job('/images', { sources: [{ kind: 'FILE', file: { rootId: 'target', relativePath: 'target.hds' } }] });
    const remainingOldObjects = [];
    let newObjects = 0;
    report.afterObjectCount = await countObjects(reopened, true, item => {
        if (item.partitionIndex === 0 && item.volumeName === newVolumeName) newObjects++;
        else if (createVolume) remainingOldObjects.push(objectIdentity(item));
    });
    assert.equal(report.afterObjectCount - report.beforeObjectCount, expectedActionCount);
    if (createVolume) {
        assert.deepEqual(await volumeList(reopened), [...oldVolumes, newVolume].sort());
        assert.equal(newObjects, expectedActionCount);
        assert.deepEqual(remainingOldObjects.sort(), oldObjects.sort());
        report.newVolume = { name: newVolumeName, partitionIndex: 0, objects: newObjects, unchangedVolumes: oldVolumes.length, unchangedObjectEntries: oldObjects.length };
    }
    await api('DELETE', `/images/${reopened.imageId}`);
    assert.deepEqual(errors, []); report.browserErrors = errors; report.success = true;
} catch (error) {
    report.error = error.stack;
    report.lastState = await page?.evaluate(() => window.tx16wAcceptance?.snapshot()).catch(() => null);
    process.exitCode = 1;
}
finally {
    await stop();
    process.removeListener('SIGINT', interrupt); process.removeListener('SIGTERM', interrupt);
    report.originalUnchanged = await hash(original) === report.originalSha256;
    report.sourcesUnchanged = true;
    for (const [index, path] of report.sourcePaths.entries()) if (await hash(path) !== report.sourceSha256[index]) report.sourcesUnchanged = false;
    assert(report.originalUnchanged && report.sourcesUnchanged, 'Original inputs changed');
    for (const port of ports) await new Promise((done, reject) => { const socket = socketServer(); socket.once('error', reject); socket.listen(port, '127.0.0.1', () => socket.close(error => error ? reject(error) : done())); });
    report.portsReleased = true;
    await writeFile(`${output}/results.json`, JSON.stringify(report, null, 2) + '\n');
    if (report.success) await rm(scratch, { recursive: true });
    console.log(JSON.stringify({ success: report.success ?? false, error: report.error, before: report.beforeObjectCount, after: report.afterObjectCount, originalUnchanged: report.originalUnchanged, sourcesUnchanged: report.sourcesUnchanged, portsReleased: report.portsReleased }));
}
