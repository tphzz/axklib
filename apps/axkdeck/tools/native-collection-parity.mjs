import { selectValue } from './select-control.mjs';
import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

async function rowGeometry(row) {
    return row.evaluate((node) => {
        const bounds = node.getBoundingClientRect();
        const style = getComputedStyle(node);
        const listStyle = getComputedStyle(node.parentElement);
        const text = (selector) => {
            const element = node.querySelector(selector);
            const rect = element.getBoundingClientRect();
            const textStyle = getComputedStyle(element);
            return {
                fontSize: textStyle.fontSize,
                fontWeight: textStyle.fontWeight,
                lineHeight: textStyle.lineHeight,
                color: textStyle.color,
                fontFamily: textStyle.fontFamily,
                whiteSpace: textStyle.whiteSpace,
                textOverflow: textStyle.textOverflow,
                left: rect.left - bounds.left,
                right: rect.right - bounds.left,
                top: rect.top - bounds.top,
                bottom: rect.bottom - bounds.top,
                height: rect.height,
            };
        };
        return {
            height: bounds.height,
            width: bounds.width,
            padding: style.padding,
            borderBottomWidth: style.borderBottomWidth,
            borderBottomColor: style.borderBottomColor,
            list: {
                display: listStyle.display,
                gap: listStyle.gap,
                padding: listStyle.padding,
                overflowY: listStyle.overflowY,
            },
            primary: text('strong'),
            metadata: text('small'),
        };
    });
}

function checkGeometry(geometry, label) {
    assert.equal(geometry.height, 26, `${label}: compact row height`);
    assert.equal(geometry.primary.fontSize, '10px', `${label}: primary font size`);
    assert.equal(geometry.primary.fontWeight, '600', `${label}: primary weight`);
    assert.equal(geometry.primary.lineHeight, '10px', `${label}: primary line height`);
    assert.equal(geometry.metadata.fontSize, '8.5px', `${label}: metadata font size`);
    assert.equal(geometry.metadata.lineHeight, '9px', `${label}: metadata line height`);
    assert(geometry.primary.bottom <= geometry.metadata.top, `${label}: identity lines must not overlap`);
    for (const [name, bounds] of Object.entries({ primary: geometry.primary, metadata: geometry.metadata })) {
        assert(bounds.right > bounds.left && bounds.height > 0, `${label}: ${name} is visible`);
        assert(
            bounds.left >= 0 && bounds.right <= geometry.width && bounds.top >= 0 && bounds.bottom <= geometry.height,
            `${label}: ${name} must be fully contained: ${JSON.stringify(geometry)}`,
        );
    }
}

function typography(geometry) {
    return {
        height: geometry.height,
        padding: geometry.padding,
        borderBottomWidth: geometry.borderBottomWidth,
        borderBottomColor: geometry.borderBottomColor,
        list: geometry.list,
        ...Object.fromEntries(
            ['primary', 'metadata'].map((kind) => [
                kind,
                Object.fromEntries(
                    ['fontSize', 'fontWeight', 'lineHeight', 'color', 'fontFamily', 'whiteSpace', 'textOverflow'].map(
                        (property) => [property, geometry[kind][property]],
                    ),
                ),
            ]),
        ),
    };
}

