window.runDesignAuditChecks = async function () {
    const failures = [],
        measurements = [];
    const check = (value, message) => {
        if (!value) failures.push(message);
    };
    const settle = () => new Promise((resolve) => setTimeout(resolve, 60));
    const wait = async (predicate) => {
        for (let i = 0; i < 100; i++) {
            if (predicate()) return;
            await settle();
        }
        throw Error('Fixture not ready');
    };
    const button = (root, label) =>
        [...root.querySelectorAll('button')].find(
            (n) => (n.getAttribute('aria-label') || n.textContent.trim()) === label,
        );
    const rect = (n) => {
        const r = n.getBoundingClientRect();
        return { top: r.top, bottom: r.bottom, left: r.left, right: r.right, height: r.height };
    };
    const mode = new URLSearchParams(location.search).get('component');
    if (mode === null) {
        await wait(() => document.querySelector('.sample-transport .editor-meta'));
        const lum = (color) =>
            color
                .match(/[\d.]+/g)
                .slice(0, 3)
                .map(Number)
                .map((v) => {
                    v /= 255;
                    return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
                })
                .reduce((n, v, i) => n + v * [0.2126, 0.7152, 0.0722][i], 0);
        for (const node of document.querySelectorAll(
            '.sample-transport .autoplay, .sample-transport small, .sample-transport .editor-meta, .sample-transport .transport-status',
        )) {
            const style = getComputedStyle(node);
            let bg = style.backgroundColor;
            for (let parent = node.parentElement; parent && bg === 'rgba(0, 0, 0, 0)'; parent = parent.parentElement)
                bg = getComputedStyle(parent).backgroundColor;
            const a = lum(style.color),
                b = lum(bg),
                ratio = (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
            check(ratio >= 4.5, `Transport caption contrast: ${node.textContent}`);
            measurements.push({ caption: node.textContent, foreground: style.color, background: bg, ratio });
        }
    } else if (mode === 'package') {
        await wait(() => document.querySelector('.package-tree'));
        const capacity = document.querySelector('.capacity-summary'),
            destination = document.querySelector('.import-destination');
        const tree = document.querySelector('.package-tree'),
            footer = document.querySelector('.dialog-footer');
        check(rect(capacity).bottom <= rect(destination).top, 'Capacity summary must not overlap destination');
        check(tree.clientHeight > 40 && tree.scrollHeight > tree.clientHeight, 'Review body is bounded and scrollable');
        check(rect(footer).bottom <= innerHeight + 1, 'Footer stays in viewport');
        measurements.push({
            capacity: rect(capacity),
            destination: rect(destination),
            tree: rect(tree),
            footer: rect(footer),
        });
    } else if (mode === 'workspace') {
        await wait(() => document.querySelector('.workspace-row'));
        button(document, 'Add storage location').click();
        await wait(() => document.querySelector('.host-row'));
        document.querySelector('.host-row').click();
        await wait(() => button(document, 'Select'));
        button(document, 'Select').click();
        await wait(() => document.querySelector('.workspace-confirm-dialog'));
        const form = document.querySelector('.workspace-confirm-dialog'),
            field = form.querySelector('input[type=text]');
        const s = getComputedStyle(field);
        const fieldStyle = { font: s.fontSize, height: s.height, radius: s.borderRadius };
        check(s.fontSize === '11px' && s.height === '26px' && s.borderRadius === '5px', 'Shared storage name geometry');
        button(form, 'Add storage location').click();
        await settle();
        check(
            [...form.querySelectorAll('input,button')].every((n) => n.disabled),
            'Registration locks editing and all buttons',
        );
        form.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        button(form, 'Cancel').click();
        window.dispatchEvent(new Event('focus'));
        await settle();
        check(
            document.querySelector('.workspace-confirm-dialog') === form,
            'Cancel/Escape cannot hide pending registration',
        );
        check(
            window.workspaceAudit.requests === 1 && !window.workspaceAudit.registered,
            'Only one pending registration',
        );
        check(form.querySelector('[role=status]').textContent.includes('Adding'), 'Persistent registration progress');
        const actions = [...form.querySelectorAll('.dialog-footer-actions button')].map(rect);
        check(
            actions.every((n) => n.height === actions[0].height && n.top === actions[0].top),
            'Mixed footer actions align',
        );
        if (window.captureAuditState) await window.captureAuditState('storage-pending');
        window.workspaceAudit.release();
        await wait(() => !document.querySelector('.workspace-confirm-dialog'));
        check(window.workspaceAudit.registered, 'Confirmed registration refreshes and closes');
        measurements.push({ ...fieldStyle, actions });
    } else if (mode === 'allocation') {
        await wait(() => document.querySelector('.allocation-viewport canvas'));
        const canvas = document.querySelector('.allocation-viewport canvas');
        canvas.focus();
        canvas.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true }));
        await settle();
        check(
            canvas.getAttribute('role') === 'slider' && canvas.getAttribute('aria-valuenow') === '0',
            'Focusable cluster value',
        );
        canvas.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true }));
        await settle();
        check(canvas.getAttribute('aria-valuenow') === '1', 'Cluster Arrow navigation');
        canvas.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
        await settle();
        const details = document.querySelector('.allocation-tooltip');
        check(
            details.getAttribute('role') === 'region' && document.activeElement === details,
            'Pinned keyboard detail focus',
        );
        const bounds = rect(details);
        check(
            bounds.top >= 0 && bounds.left >= 0 && bounds.bottom <= innerHeight + 1 && bounds.right <= innerWidth + 1,
            'Long cluster details stay in viewport',
        );
        check(
            getComputedStyle(details).overflowY === 'auto' && details.scrollHeight > details.clientHeight,
            'Full owners are scrollable',
        );
        details.scrollTop = details.scrollHeight;
        await settle();
        check(
            details.scrollTop > 0 && details.querySelectorAll('.owner').length === 15,
            'Last owner is retained and reachable',
        );
        check(
            rect(details.querySelector('.owner:last-of-type')).bottom <= rect(details).bottom + 1,
            'Last owner scrolls into view',
        );
        details.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
        await settle();
        check(
            !document.querySelector('.allocation-tooltip') && document.activeElement === canvas,
            'Escape restores map focus',
        );
        const selected = document.querySelector('[aria-label="Cluster cell size"] [aria-pressed=true]');
        check(!!selected, 'Cell size exposes pressed state');
        measurements.push({
            bounds,
            selectedForeground: getComputedStyle(selected).color,
            selectedBackground: getComputedStyle(selected).backgroundColor,
        });
    } else if (mode === 'wave') {
        await wait(() => document.querySelector('.waveform-seek-input'));
        const range = document.querySelector('.waveform-seek-input');
        check(range.type === 'range' && range.min === '0' && range.max === '1', 'Seek uses native range semantics');
        range.value = '0.5';
        range.dispatchEvent(new Event('input', { bubbles: true }));
        await settle();
        check(
            window.waveAudit.seeks.at(-1) === 0.5 && Number(range.value) === 0.5,
            'Active seek position survives unrelated updates',
        );
        check(range.getAttribute('aria-valuetext') === '0.500 s', 'Seek exposes current time');
        check(window.waveAudit.plays === 0, 'Seeking never starts playback');
    } else if (mode === 'completion') {
        await wait(() => typeof window.runPackageCompletionAudit === 'function');
        const result = await window.runPackageCompletionAudit();
        check(
            result.first.accepted && result.first.phase === 'completed' && result.first.canDismiss,
            'Canonical package result completes',
        );
        check(
            result.refreshCalls === 1 && result.starts === 1 && result.waits === 1,
            'Completion refreshes exactly once without resubmission',
        );
        measurements.push(result);
    }
    return { failures, measurements };
};
