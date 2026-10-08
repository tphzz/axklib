import assert from 'node:assert/strict';
import { resolve } from 'node:path';

export async function mappingStoredRangesCases(browser, base, output, results) {
    for (const scale of [1, 1.25, 1.5]) {
        const context = await browser.newContext({ viewport: { width: Math.round(1040 * scale), height: Math.round(680 * scale) } });
        const main = await context.newPage();
        const result = { name: `stored-ranges-${scale}`, errors: [] }; results.push(result);
        context.on('page', page => page.on('pageerror', error => result.errors.push(error.message)));
        const state = () => main.locator('[data-bank-main]').evaluate(node => JSON.parse(node.textContent));
        const waitValue = (key, value) => main.waitForFunction(({ key, value }) => JSON.parse(document.querySelector('[data-bank-main]').textContent).aValues[key] === value, { key, value });
        const open = async label => {
            const next = context.waitForEvent('page');
            await main.getByRole('button', { name: label, exact: true }).click();
            const page = await next;
            page.setDefaultTimeout(8000);
            await page.getByRole('button', { name: 'High velocity limit', exact: true }).waitFor();
            await page.evaluate(scale => { document.body.style.zoom = String(scale); document.querySelector('.mapping-window').style.height = `${100 / scale}dvh`; }, scale);
            return page;
        };
        const aligned = (page, high) => page.waitForFunction(high => {
            const root = document.querySelector('.keyboard-mapping');
            const top = (127 - high) / 128 * 100;
            return ['.zone.chosen', '.selection-outline', '.limits'].every(selector => Math.abs(parseFloat(root.querySelector(selector)?.style.top) - top) < .0001);
        }, high);
        const drag = async (page, label, delta) => {
            const handle = page.getByRole('button', { name: label, exact: true });
            await page.waitForFunction(label => !document.querySelector(`[aria-label="${label}"]`).disabled, label);
            await handle.scrollIntoViewIfNeeded();
            const box = await handle.boundingBox(), plot = await page.locator('.plot-content').boundingBox();
            const x = Math.max(plot.x + 2, Math.min(plot.x + plot.width - 2, box.x + box.width / 2));
            const y = Math.max(plot.y + 2, Math.min(plot.y + plot.height - 2, box.y + box.height / 2));
            const hit = await page.evaluate(({x,y}) => {
                const node = document.elementFromPoint(x, y);
                return { label: node?.getAttribute('aria-label'), tag: node?.tagName, class: node?.getAttribute('class'), viewport: { width: innerWidth, height: innerHeight } };
            }, {x,y});
            result.lastDrag = { label, box, plot, x, y, hit };
            assert.equal(hit.label, label, 'The test pointer hits the visible handle');
            await page.mouse.move(x, y);
            await page.mouse.down();
            await page.mouse.move(x, y + plot.height * delta / 128, { steps: 12 });
        };
        const pixels = async (page, suffix) => {
            const screenshot = await page.screenshot({ path: resolve(output, `${result.name}-${suffix}.png`) });
            const colors = await page.evaluate(async encoded => {
                const image = new Image(); image.src = `data:image/png;base64,${encoded}`; await image.decode();
                const canvas = document.createElement('canvas'); canvas.width = image.width; canvas.height = image.height;
                const ctx = canvas.getContext('2d'); ctx.drawImage(image, 0, 0);
                const plot = document.querySelector('.plot-content').getBoundingClientRect();
                const at = (note, velocity) => [...ctx.getImageData(Math.floor(plot.x + plot.width * note / 128), Math.floor(plot.y + plot.height * (127.5 - velocity) / 128), 1, 1).data].slice(0, 3);
                const outside = at(24.3, 113), inside = at(24.3, 40), neighbor = at(80.3, 113);
                ctx.fillStyle = getComputedStyle(document.querySelector('.plot')).backgroundColor; ctx.fillRect(0, 0, 1, 1);
                return { outside, inside, neighbor, background: [...ctx.getImageData(0, 0, 1, 1).data].slice(0, 3) };
            }, screenshot.toString('base64'));
            assert.ok(colors.outside.every((channel, i) => Math.abs(channel - colors.background[i]) < 4), 'Outside the stored member range must be black/grid, not a bank-effective green fill');
            assert.notDeepEqual(colors.inside, colors.outside, 'The selected stored range is painted');
            assert.notDeepEqual(colors.neighbor, colors.outside, 'An unaffected member remains painted');
            return colors;
        };
        try {
            await main.goto(`${base}/tools/layout-fixtures/bank-mapping.html?override&channel=${result.name}`);
            await main.waitForFunction(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).ready);
            const bank = await open('Bank Mapping');
            let members = await open('Member Mapping');
            await aligned(members, 84);
            assert.ok((await members.locator('.legend').textContent()).includes('Stored'));
            assert.equal(await members.locator('.source').count(), 0, 'Stored view does not duplicate its range as a source outline');
            assert.equal(await bank.locator('.zone.chosen').evaluate(node => node.style.top), '0%', 'Bank Mapping keeps effective overrides');
            result.initialPixels = await pixels(members, 'initial');

            await drag(members, 'High velocity limit', 20);
            await aligned(members, 64);
            assert.equal((await state()).aValues.velocity_high, 84, 'Preview does not mutate the canonical draft');
            assert.equal(Number(await members.locator('[data-boundary="velocityHigh"]').getAttribute('y1')), 63, 'Guide follows the local fill');
            assert.equal(await bank.locator('.zone.chosen').evaluate(node => node.style.top), '0%');
            result.previewPixels = await pixels(members, 'preview');
            await members.mouse.up(); await waitValue('velocity_high', 64);
            await members.getByRole('button', { name: 'Undo', exact: true }).click(); await waitValue('velocity_high', 84); await aligned(members, 84);
            await members.getByRole('button', { name: 'Redo', exact: true }).click(); await waitValue('velocity_high', 64); await aligned(members, 64);
            await members.getByRole('button', { name: 'Discard', exact: true }).click(); await waitValue('velocity_high', 84); await aligned(members, 84);

            await drag(members, 'Low velocity limit', -20);
            await members.waitForFunction(() => Math.abs(parseFloat(document.querySelector('.zone.chosen').style.height) - 50.78125) < .0001);
            assert.equal((await state()).aValues.velocity_low, 0);
            await members.keyboard.press('Escape'); await members.mouse.up(); await aligned(members, 84);
            assert.ok(Math.abs(await members.locator('.zone.chosen').evaluate(node => parseFloat(node.style.height)) - 66.40625) < .0001);

            await drag(bank, 'High velocity limit', 27);
            await bank.waitForFunction(() => [...document.querySelectorAll('.zone')].every(node => Math.abs(parseFloat(node.style.top) - 21.09375) < .0001));
            assert.equal((await state()).bank.velocity_high, undefined, 'Bank preview also stays local');
            await aligned(members, 84);
            await bank.keyboard.press('Escape'); await bank.mouse.up();
            assert.equal(await bank.locator('.zone.chosen').evaluate(node => node.style.top), '0%');

            await drag(members, 'High velocity limit', 14);
            await aligned(members, 70); await members.mouse.up(); await waitValue('velocity_high', 70);
            await members.getByRole('button', { name: 'Save', exact: true }).click();
            await main.waitForFunction(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).writes.length === 1);
            await members.close(); members = await open('Member Mapping'); await aligned(members, 70);
            result.reopenedPixels = await pixels(members, 'reopened');
            const saved = await state();
            assert.equal(saved.writes[0].operations.length, 1);
            assert.equal(saved.writes[0].operations[0].type, 'update_sbnk_parameters');
            assert.equal(saved.aValues.root_key, 36); assert.equal(saved.bValues.velocity_high, 127);
            assert.deepEqual(saved.bank, {}); assert.deepEqual(result.errors, []);
            result.passed = true;
            console.log(`${result.name}: live fills, guides, cancellation, stored/effective separation, pixels and Save/reopen passed`);
        } catch (error) {
            result.error = error.stack;
            result.state = await state().catch(() => null);
            result.windows = await Promise.all(context.pages().map(page => page.locator('.keyboard-mapping').first().evaluate(root => ({
                fills: [...root.querySelectorAll('.zone')].map(node => node.getAttribute('style')),
                limits: root.querySelector('.limits')?.getAttribute('style'),
            })).catch(() => null)));
            process.exitCode = 1; console.error(result.error);
        }
        finally { await context.close(); }
    }
}
