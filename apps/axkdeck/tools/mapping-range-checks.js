window.runMappingRangeChecks = async function () {
    const failures = [], measurements = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const settle = async () => { await new Promise(requestAnimationFrame); await new Promise(requestAnimationFrame); };
    for (let attempt = 0; attempt < 40 && !document.querySelector('.full .zone'); attempt++) await new Promise(resolve => setTimeout(resolve, 100));
    const root = document.querySelector('.full .keyboard-mapping');
    if (!root) throw new Error('Missing full mapping fixture');
    const selected = () => root.querySelector('.zone.chosen');
    const limits = () => root.querySelector('.limits');
    const outline = () => root.querySelector('.selection-outline');
    const plot = root.querySelector('.plot-content');
    const neighbor = root.querySelectorAll('.zone')[1];
    const neighborStyle = neighbor.getAttribute('style');
    let gesture;
    const begin = async (name, dx, dy) => {
        const handle = root.querySelector(`[aria-label="${name}"]`), rect = handle.getBoundingClientRect(), box = plot.getBoundingClientRect();
        const x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
        // Synthetic pointers have no native capture; scope the substitute to this test handle.
        handle.setPointerCapture = () => {};
        gesture = { handle, x: x + box.width * dx / 128, y: y + box.height * dy / 128 };
        handle.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, pointerId: 71, button: 0, buttons: 1, clientX: x, clientY: y }));
        handle.dispatchEvent(new PointerEvent('pointermove', { bubbles: true, pointerId: 71, buttons: 1, clientX: gesture.x, clientY: gesture.y }));
        await settle();
    };
    const end = async (cancel = false) => {
        if (cancel) window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        gesture.handle.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, pointerId: 71, clientX: gesture.x, clientY: gesture.y }));
        delete gesture.handle.setPointerCapture;
        await settle();
    };
    const aligned = () => {
        const fill = selected().getBoundingClientRect(), edge = limits().getBoundingClientRect(), highlight = outline().getBoundingClientRect();
        measurements.push({ fill: fill.toJSON(), limits: edge.toJSON(), highlight: highlight.toJSON() });
        check(['left', 'right', 'top', 'bottom'].every(key => Math.abs(fill[key] - edge[key]) < 1 && Math.abs(fill[key] - highlight[key]) < 1), 'Fill, editable limits and highlight share the same edges');
        check(neighbor.getAttribute('style') === neighborStyle, 'A local preview does not change its neighbor');
    };
    await begin('High velocity limit', 0, 43);
    check(selected().style.top === '33.59375%', 'The live fill ends at velocity 84');
    check(root.querySelector('[data-boundary="velocityHigh"]').getAttribute('y1') === '43', 'High-velocity guide follows the preview');
    check(root.querySelector('.mapping-label.chosen').getBoundingClientRect().top >= selected().getBoundingClientRect().top, 'Selected label follows the fill');
    aligned();
    const box = plot.getBoundingClientRect();
    check(!document.elementFromPoint(box.x + box.width * 8 / 128, box.y + box.height / 10).classList.contains('zone'), 'The area above a lowered range is not a stale hit target');
    await end(); aligned();
    check(selected().style.top === '33.59375%', 'Committed fill retains its stored velocity range');
    await begin('Low velocity limit', 0, -20);
    check(selected().style.height === '50.78125%', 'The live fill starts at velocity 20');
    aligned(); await end(true);
    check(selected().style.height === '66.40625%', 'Escape restores the stored lower edge');
    await begin('Low key limit', 5, 0);
    check(selected().style.left === '3.90625%', 'The live fill follows key 5');
    aligned(); await end(true);
    check(selected().style.left === '0%', 'Escape restores the stored key edge');
    check(!root.querySelector('.preview-overlaps'), 'Gesture completion removes preview-only rendering');
    return { failures, measurements };
};
