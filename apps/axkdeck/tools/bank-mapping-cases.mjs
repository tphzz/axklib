import assert from 'node:assert/strict';
import { resolve } from 'node:path';

export async function bankMappingCases(browser, base, output, results) {
    for (const scale of [1, 1.25, 1.5]) {
        const context = await browser.newContext({ viewport: { width: Math.round(1040 * scale), height: Math.round(680 * scale) } });
        const main = await context.newPage();
        const result = { name: `bank-${scale}`, errors: [] }; results.push(result);
        context.on('page', page => page.on('pageerror', error => result.errors.push(error.message)));
        const state = async () => JSON.parse(await main.locator('[data-bank-main]').textContent());
        const wait = async predicate => main.waitForFunction(predicate);
        const snapshot = async page => JSON.parse(await page.locator('[data-mapping-snapshot]').textContent()).state;
        const open = async label => {
            const next = context.waitForEvent('page');
            await main.getByRole('button', { name: label, exact: true }).click();
            const page = await next;
            await page.getByRole('button', { name: 'High velocity limit', exact: true }).waitFor();
            await page.evaluate(scale => { document.body.style.zoom = String(scale); document.querySelector('.mapping-window').style.height = `${100 / scale}dvh`; }, scale);
            return page;
        };
        try {
            await main.goto(`${base}/tools/layout-fixtures/bank-mapping.html?channel=${scale}`);
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).ready);
            await main.evaluate(scale => { document.body.style.zoom = String(scale); document.querySelector('.fixture').style.height = `${100 / scale}dvh`; }, scale);
            const bank = await open('Bank Mapping'), members = await open('Member Mapping');
            const coverage = await main.locator('.keyboard-mapping').first().evaluate(root => {
                const a = root.querySelector('[data-note="24"]'), b = root.querySelector('[data-note="96"]');
                const key = root.querySelector('.keyboard-surface').getBoundingClientRect(), toolbar = root.querySelector('.toolbar').getBoundingClientRect();
                return { a: getComputedStyle(a).fill, b: getComputedStyle(b).fill, gap: key.y - toolbar.bottom,
                    labels: [...root.querySelectorAll('.key-labels span')].map(label => {
                        const note = Number(label.dataset.note); const face = root.querySelector(`[data-note="${note}"]`).getBoundingClientRect(), text = label.getBoundingClientRect();
                        return Math.abs((face.x + face.width / 2) - (text.x + text.width / 2));
                    }) };
            });
            assert.notEqual(coverage.a, coverage.b, 'Adjacent member coverage alternates green tones');
            assert.ok(coverage.gap >= 4 * scale - 1, 'Compact toolbar leaves vertical padding');
            assert.ok(coverage.labels.every(delta => delta < 1), 'Every C label is centered on its white key face');
            for (const page of [bank, members]) {
                await page.locator('[data-key="60"]').click({ button: 'right' });
                assert.equal(await page.getByRole('menu', { name: 'Root key' }).count(), 0, 'Bank and Member contexts cannot edit root');
            }
            assert.equal(await bank.getByRole('button', { name: 'Low key limit', exact: true }).count(), 0);
            await bank.getByRole('spinbutton', { name: 'Low velocity', exact: true }).fill('50');
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).bank.velocity_low === 50);
            await members.getByRole('button', { name: 'Low key limit', exact: true }).press('ArrowRight');
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).a.key_low === 1);
            assert.equal((await snapshot(members)).zones[0].velocityLow, 0);
            assert.equal((await snapshot(bank)).zones[0].velocityLow, 50);
            assert.equal((await snapshot(members)).limits.velocityLow, 0);
            await members.getByRole('button', { name: 'Sample', exact: true }).click();
            await members.getByRole('option', { name: 'b', exact: true }).click();
            await members.getByRole('button', { name: 'High key limit', exact: true }).press('ArrowLeft');
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).b.key_high === 126);
            await bank.screenshot({ path: resolve(output, `${result.name}-overrides.png`) });
            await members.screenshot({ path: resolve(output, `${result.name}-members.png`) });
            await main.screenshot({ path: resolve(output, `${result.name}-compact.png`) });
            await bank.getByRole('button', { name: 'Save', exact: true }).click();
            await wait(() => { const s = JSON.parse(document.querySelector('[data-bank-main]').textContent); return s.writes.length === 1 && !Object.keys(s.bank).length; });
            let current = await state();
            assert.equal(current.writes[0].operations[0].type, 'update_sample_bank_overrides');
            assert.equal(current.a.key_low, 1); assert.equal(current.b.key_high, 126);
            await main.getByRole('button', { name: 'Change bank level' }).click();
            await members.getByRole('button', { name: 'Save', exact: true }).click();
            await wait(() => { const s = JSON.parse(document.querySelector('[data-bank-main]').textContent); return s.writes.length === 2 && !Object.keys(s.a).length && !Object.keys(s.b).length; });
            current = await state();
            assert.equal(current.writes[1].operations.length, 2);
            assert.ok(current.writes[1].operations.every(operation => operation.type === 'update_sbnk_parameters'));
            assert.equal(current.bank.level, 80, 'Member Save must preserve the bank draft');
            const sample = await open('Mapping Editor');
            await sample.getByRole('button', { name: 'Low key limit', exact: true }).press('ArrowRight');
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).a.key_low === 2);
            await members.getByRole('button', { name: 'Sample', exact: true }).click();
            await members.getByRole('option', { name: 'a', exact: true }).click();
            assert.equal((await snapshot(members)).limits.low, 2, 'Sample and member windows share a canonical draft');
            const rootKey = sample.locator('[data-key="37"]');
            await rootKey.click({ button: 'right' });
            const rootMenu = await sample.getByRole('menu', { name: 'Root key' }).boundingBox();
            const viewport = sample.viewportSize();
            assert.ok(rootMenu.x >= 0 && rootMenu.y >= 0 && rootMenu.x + rootMenu.width <= viewport.width && rootMenu.y + rootMenu.height <= viewport.height, 'Root menu stays inside the scaled viewport');
            await sample.getByRole('menuitem', { name: 'Set root to C#1', exact: true }).click();
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).a.root_key === 37);
            assert.equal((await state()).aValues.key_low, 2, 'Root edits leave stored key bounds untouched');
            const rootFace = await sample.locator('[data-key="37"] .root-marker').evaluate(node => {
                const swatch = document.createElement('span'); swatch.style.color = 'var(--color-warning, #e4b15b)'; document.body.append(swatch);
                const amber = getComputedStyle(swatch).color; swatch.remove();
                return { height: node.getBBox().height, fill: getComputedStyle(node).fill, root: node.classList.contains('root-marker'), amber };
            });
            assert.equal(rootFace.height, 5, 'A black-key root uses a separate marker without obscuring key edges');
            assert.equal(rootFace.root, true);
            assert.equal(rootFace.fill, rootFace.amber, 'Root uses reserved amber, not a member color');
            await sample.getByRole('button', { name: 'Undo', exact: true }).click();
            await wait(() => !Object.hasOwn(JSON.parse(document.querySelector('[data-bank-main]').textContent).a, 'root_key'));
            await rootKey.focus(); await rootKey.press('Shift+F10');
            await sample.getByRole('menu', { name: 'Root key' }).press('Escape');
            assert.ok(await rootKey.evaluate(node => node === document.activeElement), 'Root menu returns keyboard focus');
            await sample.getByRole('button', { name: 'High velocity limit', exact: true }).press('Home');
            await wait(() => JSON.parse(document.querySelector('[data-bank-main]').textContent).a.velocity_high === 0);
            const handles = await sample.locator('.plot').evaluate(plot => {
                const rect = plot.getBoundingClientRect(), limits = plot.querySelector('.limits').getBoundingClientRect();
                const center = name => { const r = plot.querySelector(`[aria-label="${name}"]`).getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 }; };
                return { rect: { x: rect.x, right: rect.right, bottom: rect.bottom }, limits: { y: limits.y, bottom: limits.bottom }, low: center('Low key limit'), high: center('High key limit'), bottom: center('Low velocity limit'), top: center('High velocity limit') };
            });
            result.singleVelocityGeometry = handles;
            assert.ok(Math.abs(handles.low.y - (handles.limits.y + handles.limits.bottom) / 2) < 1, 'Key handles follow the reduced velocity rectangle');
            assert.ok(Math.abs(handles.bottom.y - handles.rect.bottom) <= 2 * scale, 'Bottom handle is on the boundary');
            assert.ok(Math.abs(handles.top.y - handles.limits.y) < 1, 'Top handle is on the boundary');
            assert.ok(Math.abs(handles.top.x - handles.bottom.x) >= 20 * scale, 'Single-velocity handles remain separately reachable');
            await sample.screenshot({ path: resolve(output, `${result.name}-single-velocity.png`) });
            await sample.getByRole('button', { name: 'High key limit', exact: true }).press('Home');
            const narrow = await sample.locator('.plot').evaluate(plot => {
                const limits = plot.querySelector('.limits').getBoundingClientRect();
                return { left: limits.left, right: limits.right, centers: ['Low velocity limit', 'High velocity limit'].map(name => { const r = plot.querySelector(`[aria-label="${name}"]`).getBoundingClientRect(); return r.x + r.width / 2; }) };
            });
            assert.ok(narrow.centers.every(x => x >= narrow.left - 1 && x <= narrow.right + 1), 'Single-note velocity handles remain within their boundary edges');
            await sample.getByRole('button', { name: 'Undo', exact: true }).click();
            await sample.getByRole('button', { name: 'Undo', exact: true }).click();
            await sample.getByRole('button', { name: 'Zoom keyboard in' }).click();
            assert.equal(await sample.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, 'Clipped limits do not create horizontal overflow');
            await sample.getByRole('button', { name: 'Zoom keyboard out' }).click();
            await sample.screenshot({ path: resolve(output, `${result.name}-sample.png`) });
            assert.deepEqual(result.errors, []);
            result.passed = true;
            console.log(`${result.name}: simultaneous bank/member/Sample windows and isolated atomic saves passed`);
        } catch (error) { result.error = error.stack; process.exitCode = 1; console.error(result.error); }
        finally { await context.close(); }
    }
}
