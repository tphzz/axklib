window.runDesignSystemChecks = async function () {
    const failures = [], measurements = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 80));
    const style = (node, pseudo) => getComputedStyle(node, pseudo);
    const button = name => [...document.querySelectorAll('button')].find(node => node.textContent.trim() === name);
    const click = async node => { if (!node) throw Error('Missing control'); node.scrollIntoView({ block: 'nearest' }); node.click(); await settle(); };
    const colorCanvas = document.createElement('canvas').getContext('2d');
    const rgb = color => { colorCanvas.clearRect(0, 0, 1, 1); colorCanvas.fillStyle = color; colorCanvas.fillRect(0, 0, 1, 1); return [...colorCanvas.getImageData(0, 0, 1, 1).data].slice(0, 3); };
    const luminance = color => rgb(color).map(v => v / 255).map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4).reduce((v, n, i) => v + n * [.2126, .7152, .0722][i], 0);
    const ratio = (a, b) => { const x = luminance(a), y = luminance(b); return (Math.max(x, y) + .05) / (Math.min(x, y) + .05); };
    const background = node => { for (let p = node; p; p = p.parentElement) { const value = style(p).backgroundColor; if (value !== 'rgba(0, 0, 0, 0)' && value !== 'transparent') return value; } return style(document.documentElement).backgroundColor; };
    const readable = (node, name) => { const contrast = ratio(style(node).color, background(node)); measurements.push({ name, contrast }); check(contrast >= 4.5, `${name}: contrast ${contrast.toFixed(3)}`); };
    for (let i = 0; i < 50 && !document.querySelector('[data-fixture=selected]'); i++) await settle();
    for (const node of document.querySelectorAll('.dialog-footer-actions button, .warning, .dialog-segmented-control button, .collection-identity-secondary, .storage-picker-row-active small, .sequence-row span')) readable(node, node.textContent.trim());
    const actions = [...document.querySelectorAll('.dialog-footer-actions button')];
    const heights = actions.map(node => style(node).height);
    check(heights.every(height => height === '30px'), 'Mixed footer actions: 30px height');
    check(actions.every(node => style(node).marginTop === '0px' && style(node).marginBottom === '0px'), 'Mixed footer actions: zero vertical margins');
    check(style(document.querySelector('.brand strong')).letterSpacing === 'normal' || style(document.querySelector('.brand strong')).letterSpacing === '0px', 'Brand tracking is zero');
    const field = document.querySelector('.dialog-field-control');
    check(ratio(style(field).borderColor, style(field).backgroundColor) >= 3, 'Field boundary contrast');
    check(ratio(style(field, '::placeholder').color, style(field).backgroundColor) >= 4.5, 'Placeholder contrast');
    const selected = document.querySelector('[data-fixture=selected]');
    check(style(selected).backgroundColor === 'rgb(41, 73, 88)' && style(selected).backgroundImage === 'none', 'Selected row uses the shared opaque surface');
    check(style(document.querySelector('[data-fixture=active]')).backgroundColor === 'rgba(0, 0, 0, 0)', 'Active-only row is not selected');
    const editor = document.querySelector('[aria-label="Editor switch"]'), diagnostic = document.querySelector('[aria-label="Diagnostic switch"]');
    check(editor.getBoundingClientRect().right <= diagnostic.getBoundingClientRect().left, 'Switch fixture hosts must not overlap');
    const root = document.documentElement;
    root.style.setProperty('--state-focus', '#f1a4a4');
    root.style.setProperty('--state-active-border', '#34d399');
    field.focus(); await settle();
    check(document.activeElement === field, 'Field receives focus before checking its state role');
    measurements.push({ name: 'State role propagation', pageFocused: document.hasFocus(), fieldBorder: style(field).borderColor });
    check(style(field).borderColor === 'rgb(241, 164, 164)', 'Focus token overrides propagate to field boundaries');
    for (let i = 0; i < 10 && style(selected).borderColor !== 'rgb(52, 211, 153)'; i++) await settle();
    check(style(selected).borderColor === 'rgb(52, 211, 153)', 'Active-border token overrides propagate to selected rows');
    root.style.removeProperty('--state-focus'); root.style.removeProperty('--state-active-border');
    field.blur(); await settle();
    for (const [node, width, height] of [[editor, 30, 17], [diagnostic, 26, 14]]) {
        const track = node.querySelector('.switch-track');
        check(style(track).width === `${width}px` && style(track).height === `${height}px`, 'Named switch track geometry');
        check(ratio(style(track).borderColor, style(track).backgroundColor) >= 3, 'Switch boundary contrast');
    }
    await click(editor);
    check(editor.getAttribute('aria-checked') === 'true' && diagnostic.getAttribute('aria-checked') === 'true', 'Shared switch state propagates');
    await click(diagnostic);
    check(editor.getAttribute('aria-checked') === 'false', 'Diagnostic label toggles shared state');
    await click(document.querySelector('[aria-label="Inherited disabled switch"]'));
    await click(document.querySelector('[aria-label="Explicit disabled switch"]'));
    check(document.querySelector('[aria-label="Switch changes"]').textContent === '2', 'Disabled switches do not invoke callbacks');
    const compact = document.querySelector('.compact-checkbox'), dialogMark = document.querySelector('.dialog-checkbox');
    await click(compact);
    check(style(compact).width === '12px' && style(dialogMark).width === '14px', 'Named checkbox sizes');
    check(style(compact).backgroundColor === style(dialogMark).backgroundColor, 'Compact/dialog checked palette matches');
    check(style(compact, '::before').color === style(dialogMark, '::before').color, 'Compact/dialog marks share foreground');
    check(ratio(style(compact, '::before').color, style(compact).backgroundColor) >= 3, 'Checkbox mark contrast');
    for (const size of [4, 7, 11]) {
        await click(button(`${size} pixel cells`));
        const viewport = document.querySelector('.allocation-viewport'); viewport.scrollTop = 0; await settle();
        const canvas = viewport.querySelector('canvas'), context = canvas.getContext('2d'), scale = canvas.width / parseFloat(canvas.style.width);
        const columns = Math.max(1, Math.floor((parseFloat(canvas.style.width) + 1) / (size + 1)));
        [...document.querySelectorAll('.legend i')].forEach((swatch, index) => {
            const x = (index % columns) * (size + 1) + size / 2, y = Math.floor(index / columns) * (size + 1) + size / 2;
            const pixel = [...context.getImageData(Math.floor(x * scale), Math.floor(y * scale), 1, 1).data].slice(0, 3);
            check(pixel.every((value, i) => value === rgb(style(swatch).backgroundColor)[i]), `${swatch.dataset.kind}: canvas equals legend at ${size}px`);
        });
        viewport.scrollTop = 800; await settle();
        check(parseFloat(canvas.style.top) > 0, 'Allocation scrolling remains virtualized');
    }
    for (const scenario of ['idle', 'inspecting', 'repairing', 'error', 'success', 'blocked']) {
        await click(button(`Repair ${scenario}`));
        const dialog = document.querySelector('[role=dialog]'), footer = dialog.querySelector('.dialog-footer'), input = dialog.querySelector('input');
        if (input) check(style(input).fontSize === '11px', 'Repair value uses shared 11px typography');
        for (const node of footer.querySelectorAll('button')) check(style(node).height === '30px', `Repair ${scenario}: stable action height`);
        const busy = ['inspecting', 'repairing'].includes(scenario);
        check([...dialog.querySelectorAll('button')].every(node => !busy || node.disabled), `Repair ${scenario}: busy locks`);
        const details = dialog.querySelector('.placement-results');
        if (details) check(details.scrollHeight > details.clientHeight && style(details).overflowY === 'auto', 'Long repair results have an accessible bounded scroller');
        if (scenario === 'success') check(!!button('Done'), 'Repair results retain Done');
        dialog.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); await settle();
        check(!!document.querySelector('[role=dialog]') === busy, `Repair ${scenario}: Escape contract`);
        if (busy) { window.designFixture.finish(); await settle(); await click(button('Cancel')); }
    }
    return { failures, measurements };
};
