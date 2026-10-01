import assert from 'node:assert/strict';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ? pathToFileURL(process.env.PLAYWRIGHT_MODULE).href : 'playwright');
const base = process.argv[2] ?? 'http://127.0.0.1:5191';
const output = resolve(process.argv[3] ?? '../../../build/logs/sample-editor/00007');
const scope = process.env.REFINEMENT_SCOPE ?? 'all';
await mkdir(output, { recursive: true });
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH, headless: true });
const results = [];

async function run(name, test, { width = 1920, height = 620, zoom = 1 } = {}) {
    if (process.env.REFINEMENT_CASE && !new RegExp(process.env.REFINEMENT_CASE).test(name)) return;
    const page = await browser.newPage({ viewport: { width, height } });
    page.setDefaultTimeout(10000);
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const measurements = {};
    try {
        await page.goto(`${base}/tools/layout-fixtures/sample-editor.html`);
        await page.getByRole('button', { name: 'Save', exact: true }).waitFor();
        await page.evaluate(zoom => {
            document.documentElement.style.zoom = String(zoom);
            document.querySelector('main').style.height = `${innerHeight / zoom - 40}px`;
        }, zoom);
        await test(page, measurements);
        const footer = await page.locator('.sample-transport').boundingBox();
        assert(footer && footer.y + footer.height <= height + 1, 'persistent transport stays inside the viewport');
        assert(await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth + 1), 'no horizontal document overflow');
        assert.deepEqual(errors, [], 'no browser exceptions');
        assert.equal(await page.getByLabel('Write count').textContent(), '0', 'editing never writes before Save');
        results.push({ name, status: 'passed', width, height, zoom, measurements });
        console.log(`PASS ${name}`);
    } catch (error) {
        results.push({ name, status: 'failed', width, height, zoom, measurements, error: error.message, browserErrors: errors });
        console.error(`FAIL ${name}: ${error.message}`);
        process.exitCode = 1;
    } finally {
        await page.screenshot({ path: resolve(output, `refinement-${name}.png`) });
        await page.close();
    }
}

async function subpage(page, tab, name) {
    await page.getByRole('tab', { name: tab, exact: true }).click();
    if (name) await page.getByLabel('Sample subpages', { exact: true }).getByRole('button', { name, exact: true }).click();
}

async function drawing(graph) {
    return graph.evaluate(async node => {
        await new Promise(requestAnimationFrame);
        return JSON.stringify({
            paths: [...node.querySelectorAll('path:not(.grid)')].map(path => path.getAttribute('d')),
            canvases: [...node.querySelectorAll('canvas')].map(canvas => canvas.toDataURL()),
        });
    });
}

async function undo(page) {
    await page.getByRole('button', { name: 'Undo Sample edit' }).click();
}

async function drag(page, handle, dx, dy = 0) {
    const box = await handle.boundingBox();
    assert(box, 'graph handle is rendered');
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await page.mouse.move(box.x + box.width / 2 + dx, box.y + box.height / 2 + dy, { steps: 5 });
    await page.mouse.up();
}

