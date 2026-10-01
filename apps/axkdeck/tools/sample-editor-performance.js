// Synthetic high-rate pointer input, identical in Chromium and native WebKitGTK.
// Capture is stubbed only because synthetic pointer IDs are not active OS pointers.
window.runSampleEditorRegression = async function () {
    const settle = () => new Promise(resolve => setTimeout(resolve, 100));
    const click = async (selector, label) => {
        [...document.querySelectorAll(selector)].find(node => node.textContent.trim() === label).click();
        await settle();
    };
    const toggle = document.querySelector('[aria-label="Editor panel"]');
    if (!document.querySelector('.device-editor') && toggle) { toggle.click(); await settle(); }
    document.querySelector('.audition-button').click();
    await settle();
    const measurements = [];
    for (const [tab, subpage, selector] of [
        ['Filter', 'Sample EQ', '[data-handle="frequency-gain"]'],
        ['Map/Out', 'Level scaling', '.breakpoint-graph button'],
        ['EG', 'Amplitude', '[data-handle="1"]'],
    ]) {
        await click('[role=tab]', tab);
        await click('[aria-label="Sample subpages"] button', subpage);
        const handle = document.querySelector(selector), rect = handle.getBoundingClientRect();
        handle.setPointerCapture = () => {};
        handle.hasPointerCapture = () => false;
        const dispatch = (type, x, y) => handle.dispatchEvent(new PointerEvent(type, {
            bubbles: true, cancelable: true, pointerId: 71, clientX: rect.x + 6 + x, clientY: rect.y + 6 + y,
        }));
        const frames = [];
        let last;
        dispatch('pointerdown', 0, 0);
        for (let i = 0; i < 180; i++) {
            await new Promise(resolve => requestAnimationFrame(now => {
                if (last !== undefined) frames.push(now - last);
                last = now;
                for (let sample = 0; sample < 4; sample++) {
                    const phase = (i + sample / 4) / 15;
                    dispatch('pointermove', Math.sin(phase) * 100, Math.cos(phase) * 30);
                }
                resolve();
            }));
        }
        dispatch('pointerup', 0, 0);
        frames.sort((a, b) => a - b);
        measurements.push({ subpage, frames: frames.length, p50: frames[Math.floor(frames.length * .5)],
            p95: frames[Math.floor(frames.length * .95)], over33ms: frames.filter(ms => ms > 33.4).length });
    }
    return { failures: [], measurements };
};