async function checkSelection(page, rows, prefix) {
    const waitForFocus = (index) =>
        page.waitForFunction(
            (objectId) => document.activeElement?.getAttribute('data-collection-object-id') === objectId,
            `${prefix}-${index}`,
            { timeout: 5000 },
        );
    const selected = () =>
        rows.evaluateAll((nodes) =>
            nodes
                .filter((node) => node.getAttribute('aria-pressed') === 'true')
                .map((node) => node.dataset.collectionObjectId),
        );
    const first = rows.nth(0);
    const firstBounds = await first.boundingBox();
    await first.click({ position: { x: firstBounds.width - 4, y: 3 } });
    assert.deepEqual(await selected(), [`${prefix}-0`], `${prefix}: blank row padding selects`);
    await rows.nth(2).locator('small').click();
    assert.deepEqual(await selected(), [`${prefix}-2`], `${prefix}: metadata selects`);
    await first.locator('strong').click();
    await rows.nth(2).click({ modifiers: ['Control'] });
    assert.deepEqual(await selected(), [`${prefix}-0`, `${prefix}-2`], `${prefix}: Ctrl-click adds`);
    await rows.nth(4).click({ modifiers: ['Shift'] });
    assert.deepEqual(
        await selected(),
        [2, 3, 4].map((index) => `${prefix}-${index}`),
        `${prefix}: Shift-click range`,
    );
    await first.click();
    await page.keyboard.press('ArrowDown');
    assert.deepEqual(await selected(), [`${prefix}-1`], `${prefix}: keyboard next`);
    await waitForFocus(1);
    await page.keyboard.press('Shift+ArrowDown');
    assert.deepEqual(await selected(), [`${prefix}-1`, `${prefix}-2`], `${prefix}: keyboard range`);
    await waitForFocus(2);
    await page.keyboard.press('End');
    assert.deepEqual(await selected(), [`${prefix}-39`], `${prefix}: keyboard scrolls to end`);
    await page
        .waitForFunction(
            (objectId) => {
                const node = document.querySelector(`[data-collection-object-id="${objectId}"]`);
                if (!node) return false;
                const bounds = node.getBoundingClientRect();
                const viewport = node.parentElement.getBoundingClientRect();
                return (
                    node === document.activeElement && bounds.top >= viewport.top && bounds.bottom <= viewport.bottom
                );
            },
            `${prefix}-39`,
            { timeout: 5000 },
        )
        .catch(async (error) => {
            const state = await rows.last().evaluate((node) => ({
                focused: document.activeElement?.outerHTML,
                row: node.getBoundingClientRect().toJSON(),
                list: node.parentElement.getBoundingClientRect().toJSON(),
                clientHeight: node.parentElement.clientHeight,
                scrollTop: node.parentElement.scrollTop,
                scrollHeight: node.parentElement.scrollHeight,
            }));
            throw new Error(`${prefix}: End must focus and reveal the final row: ${JSON.stringify(state)}`, {
                cause: error,
            });
        });
    await page.keyboard.press('Home');
    assert.deepEqual(await selected(), [`${prefix}-0`], `${prefix}: keyboard returns to start`);
    await waitForFocus(0);
    await page.mouse.move(0, 0);
    await page.waitForTimeout(150);
    return first.evaluate((node) => {
        const style = getComputedStyle(node);
        return {
            backgroundImage: style.backgroundImage,
            backgroundColor: style.backgroundColor,
            borderColor: style.borderColor,
            borderRadius: style.borderRadius,
        };
    });
}

