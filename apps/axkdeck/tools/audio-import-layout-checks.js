window.runAudioImportLayoutChecks = async function () {
    const failures = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    for (let attempt = 0; attempt < 60 && !document.querySelector('.audio-import-card.editable'); attempt++) {
        if (['empty', 'inspecting', 'invalid'].includes(new URLSearchParams(location.search).get('scenario'))) break;
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    const shell = document.querySelector('.audio-import-dialog');
    if (!shell) return { failures: ['Audio import dialog unavailable'] };
    const bounds = node => node.getBoundingClientRect();
    const near = (a, b) => Math.abs(a - b) <= 1.1;
    const scale = bounds(shell).width / shell.offsetWidth;
    const box = bounds(shell);
    check(box.left >= -1 && box.right <= innerWidth + 1, 'Dialog escapes viewport horizontally');
    check(box.top >= -1 && box.bottom <= innerHeight + 1, 'Dialog escapes viewport vertically');
    const scroller = shell.querySelector('.audio-import-rows');
    const rowWidth = scroller ? scroller.clientWidth - parseFloat(getComputedStyle(scroller).paddingRight) : 0;
    const wide = rowWidth >= 1230;
    const fourColumns = rowWidth >= 814 && !wide;
    const rows = [...shell.querySelectorAll('.audio-import-card')];
    const params = new URLSearchParams(location.search);
    if (params.has('native-cases')) check(!!shell.querySelector('.wave-right') === params.has('short-stereo'), 'Native mono/stereo dataset mismatch');
    const geometry = [];
    for (const row of rows) {
        check(row.scrollWidth <= row.clientWidth + 1, `${row.getAttribute('aria-label')}: horizontal overflow`);
        if (row.querySelector('[role=alert]')) check(getComputedStyle(row).borderTopColor === getComputedStyle(row.querySelector('[role=alert]')).color, 'Invalid row outline must use the error color');
        for (const input of row.querySelectorAll('input[type=number]')) {
            const r = bounds(input);
            check(r.width >= 62 * scale - 1 && r.height >= 26 * scale - 1, `${input.getAttribute('aria-label')}: numeric minimum lost`);
        }
        const fields = ['.sample-name', '.wave-left', '.wave-right', '.target-rate'].map(selector => row.querySelector(selector));
        const groups = ['.sampler-pitch', '.key-setting', '.velocity-setting', '.playback-setting'].map(selector => row.querySelector(selector));
        if (row.classList.contains('editable') && (wide || fourColumns)) {
            for (let index = 0; index < fields.length; index++) {
                if (!fields[index]) continue;
                check(near(bounds(fields[index]).left, bounds(groups[index]).left), `Column ${index}: left alignment`);
                if (index < 3) check(near(bounds(fields[index]).right, bounds(groups[index]).right), `Column ${index}: right alignment`);
            }
        }
        if (wide && !row.querySelector('.loop-settings, .row-feedback, .row-details') && row.classList.contains('editable')) {
            check(near(bounds(row).height / scale, 72), `Normal row height ${bounds(row).height / scale}, expected72`);
        }
        for (const group of row.querySelectorAll('.sampler-pitch, .key-setting, .velocity-setting, .playback-setting')) {
            check(group.scrollWidth <= group.clientWidth + 1, `${group.className}: parameter group overflow`);
        }
        geometry.push({ height: bounds(row).height / scale, width: bounds(row).width / scale });
    }
    const compact = [...shell.querySelectorAll('.dialog-segmented-control--compact')];
    const top = shell.querySelector('.audio-import-controls');
    check(top.scrollWidth <= top.clientWidth + 1, 'Top control band overflows');
    const destination = [...top.querySelectorAll('.destination-label, .destination-mode, .destination-partition, .volume-combobox, .new-volume-name')].map(bounds);
    for (let i = 0; i < destination.length; i++) for (let j = i + 1; j < destination.length; j++) {
        const a = destination[i], b = destination[j];
        check(Math.min(a.right, b.right) - Math.max(a.left, b.left) <= 1 || Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top) <= 1, 'Destination controls overlap');
    }
    check(compact.length === (new URLSearchParams(location.search).get('scenario') === 'empty' ? 2 : 3), 'Unexpected compact control count');
    for (const group of compact) {
        const r = bounds(group);
        check(near(r.width / scale, 112) && near(r.height / scale, 28), `${group.getAttribute('aria-label')}: compact dimensions`);
        const css = getComputedStyle(group);
        check(parseFloat(css.borderTopWidth) > 0 && parseFloat(css.borderBottomWidth) > 0, 'Compact horizontal borders missing');
        const buttons = [...group.querySelectorAll('button')].map(bounds);
        check(near(buttons[0].top, buttons[1].top) && near(buttons[0].height, buttons[1].height), 'Unequal toggle segments');
    }
    if (shell.querySelector('.audio-import-body').clientWidth - 24 >= 1252) {
        check(compact.every(group => near(bounds(group).top, bounds(compact[0]).top)), 'Wide target controls must share one row');
    }
    const actions = [...shell.querySelectorAll('.dialog-footer button')].map(bounds);
    check(actions.every(action => near(action.height, actions[0].height)), 'Footer actions have different heights');
    const heading = shell.querySelector('.audio-import-column-headings');
    if (wide && rows[0]?.classList.contains('editable')) {
        const labels = [...heading.children];
        const fields = [rows[0].querySelector('.audio-import-file'), ...['.sample-name', '.wave-left', '.wave-right', '.target-rate'].map(selector => rows[0].querySelector(selector))];
        fields.forEach((field, index) => { if (field) check(near(bounds(field).left, bounds(labels[index]).left), `Heading ${index} misaligned`); });
    }
    return { failures, wide, fourColumns, rowWidth, scale, geometry, compactCount: compact.length };
};
