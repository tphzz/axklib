import assert from 'node:assert/strict';
import { resolve } from 'node:path';

export async function mappingAuditionCases(browser, base, output, results) {
    const context = await browser.newContext({ viewport: { width: 1040, height: 680 } });
    const main = await context.newPage();
    const result = { name: 'held-key-audition', errors: [], autoplayOverride: false };
    results.push(result);
    context.on('page', page => page.on('pageerror', error => result.errors.push(error.message)));
    main.on('pageerror', error => result.errors.push(error.message));
    const audio = () => main.locator('[data-mapping-audio]').evaluate(node => JSON.parse(node.textContent));
    const mainState = () => main.locator('[data-mapping-main]').evaluate(node => JSON.parse(node.textContent));
    const waitAudio = status => main.waitForFunction(status => JSON.parse(document.querySelector('[data-mapping-audio]').textContent).state.status === status, status);
    try {
        await main.goto(`${base}/tools/layout-fixtures/mapping-window.html?audio&channel=audition`);
        await main.waitForFunction(() => JSON.parse(document.querySelector('[data-mapping-main]').textContent).ready);
        const opened = context.waitForEvent('page');
        await main.getByRole('button', { name: 'Mapping Editor', exact: true }).click();
        const child = await opened;
        await child.locator('.keyboard g[data-key="60"]').waitFor();
        const before = await mainState();
        const key = child.locator('.keyboard g[data-key="60"]');
        const face = key.locator('[data-note="60"]');
        const normalStroke = await face.evaluate(node => getComputedStyle(node).stroke);
        const box = await key.boundingBox();
        await child.mouse.move(box.x + box.width / 2, box.y + box.height - 3);
        await child.mouse.down();
        await waitAudio('playing');
        assert.equal(await key.evaluate(node => node.classList.contains('pressed')), true);
        assert.notEqual(await face.evaluate(node => getComputedStyle(node).stroke), normalStroke);
        result.firstKey = await audio();
        assert.equal(result.firstKey.outputState, 'running', 'The first auxiliary key must start the owning AudioContext without an autoplay override');
        assert.equal(result.firstKey.voices, 2, 'Every matching assignment must play, not just the selected one');
        assert.ok(result.firstKey.peak > .1, 'Real decoded PCM must be nonblank');
        assert.deepEqual(result.firstKey.notes, [{ note: 60, velocity: 100 }]);
        await child.waitForTimeout(600);
        assert.equal((await audio()).state.status, 'playing', 'A declared loop remains held beyond short one-shot preview duration');
        await child.mouse.up(); await waitAudio('idle');
        assert.equal(await key.evaluate(node => node.classList.contains('pressed')), false);
        assert.equal(await face.evaluate(node => getComputedStyle(node).stroke), normalStroke, 'Release clears pointer feedback even in the real held-audio workflow');
        assert.equal((await mainState()).assignment, before.assignment, 'Keyboard audition never changes selection');
        assert.deepEqual((await mainState()).values, before.values, 'Keyboard audition never changes roots or drafts');

        await main.getByRole('spinbutton', { name: 'Low velocity', exact: true }).fill('64');
        await child.waitForFunction(() => JSON.parse(document.querySelector('[data-mapping-snapshot]').textContent).state.limits.velocityLow === 64);
        await child.getByRole('spinbutton', { name: 'Audition velocity', exact: true }).fill('40');
        await key.focus(); await child.keyboard.down('Space'); await waitAudio('playing');
        result.velocityLayer = await audio();
        assert.equal(result.velocityLayer.voices, 1, 'Low velocity auditions the unselected matching assignment');
        assert.deepEqual(result.velocityLayer.notes.at(-1), { note: 60, velocity: 40 });
        await child.keyboard.up('Space'); await waitAudio('idle');
        await child.keyboard.down('Space'); await waitAudio('playing');
        await child.getByRole('spinbutton', { name: 'Audition velocity', exact: true }).focus(); await waitAudio('idle');
        await child.keyboard.up('Space');
        await child.screenshot({ path: resolve(output, 'held-key-audition.png') });
        await key.focus(); await child.keyboard.down('Space'); await waitAudio('playing');
        await child.close(); await waitAudio('idle');
        assert.deepEqual(result.errors, []);
        result.passed = true;
        console.log('held-key-audition: first output, all matching layers, velocity, loop, release and blur passed');
    } catch (error) {
        result.error = error.stack;
        result.lastAudio = await audio().catch(() => null);
        process.exitCode = 1;
        console.error(result.error);
    } finally { await context.close(); }
}
