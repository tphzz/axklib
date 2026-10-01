// Shared constructed-state checks for Chromium and native WebKitGTK.
window.runInspectorPanelsRegression = async function () {
    const failures = [], measurements = [];
    const assert = (condition, message) => { if (!condition) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 100));
    for (let attempt = 0; attempt < 30 && !window.inspectorPanelsFixture; attempt++) await settle();
    const api = window.inspectorPanelsFixture;
    if (!api) throw new Error('Inspector panel fixture did not initialize');
    const configure = async values => { api.configure(values); await settle(); };
    const heading = title => [...document.querySelectorAll('.inspector-section-heading button')].find(node => node.getAttribute('aria-label') === title);
    const body = title => document.getElementById(heading(title).getAttribute('aria-controls'));
    const expanded = title => heading(title).getAttribute('aria-expanded') === 'true';
    const toggle = async title => { heading(title).click(); await settle(); };
    const titles = () => [...document.querySelectorAll('.inspector-section-heading button')].map(node => node.getAttribute('aria-label'));
    const expected = {
        program: ['Properties', 'Relationships'],
        sequence: ['Properties', 'Relationships'],
        'sample-bank': ['Preview', 'Properties', 'Relationships'],
        sample: ['Preview', 'Properties', 'Relationships', 'Stored format'],
        'wave-data': ['Preview', 'Properties', 'Relationships'],
        files: ['Properties', 'Storage details'],
    };
    for (const width of [260, 320]) {
        for (const scale of [1, 1.5, 2]) {
            for (const kind of Object.keys(expected)) {
                api.reset();
                await configure({ kind, width, scale, shown: true, version: 0 });
                const prefix = `${kind}/${width}/${scale}`;
                assert(JSON.stringify(titles()) === JSON.stringify(expected[kind]), `${prefix}: section order`);
                const content = document.querySelector('.inspector-content');
                const contentBox = content.getBoundingClientRect();
                const buttons = [...document.querySelectorAll('.inspector-section-heading button')];
                const lefts = [], rights = [], fonts = [];
                for (const button of buttons) {
                    const title = button.getAttribute('aria-label');
                    const initiallyCollapsed = title === 'Relationships' || title === 'Stored format';
                    assert(expanded(title) === !initiallyCollapsed, `${prefix}/${title}: initial disclosure state`);
                    assert(body(title).hidden === initiallyCollapsed, `${prefix}/${title}: initial body visibility`);
                    if (initiallyCollapsed) await toggle(title);
                    const rotation = new DOMMatrix(getComputedStyle(button.querySelector('.inspector-section-chevron')).transform);
                    assert(Math.abs(rotation.a) < 0.01 && Math.abs(rotation.b - 1) < 0.01, `${prefix}/${title}: expanded chevron points down`);
                    const bounds = button.getBoundingClientRect();
                    const region = body(title), regionBox = region.getBoundingClientRect();
                    assert(button.closest('.inspector-content') === content, `${prefix}/${title}: common padded content`);
                    assert(Math.abs(bounds.left - regionBox.left) < 1, `${prefix}/${title}: heading/body left alignment`);
                    assert(Math.abs(bounds.right - regionBox.right) < 1, `${prefix}/${title}: heading/body right alignment`);
                    assert(bounds.width > 0 && bounds.height >= 20 * scale - 1, `${prefix}/${title}: usable full-width heading`);
                    assert(button.scrollWidth <= button.clientWidth + 1, `${prefix}/${title}: no heading overflow`);
                    assert(regionBox.left >= contentBox.left && regionBox.right <= contentBox.right + 1, `${prefix}/${title}: body fits content`);
                    lefts.push(bounds.left); rights.push(bounds.right); fonts.push(getComputedStyle(button).fontSize);
                }
                assert(Math.max(...lefts) - Math.min(...lefts) < 1, `${prefix}: matching section left gutters`);
                assert(Math.max(...rights) - Math.min(...rights) < 1, `${prefix}: matching section right gutters`);
                assert(new Set(fonts).size === 1, `${prefix}: matching heading typography`);
                const scroller = document.querySelector('.inspector-body');
                assert(scroller.scrollWidth <= scroller.clientWidth + 1, `${prefix}: no horizontal inspector overflow`);
                const footer = document.querySelector('.inspector-mode-footer');
                const before = footer.getBoundingClientRect();
                scroller.scrollTop = scroller.scrollHeight;
                await settle();
                assert(Math.abs(footer.getBoundingClientRect().top - before.top) < 1, `${prefix}: fixed footer while scrolling`);
                scroller.scrollTop = 0;
                measurements.push({ kind, width, scale, lefts, rights, fonts, footerTop: before.top });
            }
        }
    }
    api.reset();
    await configure({ kind: 'sample', width: 260, scale: 1, playhead: 0 });
    await toggle('Relationships');
    const relationBody = body('Relationships');
    const relation = relationBody.querySelector('button');
    relation.focus();
    await toggle('Relationships');
    const collapsedRotation = new DOMMatrix(getComputedStyle(heading('Relationships').querySelector('.inspector-section-chevron')).transform);
    assert(Math.abs(collapsedRotation.a - 1) < 0.01 && Math.abs(collapsedRotation.b) < 0.01, 'collapsed chevron points right');
    assert(relationBody.contains(relation), 'collapse retains relationship elements');
    assert(document.activeElement === heading('Relationships'), 'collapse returns descendant focus to heading');
    relation.focus();
    assert(document.activeElement !== relation, 'hidden relationship rejects programmatic focus');
    await toggle('Relationships');
    relation.click();
    await settle();
    assert(document.querySelector('.inspector-panels-fixture').dataset.navigation.length > 0, 'reopened relationship navigates');
    assert(!expanded('Stored format'), 'Stored format starts independently collapsed');
    assert(heading('Stored format').textContent.includes('a3k'), 'collapsed Stored format retains badge');
    const formatDescription = document.getElementById(heading('Stored format').getAttribute('aria-describedby'));
    assert(formatDescription?.textContent.includes('a3k'), 'collapsed Stored format announces its badge');
    assert(body('Stored format').textContent.includes('188 bytes'), 'collapsed Stored format retains details');
    await toggle('Stored format');
    assert(expanded('Stored format'), 'Stored format opens on request');
    await toggle('Properties');

    const preview = body('Preview');
    const canvas = preview.querySelector('canvas');
    const pixelCount = () => {
        const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
        let count = 0;
        for (let index = 3; index < pixels.length; index += 4) if (pixels[index] > 0) count++;
        return count;
    };
    assert(pixelCount() > canvas.width * 2, 'initial preview contains waveform pixels, not only baseline');
    await toggle('Preview');
    await configure({ playhead: 33075, width: 320 });
    await toggle('Preview');
    assert(body('Preview').querySelector('canvas') === canvas, 'Preview collapse retains canvas');
    assert(pixelCount() > canvas.width * 2, 'reopened preview redraws nonblank waveform');
    assert(Math.abs(canvas.width - canvas.getBoundingClientRect().width * devicePixelRatio) <= 1, 'reopened canvas matches resized pane');
    assert(Math.abs(Number(preview.querySelector('.waveform-frame').dataset.playheadRatio) - 0.75) < 0.001, 'reopened preview uses current playback position');

    await configure({ version: 1 });
    assert(!expanded('Properties') && expanded('Stored format') && expanded('Relationships'), 'selection change retains Sample choices');
    await configure({ kind: 'program' });
    assert(expanded('Properties'), 'Sample collapse does not affect Program profile');
    assert(!expanded('Relationships'), 'Sample expansion does not affect Program relationships');
    await configure({ kind: 'files' });
    assert(expanded('Properties') && expanded('Storage details'), 'Files starts independently expanded');
    const help = body('Storage details').querySelector('button');
    help.focus();
    await settle();
    assert(document.querySelector('[role="tooltip"]'), 'Storage attribute help opens on focus');
    await toggle('Storage details');
    assert(!document.querySelector('[role="tooltip"]'), 'collapsing panel closes portalled help');
    assert(document.activeElement === heading('Storage details'), 'collapsing help moves focus to heading');
    await toggle('Properties');
    assert(document.querySelector('[role="status"]').getBoundingClientRect().height > 0, 'Files issue remains visible when panels collapse');
    const footer = document.querySelector('.inspector-mode-footer button');
    footer.click();
    await settle();
    assert(document.querySelector('.inspector-panels-fixture').dataset.navigation === 'device:sample', 'Files footer stays actionable');
    await configure({ shown: false });
    assert(!document.querySelector('aside'), 'inspector can be hidden');
    await configure({ shown: true, kind: 'sample' });
    assert(!expanded('Properties') && expanded('Stored format') && expanded('Relationships'), 'hide/show retains Sample choices');
    await configure({ kind: 'files' });
    assert(!expanded('Properties') && !expanded('Storage details'), 'mode change retains Files choices');
    api.reset();
    await configure({ kind: 'sample', width: 320, scale: 1, version: 0 });
    return { failures, measurements, trustedKeyboard: 'Checked separately by the Chromium runner' };
};
