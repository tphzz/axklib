window.runCompactMappingChecks = async function () {
    const failures = [], measurements = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 100));
    for (const [selector, name] of [['[role=tab]', 'Map/Out'], ['[aria-label="Sample subpages"] button', 'Mix & Key']]) {
        let button;
        for (let attempt = 0; attempt < 40 && !button; attempt++) {
            button = [...document.querySelectorAll(selector)].find(node => node.textContent.trim() === name);
            if (!button) await settle();
        }
        if (!button) throw new Error(`Missing ${name}`);
        button.click(); await settle();
    }
    for (let attempt = 0; attempt < 40 && !document.querySelector('.keyboard-mapping:not(.full)'); attempt++) await settle();
    const mapping = document.querySelector('.keyboard-mapping:not(.full)');
    if (!mapping) throw new Error('Missing compact mapping');
    const groups = document.querySelector('.parameter-groups[data-page="mix-key"]');
    for (const gap of [12, 8]) {
        document.documentElement.style.setProperty('--density-section-gap', `${gap}px`);
        await settle(); await settle();
        const sections = [...groups.children].filter(node => node.tagName === 'SECTION');
        const boxes = sections.map(node => node.getBoundingClientRect());
        const box = mapping.getBoundingClientRect();
        const scale = box.width / mapping.offsetWidth;
        const left = Math.min(...boxes.map(box => box.left)), right = Math.max(...boxes.map(box => box.right));
        const bottom = Math.max(...boxes.map(box => box.bottom));
        const panel = document.querySelector('.sample-panel');
        measurements.push({ gap, columns: groups.dataset.columns, left, right, bottom, mapping: box.toJSON(), scale,
            panelWidth: panel.clientWidth, panelScrollWidth: panel.scrollWidth, mappingScrollWidth: mapping.scrollWidth });
        check(Math.abs(box.left - left) <= 1, 'Compact mapping shares the parameter left edge');
        check(Math.abs(box.right - right) <= 1, 'Compact mapping shares the parameter right edge');
        check(Math.abs(box.top - bottom - 8 * scale) <= 1, 'Compact mapping retains its 8px vertical gap');
        check(box.width / scale <= 1281, 'Compact mapping keeps the existing width cap');
        check(box.right <= innerWidth + 1, 'Compact mapping fits the viewport');
    }
    check(document.querySelector('[aria-label="Write count"]').textContent === '0', 'Layout changes do not write');
    check([...document.querySelectorAll('button')].find(node => node.textContent.trim() === 'Save')?.disabled, 'Layout changes do not dirty the draft');
    return { failures, measurements };
};
