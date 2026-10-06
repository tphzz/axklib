import assert from 'node:assert/strict';
import { resolve } from 'node:path';

export async function mappingNavigationCases(browser, base, output, results) {
    const cases = [
        { width: 390, scale: 1.5 }, { width: 1040, scale: 1 },
        ...[3440, 5120].flatMap(width => [1, 1.25, 1.5].map(scale => ({ width, scale }))),
    ];
    for (const { width, scale } of cases) {
        const context = await browser.newContext({ viewport: { width, height: 900 } });
        const page = await context.newPage();
        page.setDefaultTimeout(8000);
        const result = { name: `navigation-${width}-${scale}`, errors: [], mutations: [] };
        results.push(result);
        page.on('pageerror', error => result.errors.push(error.message));
        page.on('request', request => { if (request.method() !== 'GET') result.mutations.push(request.url()); });
        const state = () => page.locator('[data-presentation]').evaluate(node => JSON.parse(node.textContent));
        const compact = page.locator('.compact .keyboard-mapping'), full = page.locator('.full > .keyboard-mapping');
        const settle = () => page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
        try {
            await page.goto(`${base}/tools/layout-fixtures/mapping-presentation.html?audition`);
            await full.locator('.zone').first().waitFor();
            await page.evaluate(scale => {
                document.body.style.zoom = String(scale);
                document.querySelector('.fixture').style.height = `${100 / scale}dvh`;
            }, scale);
            await settle();
            const compactGeometry = async () => compact.evaluate(root => {
                const axis = root.querySelector('.keys'), matrix = axis.getScreenCTM();
                const box = root.getBoundingClientRect(), toolbar = root.querySelector('.toolbar').getBoundingClientRect();
                return { width: box.width, x: box.x, keyboardX: axis.getBoundingClientRect().x, toolbarX: toolbar.x,
                    keyWidths: [...axis.querySelectorAll('rect.white')].map(rect => rect.width.baseVal.value * matrix.a),
                    keyHeight: axis.getBoundingClientRect().height, span: Number(axis.getAttribute('viewBox').split(' ')[2]),
                    overflow: document.documentElement.scrollWidth > innerWidth };
            });
            const initial = await compactGeometry();
            assert.ok(initial.width <= 1280 * scale + 1, 'The entire compact mapping block is width-capped');
            assert.ok(initial.keyWidths.every(size => size <= 20 * scale + .1), 'Compact white keys stay within 20 CSS pixels');
            assert.equal(initial.overflow, false);
            assert.ok(Math.abs(initial.keyHeight - 28 * scale) < 1);
            assert.ok(Math.abs(initial.keyboardX - initial.toolbarX) < 1, 'The strip and toolbar share their left edge');
            if (width >= 3440) {
                assert.equal(await compact.getByRole('button', { name: 'Zoom keyboard in', exact: true }).isDisabled(), true);
                assert.equal(initial.span, 128, 'Ultrawide compact keyboards stop at the natural key-width cap');
                assert.equal(await full.getByRole('button', { name: 'Zoom keyboard in', exact: true }).isDisabled(), false);
                const fullWidth = (await full.locator('.keys').boundingBox()).width;
                assert.ok(fullWidth > 1280 * scale, 'The full editor is not width-capped');
            } else {
                await compact.getByRole('button', { name: 'Zoom keyboard in', exact: true }).click();
                await settle();
                const zoomed = await compactGeometry();
                assert.ok(Math.abs(zoomed.width - initial.width) < 1, 'Compact zoom does not shrink or shift the strip');
                assert.ok(zoomed.keyWidths.every(size => size <= 20 * scale + .1));
                await compact.getByRole('scrollbar', { name: 'Keyboard viewport', exact: true }).press('End');
            }
            const beforeNavigation = await state();
            await full.getByRole('button', { name: 'Zoom keyboard in', exact: true }).click();
            const rail = full.getByRole('scrollbar', { name: 'Keyboard viewport', exact: true });
            await rail.press('Home');
            await rail.press('Shift+ArrowRight');
            assert.equal(await rail.getAttribute('aria-valuenow'), '12');
            await rail.press('PageDown');
            assert.equal(await rail.getAttribute('aria-valuenow'), '64');
            await rail.press('ArrowLeft');
            assert.equal(await rail.getAttribute('aria-valuenow'), '63');
            await rail.press('Home');
            const railBox = await rail.boundingBox();
            await page.mouse.click(railBox.x + railBox.width * .95, railBox.y + railBox.height / 2);
            assert.equal(await rail.getAttribute('aria-valuenow'), '64', 'Rail clicks recenter and clamp to the MIDI edge');
            const thumb = await rail.locator('.viewport').boundingBox();
            await page.mouse.move(thumb.x + thumb.width / 2, thumb.y + thumb.height / 2);
            await page.mouse.down();
            await page.mouse.move(thumb.x + thumb.width / 2 - railBox.width / 4, thumb.y + thumb.height / 2, { steps: 8 });
            await settle();
            assert.ok(Number(await rail.getAttribute('aria-valuenow')) < 64, 'The viewport can be drag-panned');
            await page.keyboard.press('Escape');
            await page.mouse.up();
            assert.equal(await rail.getAttribute('aria-valuenow'), '64', 'Escape restores the starting viewport');
            assert.deepEqual(await state(), beforeNavigation, 'Navigation never changes mapping values or selection');
            await full.getByRole('button', { name: 'Zoom keyboard out', exact: true }).click();
            assert.equal(await rail.count(), 0, 'The overview is omitted at the complete MIDI range');

            const key = full.locator('[data-key="24"]');
            const face = key.locator('[data-note="24"]');
            const normalStroke = await face.evaluate(node => getComputedStyle(node).stroke);
            const keyBox = await face.boundingBox();
            await page.mouse.move(keyBox.x + keyBox.width / 2, keyBox.y + keyBox.height - 3 * scale);
            await page.mouse.down();
            await settle();
            assert.equal(await key.evaluate(node => node.classList.contains('pressed')), true);
            assert.notEqual(await face.evaluate(node => getComputedStyle(node).stroke), normalStroke);
            await page.mouse.up();
            await settle();
            assert.equal(await key.evaluate(node => node.classList.contains('pressed')), false);
            assert.equal(await face.evaluate(node => getComputedStyle(node).stroke), normalStroke, 'Pointer release removes the outline despite retained focus');
            await key.press('ArrowRight');
            assert.equal(await full.locator('[data-key="25"]').evaluate(node => node.classList.contains('keyboard-focus')), true);

            const handle = full.getByRole('button', { name: 'High velocity limit', exact: true });
            assert.equal(await handle.getAttribute('title'), null);
            const handleBox = await handle.boundingBox(), plot = await full.locator('.plot').boundingBox();
            await page.mouse.move(handleBox.x + handleBox.width / 2, handleBox.y + handleBox.height / 2);
            await page.mouse.down();
            await page.mouse.move(handleBox.x + handleBox.width / 2, handleBox.y + handleBox.height / 2 + plot.height / 4, { steps: 8 });
            await settle();
            const readout = full.locator('[data-drag-readout]');
            assert.equal(await readout.textContent(), 'Velocity: 0 - 95');
            const label = await readout.boundingBox();
            assert.ok(label.x >= plot.x && label.x + label.width <= plot.x + plot.width);
            assert.ok(label.y >= plot.y && label.y + label.height <= plot.y + plot.height);
            assert.equal(await readout.evaluate(node => getComputedStyle(node).pointerEvents), 'none');
            assert.equal(await full.getByRole('button', { name: 'Zoom keyboard in', exact: true }).isDisabled(), true);
            await page.screenshot({ path: resolve(output, `${result.name}-readout.png`) });
            await page.mouse.up();
            assert.equal(await readout.count(), 0);
            if (width === 1040) {
                await compact.getByRole('scrollbar').press('Home');
                const compactHandle = compact.getByRole('button', { name: 'Low key limit', exact: true });
                const prior = await compact.locator('.toolbar').boundingBox();
                await compactHandle.focus(); await page.keyboard.down('ArrowRight');
                assert.ok((await compact.locator('.summary').textContent()).startsWith('Keys: 1 (C#-2'));
                const during = await compact.locator('.toolbar').boundingBox();
                assert.equal(during.height, prior.height, 'Compact readouts cannot add a row');
                await page.keyboard.up('ArrowRight');
                assert.equal(await compact.locator('[data-drag-readout]').count(), 0);
                await full.getByRole('button', { name: 'Zoom keyboard in', exact: true }).click();
                const resizedRail = full.getByRole('scrollbar');
                const resizedThumb = await resizedRail.locator('.viewport').boundingBox();
                await page.mouse.move(resizedThumb.x + resizedThumb.width / 2, resizedThumb.y + 10);
                await page.mouse.down();
                await page.setViewportSize({ width: 850, height: 900 });
                await settle(); await page.mouse.up();
                await resizedRail.press('End');
                assert.equal(await resizedRail.getAttribute('aria-valuenow'), '64', 'Resize cancels a captured overview drag cleanly');
                await page.setViewportSize({ width: 500, height: 900 });
                await settle();
                await compact.getByRole('button', { name: 'Zoom keyboard in', exact: true }).click();
                await settle();
                const originalView = (await compact.locator('.keys').getAttribute('viewBox')).split(' ').map(Number);
                const compactThumb = await compact.locator('.overview .viewport').boundingBox();
                await page.mouse.move(compactThumb.x + compactThumb.width / 2, compactThumb.y + 10);
                await page.mouse.down();
                await page.setViewportSize({ width: 700, height: 900 });
                await settle(); await page.mouse.up();
                const restoredView = (await compact.locator('.keys').getAttribute('viewBox')).split(' ').map(Number);
                assert.ok(restoredView[2] > originalView[2], 'A wider compact viewport increases its required span');
                assert.ok(Math.abs(restoredView[0] + restoredView[2] / 2 - originalView[0] - originalView[2] / 2) <= .5,
                    'Canceled compact pans preserve the original center when the key-width cap increases');
            }
            result.geometry = initial;
            assert.deepEqual(result.errors, []);
            assert.deepEqual(result.mutations, []);
            result.passed = true;
            await page.screenshot({ path: resolve(output, `${result.name}.png`) });
            console.log(`${result.name}: width cap, press feedback, overview and drag readouts passed`);
        } catch (error) {
            result.error = error.stack;
            process.exitCode = 1;
            console.error(result.error);
            await page.screenshot({ path: resolve(output, `${result.name}-failure.png`) }).catch(() => {});
        } finally { await context.close(); }
    }
}