export async function checkNativeCollectionParity(page, output, width, scale) {
    const selector = page.getByRole('combobox', { name: 'Image', exact: true });
    const results = [];
    await selectValue(selector, 'source-1');
    const programs = page.getByRole('region', { name: 'Programs', exact: true });
    const programRows = programs.locator('[data-collection-object-id]');
    await programRows.nth(39).waitFor({ state: 'attached' });
    const reference = await rowGeometry(programRows.first());
    checkGeometry(reference, 'A-series Programs');
    const referenceLong = await rowGeometry(programRows.nth(1));
    checkGeometry(referenceLong, 'A-series Programs long identity');
    assert(
        await programRows
            .nth(1)
            .getByText('a4k/a5k', { exact: true })
            .evaluate((node) => {
                const badge = node.getBoundingClientRect();
                const row = node.closest('[data-collection-object-id]').getBoundingClientRect();
                return badge.width > 0 && badge.height === 13 && badge.top >= row.top && badge.bottom <= row.bottom;
            }),
        'A-series format badge remains visible and contained beside a truncated name',
    );
    for (const text of ['strong', 'small']) {
        assert(
            await programRows
                .nth(1)
                .locator(text)
                .evaluate((node) => node.scrollWidth > node.clientWidth),
            `A-series Programs: long ${text === 'strong' ? 'name' : 'metadata'} truncates`,
        );
        assert(await programRows.nth(1).locator(text).getAttribute('title'), 'Truncated A-series text has a tooltip');
    }
    const selection = await checkSelection(page, programRows, 'program');
    assert.notEqual(selection.backgroundImage, 'none');
    const programSearch = programs.getByRole('searchbox', { name: 'Search Programs' });
    await programSearch.focus();
    const toolbar = await programSearch.boundingBox();
    const header = await page.locator('.app-header').boundingBox();
    assert(
        toolbar.y >= header.y + header.height,
        'The actual Program workspace toolbar remains visible after navigation',
    );
    await page.screenshot({ path: resolve(output, `programs-${width}-${scale}.png`) });
    await programSearch.fill('no matching collection fixture');
    await programs.getByText('No matching Programs', { exact: true }).waitFor();
    assert.equal(await programRows.count(), 0);
    await programSearch.fill('');
    await selectValue(selector, 'source-2');
    const tabs = page.getByRole('navigation', { name: 'Workspace views' });
    const collection = page.getByRole('region', { name: 'TX16W native objects' });
    for (const [label, kind] of [
        ['Performances', 'performance'],
        ['Voices', 'voice'],
        ['Timbres', 'timbre'],
        ['Setups', 'setup'],
    ]) {
        await tabs.getByRole('button', { name: label, exact: true }).click();
        const rows = collection.locator('[data-collection-object-id]');
        await rows.nth(39).waitFor({ state: 'attached' });
        const geometry = await rowGeometry(rows.first());
        const longGeometry = await rowGeometry(rows.nth(1));
        await writeFile(
            resolve(output, `row-${kind}-${width}-${scale}.json`),
            `${JSON.stringify({ reference, referenceLong, geometry, longGeometry }, null, 2)}\n`,
        );
        checkGeometry(geometry, `TX16W ${label}`);
        checkGeometry(longGeometry, `TX16W ${label} long identity`);
        assert.deepEqual(typography(geometry), typography(reference), `${label}: same row appearance as A-series`);
        assert(
            await rows
                .nth(1)
                .locator('strong')
                .evaluate((node) => node.scrollWidth > node.clientWidth),
            `${label}: long names truncate inside the available width`,
        );
        assert(await rows.nth(1).locator('strong').getAttribute('title'), `${label}: full name tooltip`);
        assert(await rows.nth(1).locator('small').getAttribute('title'), `${label}: full metadata tooltip`);
        assert.deepEqual(await checkSelection(page, rows, kind), selection, `${label}: shared selected appearance`);
        const search = collection.getByRole('searchbox', { name: `Search ${label}` });
        await search.focus();
        await page.screenshot({ path: resolve(output, `${kind}-${width}-${scale}.png`) });
        await search.fill('no matching collection fixture');
        await collection.getByText('No matching objects', { exact: true }).waitFor();
        assert.equal(await rows.count(), 0);
        await search.fill('');
        await rows.nth(39).waitFor({ state: 'attached' });
        results.push({ kind, count: await rows.count(), geometry, longGeometry });
    }
    await tabs.getByRole('button', { name: 'Performances', exact: true }).click();
    assert.equal(
        await collection.locator('[data-collection-object-id="performance-0"]').getAttribute('aria-pressed'),
        'true',
    );
    await selectValue(selector, 'source-1');
    assert.equal(
        await programRows.first().getAttribute('aria-pressed'),
        'true',
        'A-series selection survives source switching',
    );
    await selectValue(selector, 'source-2');
    return { reference, referenceLong, selection, tabs: results };
}