try {
    for (const [width, height, zoom] of [[1920, 520, 1], [1920, 620, 1.25], [1280, 620, 1.5], [800, 620, 1], [390, 740, 1]]) {
        await run(`waveform-edge-${width}-${zoom}`, async (page, measurements) => {
            await page.getByRole('spinbutton', { name: 'Loop end', exact: true }).fill('83996');
            await page.getByRole('spinbutton', { name: 'Loop start', exact: true }).fill('83995');
            measurements.geometry = await page.evaluate(() => {
                const panel = document.querySelector('.sample-panel');
                const surface = document.querySelector('.wave-surface').getBoundingClientRect();
                return {
                    panelWidth: panel.clientWidth,
                    panelScrollWidth: panel.scrollWidth,
                    surfaceLeft: surface.left,
                    surfaceRight: surface.right,
                    markers: [...document.querySelectorAll('.wave-marker > span')].map(node => {
                        const rect = node.getBoundingClientRect();
                        return { label: node.textContent, left: rect.left, right: rect.right };
                    }),
                };
            });
            const geometry = measurements.geometry;
            assert(geometry.panelScrollWidth <= geometry.panelWidth + 1, `waveform panel overflows by ${geometry.panelScrollWidth - geometry.panelWidth}px`);
            for (const marker of geometry.markers) {
                assert(marker.left >= geometry.surfaceLeft - 1 && marker.right <= geometry.surfaceRight + 1, `${marker.label} label leaves waveform bounds`);
            }
        }, { width, height, zoom });
    }

    await run('waveform-left-edge', async (page, measurements) => {
        await page.getByRole('spinbutton', { name: 'Loop start', exact: true }).fill('0');
        await page.getByRole('spinbutton', { name: 'Loop end', exact: true }).fill('1');
        measurements.markers = await page.locator('.wave-marker > span').evaluateAll(nodes => {
            const surface = document.querySelector('.wave-surface').getBoundingClientRect();
            return nodes.map(node => {
                const rect = node.getBoundingClientRect();
                return { label: node.textContent, left: rect.left, right: rect.right, minimum: surface.left, maximum: surface.right };
            });
        });
        for (const marker of measurements.markers) {
            assert(marker.left >= marker.minimum - 1 && marker.right <= marker.maximum + 1, `${marker.label} label leaves waveform bounds at the left edge`);
        }
    });

    await run('numeric-suffix-centering', async (page, measurements) => {
        await subpage(page, 'Map/Out', 'Level scaling');
        measurements.suffixes = await page.locator('.editor-value.has-unit').evaluateAll(nodes => nodes.map(node => {
            const input = node.querySelector('input').getBoundingClientRect();
            const suffix = node.querySelector('span').getBoundingClientRect();
            return { label: node.querySelector('input').getAttribute('aria-label'), difference: Math.abs(input.y + input.height / 2 - suffix.y - suffix.height / 2) };
        }));
        assert(measurements.suffixes.length >= 2);
        assert(measurements.suffixes.every(suffix => suffix.difference <= 1), JSON.stringify(measurements.suffixes));
    }, { zoom: 1.25 });

    await run('single-divider-line', async (page, measurements) => {
        await subpage(page, 'Map/Out', 'Level scaling');
        const divider = page.getByRole('separator', { name: 'Resize graph and controls' });
        measurements.divider = await divider.evaluate(node => {
            const style = getComputedStyle(node);
            return { leftBorder: parseFloat(style.borderLeftWidth), rightBorder: parseFloat(style.borderRightWidth), width: node.getBoundingClientRect().width };
        });
        assert(!(measurements.divider.leftBorder > 0 && measurements.divider.rightBorder > 0), 'splitter paints two edge lines');
        assert(measurements.divider.width >= 8, 'splitter retains the shared 8px hit area');
        await divider.focus();
        const initial = Number(await divider.getAttribute('aria-valuenow'));
        await divider.press('ArrowRight');
        assert(Number(await divider.getAttribute('aria-valuenow')) > initial, 'splitter remains keyboard adjustable');
        assert(await page.getByRole('button', { name: 'Save', exact: true }).isDisabled(), 'layout adjustments are not sample edits');
    });

    await run('slider-keyboard-focus', async (page, measurements) => {
        await subpage(page, 'Map/Out', 'Expansion & Velocity');
        const slider = page.getByRole('slider', { name: 'Detune slider', exact: true });
        await slider.focus();
        await slider.press('ArrowRight');
        measurements.focus = await slider.evaluate(node => {
            const style = getComputedStyle(node);
            return { outlineWidth: parseFloat(style.outlineWidth), outlineStyle: style.outlineStyle, focusVisible: node.matches(':focus-visible'), value: node.value };
        });
        assert(measurements.focus.focusVisible, 'keyboard focus remains detectable');
        assert(measurements.focus.outlineWidth === 0 || measurements.focus.outlineStyle === 'none', 'keyboard focus should not outline the full slider rectangle');
        assert.equal(measurements.focus.value, '1', 'focused slider stays keyboard editable');
        assert.equal(await page.locator('.editor-heading').first().evaluate(node => getComputedStyle(node).userSelect), 'none', 'form headings do not create accidental selections');
        assert.notEqual(await page.getByRole('spinbutton', { name: 'Detune', exact: true }).evaluate(node => getComputedStyle(node).userSelect), 'none', 'numeric text remains selectable and editable');
    });

    if (scope !== 'layout') {
        await run('vertical-velocity-range', async (page, measurements) => {
            await subpage(page, 'Map/Out', 'Expansion & Velocity');
            const low = page.getByRole('slider', { name: 'Low velocity boundary', exact: true });
            const high = page.getByRole('slider', { name: 'High velocity boundary', exact: true });
            assert.equal(await low.getAttribute('aria-orientation'), 'vertical');
            assert.equal(await high.getAttribute('aria-orientation'), 'vertical');
            measurements.range = await low.evaluate(node => {
                const group = node.closest('[role="group"]');
                const box = group.getBoundingClientRect();
                return { width: box.width, height: box.height, text: group.textContent };
            });
            assert(measurements.range.height <= 130, 'velocity range stays compact');
            assert.match(measurements.range.text, /soft/i);
            assert.match(measurements.range.text, /hard/i);
            assert.equal(await page.getByRole('slider', { name: 'Low velocity slider', exact: true }).count(), 0, 'no duplicated boundary slider');
            assert.equal(await page.getByRole('slider', { name: 'High velocity slider', exact: true }).count(), 0, 'no duplicated boundary slider');
            await low.focus();
            await low.press('ArrowUp');
            assert.equal(await page.getByRole('spinbutton', { name: 'Low velocity', exact: true }).inputValue(), '1');
            await undo(page);
            assert.equal(await low.getAttribute('aria-valuenow'), '0');
            await page.getByRole('spinbutton', { name: 'Low velocity', exact: true }).fill('45');
            assert.equal(await low.getAttribute('aria-valuenow'), '45', 'forms update the vertical band');
            await undo(page);
            assert(await page.getByRole('button', { name: 'Save', exact: true }).isDisabled());
        });

        await run('filter-and-eq-graphs', async (page, measurements) => {
            await subpage(page, 'Filter', 'Sample EQ');
            const eq = page.getByRole('group', { name: 'Sample EQ response', exact: true });
            await eq.waitFor();
            const neutral = await drawing(eq);
            await subpage(page, 'Map/Out');
            await page.getByRole('spinbutton', { name: 'Pan', exact: true }).fill('3');
            await subpage(page, 'Filter', 'Sample EQ');
            assert.equal(await drawing(eq), neutral, 'unrelated draft edits preserve the stored EQ response');
            await undo(page);
            await page.getByRole('spinbutton', { name: 'EQ gain (dB)', exact: true }).fill('8');
            const boosted = await drawing(eq);
            assert.notEqual(boosted, neutral, 'EQ gain updates the response');
            await page.getByRole('spinbutton', { name: 'EQ frequency selection', exact: true }).fill('38');
            assert.notEqual(await drawing(eq), boosted, 'frequency updates the response');
            await page.getByRole('button', { name: 'EQ type: Low shelf', exact: true }).click();
            const lowShelf = await drawing(eq);
            await page.getByRole('button', { name: 'EQ type: High shelf', exact: true }).click();
            assert.notEqual(await drawing(eq), lowShelf, 'the shelf directions are distinct');
            assert(await page.getByRole('spinbutton', { name: 'EQ width', exact: true }).isDisabled(), 'shelf width is inactive');
            await page.getByRole('button', { name: 'EQ type: Peak/Dip', exact: true }).click();
            assert(!(await page.getByRole('spinbutton', { name: 'EQ width', exact: true }).isDisabled()));
            const widthInput = page.getByRole('spinbutton', { name: 'EQ width', exact: true });
            const widthBefore = await widthInput.inputValue();
            await eq.locator('[data-handle]').hover();
            await page.mouse.wheel(0, -100);
            await page.waitForTimeout(220);
            assert.notEqual(await widthInput.inputValue(), widthBefore, 'wheel over the EQ handle edits bandwidth');
            await undo(page);
            assert.equal(await widthInput.inputValue(), widthBefore, 'one undo restores the bandwidth gesture');
            const handles = eq.getByRole('button');
            assert.equal(await handles.count(), 1, 'EQ exposes one frequency/gain handle');
            const gainBefore = await page.getByRole('spinbutton', { name: 'EQ gain (dB)', exact: true }).inputValue();
            await handles.first().focus();
            await handles.first().press('ArrowUp');
            assert.notEqual(await page.getByRole('spinbutton', { name: 'EQ gain (dB)', exact: true }).inputValue(), gainBefore, 'EQ graph edits synchronize to the form');
            await undo(page);
            assert.equal(await page.getByRole('spinbutton', { name: 'EQ gain (dB)', exact: true }).inputValue(), gainBefore);
            measurements.eq = await eq.boundingBox();
            await page.getByRole('button', { name: 'Filter', exact: true }).click();
            const filter = page.getByRole('group', { name: 'Filter response', exact: true });
            await filter.waitFor();
            const bypass = await drawing(filter);
            await page.getByRole('button', { name: 'Filter type', exact: true }).click();
            await page.getByRole('option', { name: 'LowPass1', exact: true }).click();
            assert.notEqual(await drawing(filter), bypass, 'filter type changes the schematic response');
            const first = await drawing(filter);
            await page.getByRole('spinbutton', { name: 'Cutoff', exact: true }).fill('64');
            assert.notEqual(await drawing(filter), first, 'cutoff updates the schematic response');
            await drag(page, filter.getByRole('button', { name: /^Filter cutoff \/ Q:/ }), 24, -12);
            assert.notEqual(await page.getByRole('spinbutton', { name: 'Cutoff', exact: true }).inputValue(), '64', 'filter graph dragging edits cutoff');
            await undo(page);
            assert.equal(await page.getByRole('spinbutton', { name: 'Cutoff', exact: true }).inputValue(), '64', 'one undo restores the complete filter gesture');
            for (const type of ['Bypass', 'LowPass1', 'LowPass2', 'HiPass1', 'HiPass2', 'BandPass', 'BandElim', 'LowPass3', 'Peak1', 'Peak2', '2Peaks', '2Dips', 'DualLPFs', 'LPF+Peak', 'DualHPFs', 'HPF+Peak', 'LPF+HPF']) {
                await page.getByRole('button', { name: 'Filter type', exact: true }).click();
                await page.getByRole('option', { name: type, exact: true }).click();
                const response = await filter.locator('[data-trace="filter"]').getAttribute('d');
                assert(response.length > 20 && !/NaN|Infinity/.test(response), `${type} renders a finite schematic response`);
            }
            measurements.filter = await filter.boundingBox();
            await page.getByRole('button', { name: 'Sample EQ', exact: true }).click();
            assert.equal(await page.getByRole('spinbutton', { name: 'EQ gain (dB)', exact: true }).inputValue(), gainBefore, 'subpage changes retain the EQ draft');
        });

        await run('envelope-rate-editing', async (page, measurements) => {
            await subpage(page, 'EG', 'Amplitude');
            for (const name of ['Amplitude', 'Filter', 'Pitch']) {
                await page.getByRole('button', { name, exact: true }).click();
                const graph = page.getByRole('group', { name: /Envelope/ });
                const attack = page.getByRole('spinbutton', { name: 'Attack rate', exact: true });
                await attack.fill('32');
                const slow = await drawing(graph);
                await attack.fill('96');
                assert.notEqual(await drawing(graph), slow, `${name} rate affects the graph`);
                const handle = graph.getByRole('button', { name: /^(Peak|Attack|Attack rate):/ }).first();
                await handle.focus();
                await handle.press('ArrowRight');
                assert.notEqual(await attack.inputValue(), '96', `${name} graph can edit attack rate`);
                await undo(page);
                assert.equal(await attack.inputValue(), '96', 'graph rate edit is one undo step');
                await drag(page, handle, 24);
                assert.notEqual(await attack.inputValue(), '96', `${name} pointer dragging edits the rate`);
                await undo(page);
                assert.equal(await attack.inputValue(), '96', 'one undo restores the complete rate drag');
                await page.getByRole('button', { name: 'Fit envelope to width' }).click();
                const envelopePath = await graph.locator('[data-trace="envelope"]').getAttribute('d');
                assert.match(envelopePath, /^M0,/, 'envelope begins at the left edge');
                assert.match(envelopePath, /L1000,[\d.e+-]+$/, 'envelope reaches the right edge');
                measurements[name] = await graph.boundingBox();
            }
        });

        await run('lfo-delay-speed-overlays', async (page, measurements) => {
            await subpage(page, 'LFO');
            const graph = page.locator('.lfo-plot');
            await page.getByRole('spinbutton', { name: 'Pitch depth', exact: true }).fill('90');
            await page.getByRole('spinbutton', { name: 'Amplitude depth', exact: true }).fill('50');
            const first = await drawing(graph);
            await page.getByRole('spinbutton', { name: 'Speed', exact: true }).fill('80');
            assert.notEqual(await drawing(graph), first, 'speed changes cycle spacing');
            const faster = await drawing(graph);
            await page.getByRole('spinbutton', { name: 'Delay', exact: true }).fill('85');
            assert.notEqual(await drawing(graph), faster, 'delay shifts and shapes the modulation buildup');
            measurements.traces = await graph.locator('.modulation').count();
            assert(measurements.traces >= 2, 'pitch and amplitude are simultaneously visible');
            const delayed = await drawing(graph);
            const amplitude = await graph.locator('[data-trace="amp"]').getAttribute('d');
            await page.getByRole('switch', { name: 'Invert pitch phase', exact: true }).click();
            assert.notEqual(await drawing(graph), delayed, 'phase inversion updates the overlay');
            assert.equal(await graph.locator('[data-trace="amp"]').getAttribute('d'), amplitude, 'pitch inversion does not invert the amplitude trace');
            await undo(page);
            assert.equal(await drawing(graph), delayed);
            const speed = await page.getByRole('spinbutton', { name: 'Speed', exact: true }).inputValue();
            await page.getByRole('button', { name: 'Wave: S & H', exact: true }).click();
            assert.equal(await page.getByRole('textbox', { name: 'Speed', exact: true }).inputValue(), 'Program');
            assert(await page.getByRole('textbox', { name: 'Speed', exact: true }).isDisabled());
            await undo(page);
            assert.equal(await page.getByRole('spinbutton', { name: 'Speed', exact: true }).inputValue(), speed, 'Sample & Hold preserves the stored sample speed');
        });

        await run('midi-search-clear-and-keyboard', async (page, measurements) => {
            await subpage(page, 'MIDI/CTRL');
            const save = page.getByRole('button', { name: 'Save', exact: true });
            const receive = page.getByRole('combobox', { name: 'Receive channel', exact: true });
            await receive.focus();
            assert.equal(await page.getByRole('listbox').count(), 0, 'automatic focus does not open choices');
            await page.getByRole('button', { name: 'Clear Receive channel', exact: true }).click();
            assert.equal(await receive.inputValue(), '');
            assert(await receive.evaluate(node => document.activeElement === node));
            assert(await page.getByRole('listbox').isVisible());
            assert(await save.isDisabled(), 'clearing a search preserves the assignment');
            await receive.press('Escape');
            assert.equal(await receive.inputValue(), 'A01');
            assert.equal(await page.getByRole('listbox').count(), 0);
            await subpage(page, 'MIDI/CTRL', 'Control');
            for (let index = 1; index <= 6; index++) {
                for (const label of ['Controller', 'Function', 'Type']) assert(await page.getByRole('combobox', { name: `Control ${index} ${label}`, exact: true }).isVisible());
            }
            const controller = page.getByRole('combobox', { name: 'Control 6 Controller', exact: true });
            await controller.fill('74');
            await page.getByRole('option', { name: 'CC 074', exact: true }).click();
            assert.equal(await controller.inputValue(), 'CC 074');
            assert(!(await save.isDisabled()));
            await page.getByRole('button', { name: 'Clear Control 6 Controller', exact: true }).click();
            assert(await page.getByRole('option', { name: 'CC 000', exact: true }).isVisible(), 'clear restores the unfiltered option list');
            await controller.fill('no matching controller');
            assert.equal(await page.getByRole('option').count(), 0);
            await page.getByRole('columnheader', { name: 'Range', exact: true }).click();
            assert.equal(await controller.inputValue(), 'CC 074', 'outside dismissal restores the assigned label');
            await undo(page);
            assert(await save.isDisabled(), 'searching and clearing add no undo edits');
            const fn = page.getByRole('combobox', { name: 'Control 6 Function', exact: true });
            await fn.fill('attack rate');
            await fn.press('Home');
            await fn.press('Enter');
            assert.equal(await fn.inputValue(), 'AEG Attack Rate', 'keyboard selects a filtered function');
            measurements.function = await fn.inputValue();
            await undo(page);
            assert(await save.isDisabled());
        });

        for (const [width, height, zoom] of [[1920, 620, 1.5], [800, 620, 1], [390, 740, 1]]) {
            await run(`compact-graphs-${width}-${zoom}`, async (page, measurements) => {
                measurements.graphs = [];
                for (const [tab, name] of [['Filter', 'Filter'], ['Filter', 'Sample EQ'], ['EG', 'Amplitude'], ['EG', 'Filter'], ['EG', 'Pitch'], ['LFO', null]]) {
                    await subpage(page, tab, name);
                    await page.locator('.graph-region .plot-handle:not(:disabled)').evaluateAll(handles => {
                        const rightmost = handles.sort((left, right) => left.getBoundingClientRect().x - right.getBoundingClientRect().x).at(-1);
                        rightmost?.focus();
                    });
                    const geometry = await page.locator('.graph-region').evaluate(node => {
                        const frame = node.querySelector('.graph-frame').getBoundingClientRect();
                        const plot = node.querySelector('.graph-surface').getBoundingClientRect();
                        const axis = node.querySelector('.axis-y').getBoundingClientRect();
                        const ticks = [...node.querySelectorAll('.axis-y span')].map(tick => {
                            const range = document.createRange();
                            range.selectNodeContents(tick);
                            const lines = [...range.getClientRects()];
                            return { label: tick.textContent, lineCount: lines.length, fits: lines.every(line => line.left >= axis.left - 1 && line.right <= axis.right + 1) };
                        });
                        const readout = node.querySelector('.graph-readout output');
                        const readoutBox = readout?.getBoundingClientRect();
                        const readoutHost = readout?.parentElement.getBoundingClientRect();
                        return { label: `${node.querySelector('.graph-frame').getAttribute('aria-label')}`, width: frame.width, height: frame.height, plotHeight: plot.height, clientWidth: node.clientWidth, scrollWidth: node.scrollWidth, ticks,
                            readout: readout ? { text: readout.textContent, left: readoutBox.left, right: readoutBox.right, minimum: readoutHost.left, maximum: readoutHost.right } : null };
                    });
                    if (geometry.readout) assert(geometry.readout.left >= geometry.readout.minimum - 1 && geometry.readout.right <= geometry.readout.maximum + 1, `${tab}/${name ?? tab} focused readout stays inside its graph: ${JSON.stringify(geometry.readout)}`);
                    assert(geometry.width > 120 && geometry.plotHeight >= 48 * zoom, `${tab}/${name ?? tab} keeps a usable graph`);
                    assert(geometry.scrollWidth <= geometry.clientWidth + 1, `${tab}/${name ?? tab} graph does not overflow horizontally`);
                    assert(geometry.ticks.every(tick => tick.lineCount <= 1 && tick.fits), `${tab}/${name ?? tab} axis ticks fit on one line: ${JSON.stringify(geometry.ticks)}`);
                    measurements.graphs.push(geometry);
                    await page.screenshot({ path: resolve(output, `refinement-compact-${width}-${zoom}-${tab}-${name ?? tab}.png`) });
                }
                await subpage(page, 'MIDI/CTRL', 'Control');
                const type = page.getByRole('combobox', { name: 'Control 6 Type', exact: true });
                await type.click();
                const popup = await page.getByRole('listbox', { name: 'Control 6 Type', exact: true }).boundingBox();
                assert(popup.x >= 0 && popup.y >= 0 && popup.x + popup.width <= width + 1 && popup.y + popup.height <= height + 1, 'autocomplete popup fits scaled/narrow viewport');
                await type.press('Escape');
                assert(await page.getByRole('button', { name: 'Save', exact: true }).isDisabled(), 'navigation and autocomplete browsing do not dirty the sample');
            }, { width, height, zoom });
        }
    }
} finally {
    await writeFile(resolve(output, `refinement-results-${scope}.json`), JSON.stringify(results, null, 2) + '\n');
    await browser.close();
}
