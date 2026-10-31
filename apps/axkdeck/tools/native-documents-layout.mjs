import { selectValue } from './select-control.mjs';
import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createServer as socketServer } from 'node:net';
import { pathToFileURL } from 'node:url';
import { createServer } from 'vite';
import { checkNativeCollectionParity } from './native-collection-parity.mjs';

const { chromium } = await import(
    process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright'
);
const output = resolve(process.argv[2]);
await mkdir(output, { recursive: true });
const availablePort = await new Promise((resolve, reject) => {
    const socket = socketServer();
    socket.once('error', reject);
    socket.listen(0, '127.0.0.1', () => {
        const port = socket.address().port;
        socket.close((error) => (error ? reject(error) : resolve(port)));
    });
});
const server = await createServer({ server: { host: '127.0.0.1', port: availablePort, strictPort: true } });
const results = [];
let browser, port, page, failure;
async function stop() {
    await browser?.close();
    await server.close();
}
const signal = () => void stop();
process.once('SIGTERM', signal);
process.once('SIGINT', signal);
const deadline = setTimeout(signal, 240000);
try {
    await server.listen();
    port = server.httpServer.address().port;
    browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
    for (const [width, scale] of [
        [1366, 1],
        [915, 1],
        [640, 1],
        [390, 1],
        [1366, 1.25],
        [1366, 1.5],
        [915, 1.25],
        [915, 1.5],
        [640, 1.25],
        [640, 1.5],
    ]) {
        const logicalWidth = Math.ceil(width / scale);
        page = await browser.newPage({
            viewport: { width: logicalWidth, height: Math.ceil(844 / scale) },
            deviceScaleFactor: scale,
        });
        page.setDefaultTimeout(15000);
        const errors = [];
        page.on('pageerror', (error) => errors.push(error.message));
        await page.goto(`http://127.0.0.1:${port}/tools/layout-fixtures/native-documents.html`);
        const dialog = page.getByRole('dialog', { name: 'Open', exact: true });
        await dialog.waitFor();
        const before = await dialog.boundingBox();
        await dialog.getByRole('option', { name: /Yamaha images/ }).click();
        await dialog.getByRole('option', { name: /^Disk1\.hds/ }).click();
        await dialog.getByRole('option', { name: /^Disk2\.hds/ }).click();
        assert.equal(await dialog.getByRole('region', { name: 'Source set' }).getByRole('listitem').count(), 2);
        await dialog.getByRole('button', { name: /Remove .*Disk1.hds/ }).click();
        assert.equal(await dialog.getByRole('option', { name: /^Disk1\.hds/ }).getAttribute('aria-selected'), 'false');
        await dialog.getByRole('option', { name: /^Disk1\.hds/ }).click();
        await dialog.getByRole('button', { name: 'Load more', exact: true }).click();
        await dialog.getByRole('option', { name: /^Disk90\.hds/ }).click();
        assert.equal(await dialog.getByRole('region', { name: 'Source set' }).getByRole('listitem').count(), 3);
        await dialog.getByRole('button', { name: 'Add current folder' }).click();
        await dialog.getByRole('button', { name: 'Remove current folder' }).click();
        assert.equal(await dialog.locator('.open-source-panel:not([hidden]) .dialog-footer').count(), 1);
        assert.equal(await dialog.getByRole('button', { name: /^Add \d/ }).count(), 0);
        assert.deepEqual(await dialog.boundingBox(), before, 'Selection must not resize the dialog');
        const footer = dialog.locator('.open-source-panel:not([hidden]) > footer');
        const geometry = await footer.locator('button').evaluateAll((buttons) =>
            buttons.map((button) => ({
                height: button.getBoundingClientRect().height,
                top: button.getBoundingClientRect().top,
                marginTop: getComputedStyle(button).marginTop,
                marginBottom: getComputedStyle(button).marginBottom,
            })),
        );
        assert(geometry.length >= 2);
        geometry.forEach((item) => assert.deepEqual(item, geometry[0]));
        assert.equal(geometry[0].marginTop, '0px');
        assert.equal(geometry[0].marginBottom, '0px');
        await page.screenshot({ path: resolve(output, `sources-${width}-${scale}.png`) });
        await dialog.getByRole('button', { name: 'Open', exact: true }).click();
        await dialog.waitFor({ state: 'detached' });
        const selector = page.getByRole('combobox', { name: 'Image', exact: true });
        await selector.waitFor({ state: 'visible' });
        assert.equal(await selector.evaluate((node) => getComputedStyle(node).colorScheme), 'dark');
        const tabs = page.getByRole('navigation', { name: 'Workspace views' });
        assert.deepEqual(
            await tabs.locator('button').evaluateAll((nodes) => nodes.map((node) => node.getAttribute('aria-label'))),
            ['Performances', 'Voices', 'Timbres', 'Waves', 'Setups'],
        );
        assert.equal(
            await tabs.getByRole('button', { name: 'Performances', exact: true }).getAttribute('aria-pressed'),
            'true',
        );
        const root = page
            .getByRole('navigation', { name: 'TX16W structure' })
            .getByRole('button', { name: '/', exact: true });
        assert.equal(await page.getByRole('button', { name: /All objects/i }).count(), 0);
        if (await root.isVisible()) {
            const structure = page.getByRole('navigation', { name: 'TX16W structure' });
            const structureSearch = structure.getByRole('searchbox', { name: 'Search TX16W structure' });
            const navigationGeometry = async (section) =>
                section.evaluate((node) => {
                    const base = node.getBoundingClientRect();
                    return ['.panel-heading', '.eyebrow', 'h2', '.search-field'].map((selector) => {
                        const element = node.querySelector(selector);
                        const bounds = element.getBoundingClientRect(),
                            style = getComputedStyle(element);
                        return {
                            left: bounds.left - base.left,
                            top: bounds.top - base.top,
                            height: bounds.height,
                            fontSize: style.fontSize,
                            fontWeight: style.fontWeight,
                            color: style.color,
                        };
                    });
                });
            const txGeometry = await navigationGeometry(structure);
            const objectRows = await page
                .getByRole('region', { name: 'TX16W native objects' })
                .locator('[data-collection-object-id]')
                .count();
            await structureSearch.fill('no matching structure');
            assert.equal(await root.count(), 0);
            assert.equal(
                await page
                    .getByRole('region', { name: 'TX16W native objects' })
                    .locator('[data-collection-object-id]')
                    .count(),
                objectRows,
            );
            await structureSearch.fill('/');
            await selectValue(selector, 'source-1');
            const aSeries = page.getByRole('region', { name: 'Image contents', exact: true });
            await aSeries.getByRole('searchbox').waitFor();
            assert.deepEqual(
                await navigationGeometry(aSeries),
                txGeometry,
                'Sampler headers and Search must share geometry and typography',
            );
            await selectValue(selector, 'source-2');
            await structureSearch.waitFor();
            assert.equal(await structureSearch.inputValue(), '/', 'Structure query survives source switching');
            await structureSearch.fill('');
            assert.equal(await root.evaluate(node => getComputedStyle(node).backgroundColor), 'rgb(41, 73, 88)', 'Structural selection uses the canonical opaque selection surface');
            assert.equal(await root.evaluate(node => getComputedStyle(node).backgroundImage), 'none');
            await root.click({ button: 'right' });
            assert.equal(await page.getByRole('menuitem', { name: 'Add companion images...', exact: true }).count(), 0);
            await page.getByRole('menuitem', { name: 'Convert...', exact: true }).click();
            assert.equal(await page.getByLabel('Conversion requests').textContent(), '1');
            await root.focus();
            await page.keyboard.press('Shift+F10');
            await page.getByRole('menuitem', { name: 'Convert...', exact: true }).waitFor();
            await page.keyboard.press('Escape');
            assert(await root.evaluate((node) => node === document.activeElement));
        }
        const collectionParity = await checkNativeCollectionParity(page, output, width, scale);
        await tabs.getByRole('button', { name: 'Waves', exact: true }).click();
        const waveRow = page.getByRole('group', { name: 'Wave 20 Wave Data', exact: true });
        await waveRow.scrollIntoViewIfNeeded();
        const waveSelect = waveRow.getByRole('button', { name: 'Inspect Wave 20', exact: true });
        await waveSelect.click({ position: { x: 3, y: 3 } });
        assert.equal(await waveSelect.getAttribute('aria-pressed'), 'true', 'Blank row padding selects the wave');
        const waveGeometry = await waveRow.evaluate((node) => {
            const style = getComputedStyle(node),
                bounds = node.getBoundingClientRect();
            const overlay = node.querySelector('.wave-data-selection').getBoundingClientRect();
            const waveform = node.querySelector('.waveform-seek').getBoundingClientRect();
            const playback = node.querySelector('.wave-data-playback').getBoundingClientRect();
            return {
                display: style.display,
                height: bounds.height,
                minHeight: style.minHeight,
                identityFont: getComputedStyle(node.querySelector('.wave-data-identity')).fontSize,
                metadataFont: getComputedStyle(node.querySelector('.wave-data-meta')).fontSize,
                selectionBackground: style.backgroundColor,
                selectionImage: style.backgroundImage,
                overlayWidth: overlay.width,
                width: bounds.width,
                containerWidth: node.closest('.wave-data-list').getBoundingClientRect().width,
                waveformWidth: waveform.width,
                waveformHeight: waveform.height,
                playbackHeight: playback.height,
                content: Array.from(
                    node.querySelectorAll(
                        '.wave-data-identity, .wave-data-meta, .wave-data-format, .waveform-seek, .wave-data-playback',
                    ),
                ).map((element) => {
                    const child = element.getBoundingClientRect();
                    return {
                        name: element.className,
                        left: child.left - bounds.left,
                        top: child.top - bounds.top,
                        right: child.right - bounds.left,
                        bottom: child.bottom - bounds.top,
                        width: child.width,
                        height: child.height,
                    };
                }),
            };
        });
        assert.equal(waveGeometry.display, 'grid');
        const expectedHeight = waveGeometry.width <= 520 ? 72 : 40;
        assert.equal(waveGeometry.height, expectedHeight);
        assert.equal(waveGeometry.minHeight, `${expectedHeight}px`);
        assert.equal(waveGeometry.identityFont, '10px');
        assert.equal(waveGeometry.metadataFont, '8.5px');
        assert.equal(waveGeometry.waveformHeight, 28);
        assert.equal(waveGeometry.playbackHeight, 26);
        assert(waveGeometry.waveformWidth >= 150, 'Native wave preview retains inspectable width');
        assert(Math.abs(waveGeometry.overlayWidth - waveGeometry.width) <= 2);
        assert.equal(waveGeometry.selectionBackground, 'rgb(41, 73, 88)', 'Selected native object uses the canonical selection surface');
        assert.equal(waveGeometry.selectionImage, 'none');
        for (const item of waveGeometry.content) {
            assert(item.width > 0 && item.height > 0, `Wave content remains visible: ${JSON.stringify(item)}`);
            assert(
                item.left >= 0 &&
                    item.top >= 0 &&
                    item.right <= waveGeometry.width &&
                    item.bottom <= waveGeometry.height,
                `Wave content stays inside its row at ${width}/${scale}: ${JSON.stringify({ row: waveGeometry, item })}`,
            );
        }
        await waveRow.getByText(/Approx\..*33,333 Hz/).waitFor();
        assert.match(await waveRow.locator('.wave-data-format').textContent(), /12-bit/);
        assert.match(await waveSelect.getAttribute('title'), /Wave 20\nApprox\..*33,333 Hz\n12-bit/);
        assert(
            await waveRow.evaluate((row) => {
                const bounds = row.querySelector('.wave-data-meta').getBoundingClientRect();
                return (
                    document.elementFromPoint(bounds.left + bounds.width / 2, bounds.top + bounds.height / 2) ===
                    row.querySelector('.wave-data-selection')
                );
            }),
            'Metadata hover reaches the selection overlay tooltip',
        );
        await page.waitForFunction(() => {
            const canvas = document.querySelector('[aria-label="Wave 20 Wave Data"] canvas');
            if (!canvas || canvas.width < 2 || canvas.height < 4) return false;
            const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
            let waveformPixels = 0;
            for (let y = 0; y < canvas.height; y++) {
                if (Math.abs(y - Math.floor(canvas.height / 2)) < 2) continue;
                for (let x = 0; x < canvas.width; x++) if (pixels[(y * canvas.width + x) * 4 + 3]) waveformPixels++;
            }
            return waveformPixels > 10;
        });
        const inspector = page.locator('[aria-label="TX16W inspector"]');
        const inspectorState = logicalWidth > 850 ? 'visible' : 'attached';
        await inspector.getByText('33333', { exact: true }).waitFor({ state: inspectorState });
        const inspectorTypography = await inspector.evaluate((node) => {
            const size = (selector) => getComputedStyle(node.querySelector(selector)).fontSize;
            return {
                section: size('.inspector-section-title'),
                label: size('.metadata-list dt'),
                value: size('.metadata-list dd'),
                relationshipHeading: size('.inspector-relationship-group h5'),
                relationshipLabel: size('.inspector-relationship-group dt'),
                relationshipValue: size('.inspector-relationship-group dd'),
                relationshipAction: size('.inspector-relationship-group button'),
            };
        });
        assert.deepEqual(inspectorTypography, {
            section: '10px',
            label: '8px',
            value: '8.5px',
            relationshipHeading: '8px',
            relationshipLabel: '8px',
            relationshipValue: '8.5px',
            relationshipAction: '8.5px',
        });
        assert.equal(
            await inspector
                .getByRole('button', { name: 'Preview', exact: true, includeHidden: true })
                .getAttribute('aria-expanded'),
            'true',
        );
        assert.equal(
            await inspector
                .getByRole('button', { name: 'Properties', exact: true, includeHidden: true })
                .getAttribute('aria-expanded'),
            'true',
        );
        assert.equal(
            await inspector
                .getByRole('button', { name: 'Relationships', exact: true, includeHidden: true })
                .getAttribute('aria-expanded'),
            'false',
        );
        assert.equal(
            await inspector
                .getByRole('button', { name: 'Stored format', exact: true, includeHidden: true })
                .getAttribute('aria-expanded'),
            'false',
        );
        assert.equal(
            await inspector.isVisible(),
            logicalWidth > 850,
            'Preserve the shared narrow-screen inspector layout',
        );
        await page.getByRole('searchbox', { name: 'Search Waves' }).fill('Wave 2');
        await tabs.getByRole('button', { name: 'Voices', exact: true }).click();
        await page.getByRole('searchbox', { name: 'Search Voices' }).fill('Piano');
        await tabs.getByRole('button', { name: 'Waves', exact: true }).click();
        assert.equal(await page.getByRole('searchbox', { name: 'Search Waves' }).inputValue(), 'Wave 2');
        if (logicalWidth > 850) {
            await inspector.getByRole('button', { name: 'Relationships', exact: true }).click();
            await inspector.getByText('AMBIGUOUS', { exact: true }).waitFor();
            await inspector.getByRole('button', { name: 'Candidate: Wave 21' }).click();
            assert(
                await page
                    .getByRole('region', { name: 'TX16W native objects' })
                    .getByRole('button', { name: 'Inspect Wave 21', exact: true })
                    .evaluate((node) => node === document.activeElement),
            );
        }
        await waveRow.getByRole('button', { name: 'Play Wave 20', exact: true }).click();
        await waveRow.getByRole('button', { name: 'Stop Wave 20', exact: true }).waitFor();
        const seek = waveRow.getByRole('slider', { name: 'Seek Wave 20', exact: true });
        await seek.focus();
        await page.keyboard.press('Home');
        await page.keyboard.press('ArrowRight');
        assert(Number(await seek.inputValue()) > 0, 'Native TX16W keyboard seeking changes actual playback position');
        const seekBounds = await seek.boundingBox();
        await seek.click({ position: { x: seekBounds.width / 2, y: seekBounds.height / 2 } });
        await page.waitForFunction(
            () =>
                Number(
                    document
                        .querySelector('[aria-label="Wave 20 Wave Data"] .waveform-frame')
                        ?.getAttribute('data-playhead-ratio'),
                ) > 0.1,
        );
        await seek.focus();
        await page.keyboard.press('End');
        await waveRow.getByRole('button', { name: 'Play Wave 20', exact: true }).waitFor();
        await page.waitForFunction(() => document.activeElement?.getAttribute('aria-label') === 'Select Wave 20 waveform');
        assert.equal(await waveRow.getByRole('slider').count(), 0, 'Completed playback has no false idle seek position');
        await waveRow.getByRole('button', { name: 'Play Wave 20', exact: true }).click();
        await waveRow.getByRole('button', { name: 'Stop Wave 20', exact: true }).waitFor();
        await page.getByRole('button', { name: 'Files', exact: true }).click();
        await page.getByText('Files workspace').first().waitFor();
        const fileInspector = page.getByRole('complementary', { name: 'Filesystem inspector', includeHidden: true });
        assert.equal(await fileInspector.locator('.inspector-mode-footer button').count(), 1);
        if (logicalWidth > 850) {
            const action = fileInspector.getByRole('button', { name: 'Show in TX16W disk set (220 objects)' });
            const actionBounds = await action.boundingBox();
            const bodyBounds = await fileInspector.locator('.inspector-body').boundingBox();
            const inspectorBounds = await fileInspector.boundingBox();
            assert(actionBounds.y >= bodyBounds.y + bodyBounds.height - 1);
            assert(actionBounds.y + actionBounds.height <= inspectorBounds.y + inspectorBounds.height);
            await page.screenshot({ path: resolve(output, `files-${width}-${scale}.png`) });
            await action.click();
        } else {
            await page.getByRole('button', { name: 'Device', exact: true }).click();
        }
        await page.waitForFunction(() => document.querySelector('[aria-label="Search Waves"]')?.value === 'Wave 2');
        await waveRow.getByRole('button', { name: 'Play Wave 20', exact: true }).waitFor();
        assert.equal(
            await waveRow.getByRole('button', { name: 'Stop Wave 20', exact: true }).count(),
            0,
            'Files mode stops native audition',
        );
        await inspector.getByText('33333', { exact: true }).waitFor({ state: inspectorState });
        await page.getByText('Discovery notices (1)').click();
        await page.getByText('Missing wave companion', { exact: true }).waitFor();
        await page.getByText('Discovery notices (1)').focus();
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('details[open]').count(), 0);
        const bounds = await selector.boundingBox();
        assert(bounds.x >= 0 && bounds.x + bounds.width <= logicalWidth + 1);
        const header = await page.locator('.app-header').boundingBox();
        const content = await page.getByRole('region', { name: 'TX16W native objects' }).boundingBox();
        assert(content.y >= header.y + header.height - 1, 'Document controls must not overlap workspace content');
        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth), false);
        await waveRow.scrollIntoViewIfNeeded();
        await page.screenshot({ path: resolve(output, `native-${width}-${scale}.png`) });
        assert.deepEqual(errors, []);
        results.push({ width, scale, passed: true, geometry, collectionParity, waveGeometry, inspectorTypography });
        await page.close();
    }
} catch (error) {
    failure = String(error);
    if (page && !page.isClosed()) await page.screenshot({ path: resolve(output, 'failure.png') }).catch(() => {});
    throw error;
} finally {
    clearTimeout(deadline);
    process.removeListener('SIGTERM', signal);
    process.removeListener('SIGINT', signal);
    await stop();
    if (port)
        await new Promise((resolve, reject) => {
            const socket = socketServer();
            socket.once('error', reject);
            socket.listen(port, '127.0.0.1', () => socket.close((error) => (error ? reject(error) : resolve())));
        });
    await writeFile(
        resolve(output, 'results.json'),
        `${JSON.stringify({ serverStopped: true, browserStopped: true, portReleased: !!port, failure, results }, null, 2)}\n`,
    );
}
