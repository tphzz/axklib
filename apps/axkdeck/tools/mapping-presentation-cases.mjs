import assert from 'node:assert/strict';
import { resolve } from 'node:path';

export async function mappingPresentationCases(browser, base, output, results) {
    for (const scale of [1, 1.25, 1.5]) {
        for (const scenario of ['adjacent', 'overlap', 'group']) {
            const context = await browser.newContext({ viewport: { width: Math.round(1040 * scale), height: Math.round(680 * scale) } });
            const page = await context.newPage();
            page.setDefaultTimeout(8000);
            const result = { name: `presentation-${scenario}-${scale}`, errors: [] };
            results.push(result);
            page.on('pageerror', error => result.errors.push(error.message));
            const full = page.locator('.full > .keyboard-mapping');
            const colors = () => full.locator('.zone').evaluateAll(zones => zones.map(zone => zone.style.getPropertyValue('--zone-color')));
            const geometry = () => full.evaluate(root => {
                const plot = root.querySelector('.plot-content'), box = plot.getBoundingClientRect();
                const svg = root.querySelector('.guides'), matrix = svg.getScreenCTM();
                const guides = [...svg.querySelectorAll('line')].map(line => {
                    const point = new DOMPoint(line.x1.baseVal.value, line.y1.baseVal.value).matrixTransform(matrix);
                    return { boundary: line.dataset.boundary, x: point.x, y: point.y, pointer: getComputedStyle(svg).pointerEvents };
                });
                const limits = root.querySelector('.limits').getBoundingClientRect();
                const selectedBoxes = [...root.querySelectorAll('.selection-outline')].map(node => node.getBoundingClientRect());
                const selectedBounds = { x: Math.min(...selectedBoxes.map(box => box.x)), right: Math.max(...selectedBoxes.map(box => box.right)), y: Math.min(...selectedBoxes.map(box => box.y)), bottom: Math.max(...selectedBoxes.map(box => box.bottom)) };
                const major = root.querySelector('.velocity-major'), minor = root.querySelector('.velocity-minor');
                const rasterValues = path => [...path.getAttribute('d').matchAll(/M[\d.]+ ([\d.]+)H/g)].map(match => 127.5 - Number(match[1]));
                const labels = [...root.querySelectorAll('.mapping-label')].map(label => {
                    const style = getComputedStyle(label), rect = label.getBoundingClientRect();
                    return { text: label.textContent, x: rect.x, y: rect.y, right: rect.right, bottom: rect.bottom, vertical: style.writingMode, pointer: style.pointerEvents, font: style.fontSize, familyMatches: style.fontFamily === getComputedStyle(root).fontFamily, ellipsis: style.textOverflow };
                });
                const ticks = [...root.querySelectorAll('.velocity-axis span')].map(label => {
                    const rect = label.getBoundingClientRect(), value = Number(label.dataset.velocity);
                    return { value, delta: Math.abs(rect.y + rect.height / 2 - (box.y + (127 - value + 0.5) / 128 * box.height)) };
                });
                return { plot: { x: box.x, y: box.y, right: box.right, bottom: box.bottom }, limits: { x: limits.x, right: limits.right, y: limits.y, bottom: limits.bottom }, selectedBounds, guides, labels, ticks,
                    majorValues: rasterValues(major), minorValues: rasterValues(minor), majorWidth: Number(getComputedStyle(major).strokeWidth.replace('px', '')), minorWidth: Number(getComputedStyle(minor).strokeWidth.replace('px', '')),
                    selected: root.querySelectorAll('.zone.chosen').length, outlines: root.querySelectorAll('.selection-outline').length,
                    sources: root.querySelectorAll('.source').length, overflow: document.documentElement.scrollWidth > innerWidth };
            });
            try {
                const query = scenario === 'adjacent' ? '' : `?${scenario}`;
                await page.goto(`${base}/tools/layout-fixtures/mapping-presentation.html${query}`);
                await full.locator('.zone').first().waitFor();
                await page.evaluate(scale => {
                    document.body.style.zoom = String(scale);
                    document.querySelector('.fixture').style.height = `${100 / scale}dvh`;
                }, scale);
                const originalColors = await colors();
                assert.equal(new Set(originalColors).size, 2, 'Only two restrained green tones are used');
                const initial = await geometry();
                assert.equal(initial.overflow, false);
                assert.equal(initial.selected, scenario === 'group' ? 2 : 1);
                assert.equal(initial.outlines, initial.selected, 'Every selected effective region has an outline');
                assert.ok(initial.sources >= 8);
                assert.equal(initial.labels.length, 8, 'Fitting neighboring mappings have names; overlap labels do not collide');
                assert.ok(initial.labels.every(label => label.vertical === 'vertical-rl' && label.pointer === 'none' && label.font === '10px' && label.familyMatches && label.ellipsis === 'ellipsis'));
                assert.ok(initial.labels.every(label => label.x >= initial.plot.x && label.right <= initial.plot.right && label.y >= initial.plot.y && label.bottom <= initial.plot.bottom));
                assert.deepEqual(initial.ticks.map(tick => tick.value), Array.from({ length: 26 }, (_, index) => index * 5));
                assert.deepEqual(initial.majorValues, [0, 25, 50, 75, 100, 125]);
                assert.deepEqual([...initial.majorValues, ...initial.minorValues].sort((a, b) => a-b), Array.from({ length: 26 }, (_, i) => i * 5));
                assert.ok(initial.majorWidth > initial.minorWidth, 'Every twenty-fifth raster line is heavier');
                assert.ok(initial.ticks.every(tick => tick.delta < 2 * scale), 'Velocity axis labels follow their actual grid lines');
                assert.ok(initial.guides.every(guide => guide.pointer === 'none'), 'Guides cannot intercept selection or dragging');
                assert.ok(Math.abs(initial.guides.find(guide => guide.boundary === 'high').x - initial.selectedBounds.right) < scale, 'Vertical guide follows selected group edge, not editable limits');
                const chosen = await full.locator('.selection-outline').first().evaluate(zone => getComputedStyle(zone).backgroundColor);
                assert.notEqual(chosen, 'rgba(0, 0, 0, 0)', 'Selection has a separate highlighted fill above static mappings');
                const compactTones = await page.locator('.compact [data-note="20"]').evaluate(key => getComputedStyle(key).fill);
                const fullTones = await full.locator('[data-note="20"]').evaluate(key => getComputedStyle(key).fill);
                assert.equal(compactTones, fullTones, 'Compact and full keyboards agree');
                await page.screenshot({ path: resolve(output, `${result.name}.png`) });

                if (scenario === 'adjacent') {
                    const label = full.locator('.mapping-label[data-zone="zone-2"]');
                    const box = await label.boundingBox();
                    await page.mouse.click(box.x + box.width / 2, box.y + 30 * scale);
                    await page.waitForFunction(() => JSON.parse(document.querySelector('[data-presentation]').textContent).selected === 'zone-2');
                    assert.deepEqual(await colors(), originalColors, 'Selection cannot reshuffle tones');
                    await full.getByRole('button', { name: 'Zoom keyboard in' }).click();
                    assert.deepEqual(await colors(), originalColors, 'Zoom cannot reshuffle tones');
                    await full.getByRole('button', { name: 'Pan keyboard right' }).click();
                    assert.deepEqual(await colors(), originalColors, 'Pan cannot reshuffle tones');
                    const aligned = await full.evaluate(root => {
                        const region = root.querySelector('.zone.chosen').getBoundingClientRect();
                        const outline = root.querySelector('.selection-outline').getBoundingClientRect();
                        return ['left', 'right', 'top', 'bottom'].every(edge => Math.abs(region[edge] - outline[edge]) < 1);
                    });
                    assert.equal(aligned, true, 'Transformed hit targets stay aligned with unscaled selection outlines through zoom and pan');
                    await full.getByRole('button', { name: 'Zoom keyboard out' }).click();
                    await page.getByRole('button', { name: 'Wide limits', exact: true }).click();
                    assert.equal(await full.locator('.zone.chosen').count(), 1, 'Wide editable limits do not invent effective coverage');
                    const wide = await geometry();
                    assert.ok(Math.abs(wide.guides.find(guide => guide.boundary === 'low').x - wide.selectedBounds.x) < scale);
                    assert.ok(wide.guides.find(guide => guide.boundary === 'low').x > wide.limits.x + 20 * scale, 'Selected vertical guides are visible within wider editable limits');
                    const handle = full.getByRole('button', { name: 'High velocity limit', exact: true });
                    const before = await handle.boundingBox(), plot = await full.locator('.plot-content').boundingBox();
                    await page.mouse.move(before.x + before.width / 2, before.y + before.height / 2);
                    await page.mouse.down();
                    await page.mouse.move(before.x + before.width / 2, before.y + before.height / 2 + plot.height / 4, { steps: 8 });
                    const dragged = await geometry();
                    assert.ok(Math.abs(dragged.guides.find(guide => guide.boundary === 'velocityHigh').y - dragged.selectedBounds.y) < scale, 'Guides denote effective coverage, while blue bounds show the local preview');
                    assert.ok(dragged.limits.y > dragged.plot.y + plot.height / 5);
                    await page.mouse.up();
                    await page.screenshot({ path: resolve(output, `${result.name}-guides.png`) });
                    const tallTicks = dragged.ticks.length;
                    await page.setViewportSize({ width: Math.round(740 * scale), height: Math.round(480 * scale) });
                    await page.waitForFunction(() => document.querySelector('.plot').clientHeight < 340);
                    const short = await geometry();
                    assert.equal(short.ticks.length, tallTicks, 'Raster spacing remains fixed through resizing');
                    assert.equal(short.overflow, false);
                    await page.screenshot({ path: resolve(output, `${result.name}-short.png`) });
                    await page.getByRole('button', { name: 'Thin limits', exact: true }).click();
                    const thin = await geometry();
                    assert.equal(thin.guides.length, 4, 'Single-cell limits retain exact guides');
                    await page.getByRole('button', { name: 'Read-only', exact: true }).click();
                    assert.equal(await full.getByRole('button', { name: 'Low key limit', exact: true }).isDisabled(), true);
                    assert.equal((await geometry()).guides.length, 4, 'Read-only views retain guides');
                    await page.screenshot({ path: resolve(output, `${result.name}-readonly-thin.png`) });
                    if (scale === 1.5) {
                        await page.setViewportSize({ width: 390, height: 680 });
                        await page.goto(`${base}/tools/layout-fixtures/mapping-presentation.html?audition`);
                        await page.evaluate(() => { document.body.style.zoom = '1.5'; });
                        await page.getByRole('spinbutton', { name: 'Audition velocity', exact: true }).first().waitFor();
                        const contained = await page.locator('.toolbar').evaluateAll(toolbars => toolbars.every(toolbar => {
                            const box = toolbar.getBoundingClientRect();
                            return [...toolbar.querySelectorAll('button, input')].every(control => {
                                const rect = control.getBoundingClientRect();
                                return rect.left >= box.left - 1 && rect.right <= box.right + 1;
                            });
                        }));
                        assert.equal(contained, true, 'Audition controls remain contained at 390px and 150% scale');
                        assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, 'Narrow audition controls must not overflow the viewport');
                        await page.screenshot({ path: resolve(output, `${result.name}-narrow-audition.png`) });
                    }
                } else if (scenario === 'overlap') {
                    await full.getByRole('button', { name: 'Select overlapping mappings:', exact: false }).click();
                    await page.waitForFunction(() => JSON.parse(document.querySelector('[data-presentation]').textContent).selected === 'overlap');
                    assert.deepEqual(await colors(), originalColors);
                    assert.ok((await geometry()).labels.some(label => label.text === 'Overlapping Sample'), 'Selected overlap label takes priority');
                }
                result.geometry = initial;
                assert.deepEqual(result.errors, []);
                result.passed = true;
                console.log(`${result.name}: restrained tones, labels, grid, selection and guides passed`);
            } catch (error) {
                result.error = error.stack;
                process.exitCode = 1;
                console.error(result.error);
                await page.screenshot({ path: resolve(output, `${result.name}-failure.png`) }).catch(() => {});
            } finally { await context.close(); }
        }
    }
}
