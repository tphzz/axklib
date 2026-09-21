window.runASeriesPreferencesChecks = async () => {
    const failures = [];
    const measurements = [];
    const check = (condition, message) => {
        if (!condition) failures.push(message);
    };
    const wait = async (predicate, message) => {
        for (let index = 0; index < 160; index++) {
            if (predicate()) return;
            await new Promise((resolve) => setTimeout(resolve, 25));
        }
        throw new Error(`Timed out: ${message}`);
    };
    const button = (name, parent = document) =>
        [...parent.querySelectorAll('button')].find(
            (node) => (node.getAttribute('aria-label') || node.textContent).trim() === name,
        );
    const output = (name) => document.querySelector(`output[aria-label="${name}"]`).textContent;
    const dialog = () => document.querySelector('[role="dialog"]');
    const input = (node, value) => {
        node.value = value;
        node.dispatchEvent(new Event('input', { bubbles: true }));
    };
    const change = (node, value) => {
        node.value = value;
        node.dispatchEvent(new Event('change', { bubbles: true }));
    };
    const key = (node, value, options = {}) =>
        node.dispatchEvent(
            new KeyboardEvent('keydown', {
                key: value,
                bubbles: true,
                cancelable: true,
                ...options,
            }),
        );
    const open = async (name) => {
        const trigger = button(name);
        trigger.focus();
        trigger.click();
        await wait(dialog, `${name} opens`);
        await wait(() => dialog().contains(document.activeElement), 'initial modal focus');
        return dialog();
    };
    const dismissed = () => wait(() => !dialog(), 'dialog dismissal');
    const capture = async (name) => {
        if (window.captureASeriesPreferences) await window.captureASeriesPreferences(name);
    };
    const formatSelected = (label) => button(label, dialog())?.getAttribute('aria-pressed') === 'true';
    const geometry = (name, primaryName) => {
        const shell = dialog();
        const bounds = shell.getBoundingClientRect();
        const cancel = button('Cancel', shell);
        const primary = button(primaryName, shell);
        const left = cancel.getBoundingClientRect(),
            right = primary.getBoundingClientRect();
        const cancelStyle = getComputedStyle(cancel),
            primaryStyle = getComputedStyle(primary);
        check(
            bounds.left >= 0 && bounds.right <= innerWidth && bounds.top >= 0 && bounds.bottom <= innerHeight,
            `${name}: dialog fits viewport`,
        );
        check(shell.scrollHeight <= shell.clientHeight + 1, `${name}: shell has no vertical overflow`);
        check(
            Math.abs(left.height - right.height) < 1 && Math.abs(left.top - right.top) < 1,
            `${name}: mixed footer actions have equal heights and alignment`,
        );
        check(
            [cancelStyle, primaryStyle].every((style) => style.marginTop === '0px' && style.marginBottom === '0px'),
            `${name}: footer actions have zero vertical margins`,
        );
        check(left.right <= right.left, `${name}: dismissal precedes primary action without overlap`);
        const controls = [...shell.querySelectorAll('.dialog-field-control, .dialog-segmented-control button')];
        const controlFonts = controls.map((control) => {
            const style = getComputedStyle(control);
            const token = control.classList.contains('dialog-field-control')
                ? '--dialog-control-font-size'
                : '--dialog-table-header-font-size';
            return { actual: style.fontSize, expected: style.getPropertyValue(token).trim() };
        });
        check(
            controlFonts.every(({ actual, expected }) => actual === expected),
            `${name}: controls use their shared typography tokens`,
        );
        check(
            controls.every((control) => {
                const rect = control.getBoundingClientRect();
                return (
                    rect.left >= bounds.left &&
                    rect.right <= bounds.right &&
                    rect.top >= bounds.top &&
                    rect.bottom <= bounds.bottom
                );
            }),
            `${name}: controls stay inside dialog`,
        );
        check(getComputedStyle(shell.querySelector('h2')).fontSize === '13px', `${name}: compact dialog title`);
        const lastAction = primary.disabled ? cancel : primary;
        lastAction.focus();
        key(lastAction, 'Tab');
        check(document.activeElement === button('Close', shell), `${name}: Tab wraps to modal start`);
        key(document.activeElement, 'Tab', { shiftKey: true });
        check(document.activeElement === lastAction, `${name}: Shift+Tab wraps to modal end`);
        measurements.push({
            name,
            width: bounds.width,
            height: bounds.height,
            footerHeight: left.height,
            controlFonts,
        });
    };

    await wait(() => button('Open preferences') && !button('Open preferences').disabled, 'fixture initialization');
    await open('Open preferences');
    check(formatSelected('a3k'), 'Preferences: initial native preference');
    const formatChoices = dialog().querySelector('[role="group"][aria-label="Preferred A-Series generation"]');
    check(
        [...formatChoices.querySelectorAll('button')].map((node) => node.textContent.trim()).join('|') ===
            'a3k|a4k/a5k',
        'Preferences: native choice is first',
    );
    geometry('preferences', 'Save');
    await capture('preferences');
    button('a4k/a5k', dialog()).click();
    await wait(() => formatSelected('a4k/a5k'), 'preference draft');
    button('Cancel', dialog()).click();
    await dismissed();
    check(
        output('Saved generation') === 'A3000' && output('Preference writes') === '0',
        'Cancel does not publish preference',
    );
    await wait(() => document.activeElement === button('Open preferences'), 'preference focus restoration');

    document.querySelector('input[type="checkbox"]').click();
    await open('Open preferences');
    button('a4k/a5k', dialog()).click();
    button('Save', dialog()).click();
    await wait(() => dialog()?.textContent.includes('Test preference save failure'), 'failed save');
    check(output('Saved generation') === 'A3000', 'Failed save does not publish draft');
    geometry('preferences-save-failure', 'Save');
    await capture('preferences-save-failure');
    button('Save', dialog()).click();
    await dismissed();
    check(output('Saved generation') === 'A4000_A5000' && output('Preference writes') === '1', 'Save publishes once');
    await open('Open preferences');
    check(formatSelected('a4k/a5k'), 'Reopened preferences use saved generation');
    key(dialog(), 'Escape');
    await dismissed();

    await open('Assign native Samples');
    check(formatSelected('a3k'), 'Known native source overrides later preference');
    geometry('new-native-bank', 'Assign to Sample Bank');
    input(dialog().querySelector('#sample-bank-name'), 'Native Sources');
    button('a4k/a5k', dialog()).click();
    await wait(() => formatSelected('a4k/a5k'), 'bank format override');
    await capture('bank-format-override');
    button('Existing', dialog()).click();
    await wait(() => dialog().querySelector('[role="combobox"]'), 'existing bank mode');
    check(!button('a3k', dialog()) && !button('a4k/a5k', dialog()), 'Existing bank has no conversion selector');
    const picker = dialog().querySelector('[role="combobox"]');
    await wait(() => document.activeElement === picker, 'existing bank focus');
    check(picker.getAttribute('aria-expanded') === 'false', 'Existing bank popup starts closed');
    key(picker, 'ArrowDown');
    await wait(() => picker.getAttribute('aria-expanded') === 'true', 'keyboard bank popup opening');
    key(picker, 'Escape');
    await wait(() => picker.getAttribute('aria-expanded') === 'false', 'keyboard bank popup dismissal');
    check(Boolean(dialog()), 'First Escape closes only the bank popup');
    key(picker, 'ArrowDown');
    key(picker, 'Enter');
    await wait(() => picker.value === 'Native Bank', 'keyboard bank selection');
    check(
        dialog().querySelector('.format-badge')?.textContent.trim() === 'a3k',
        'Existing target shows stored native badge',
    );
    check(
        ![...dialog().querySelectorAll('button')].some((node) => /convert/i.test(node.textContent)),
        'Existing target never offers conversion',
    );
    geometry('existing-bank', 'Assign to Sample Bank');
    await capture('existing-bank');
    button('New', dialog()).click();
    await wait(() => formatSelected('a4k/a5k'), 'override retained across mode switch');
    check(
        dialog().querySelector('#sample-bank-name').value === 'Native Sources',
        'New bank name retained across mode switch',
    );
    button('Assign to Sample Bank', dialog()).click();
    await dismissed();
    check(
        JSON.parse(output('Bank submission')).sampleFormat === 'A4000_A5000_224',
        'New target submits explicit override',
    );

    await open('Assign mixed Samples');
    check(formatSelected('a4k/a5k'), 'Mixed source initializes later bank format');
    button('a3k', dialog()).click();
    input(dialog().querySelector('#sample-bank-name'), 'Mixed Sources');
    await wait(() => !button('Assign to Sample Bank', dialog()).disabled, 'new bank submission ready');
    button('Assign to Sample Bank', dialog()).click();
    await dismissed();
    check(
        JSON.parse(output('Bank submission')).sampleFormat === 'A3000_188',
        'Mixed sources allow explicit native bank override',
    );
    await open('Assign unknown Samples');
    check(formatSelected('a4k/a5k'), 'Unknown source uses saved preference');
    button('Existing', dialog()).click();
    await wait(() => dialog().querySelector('[role="combobox"]'), 'existing target choice');
    const existing = dialog().querySelector('[role="combobox"]');
    input(existing, 'Later Bank');
    await wait(() => dialog().querySelector('[role="option"]'), 'filtered bank option');
    dialog().querySelector('[role="option"]').click();
    await wait(() => !button('Assign to Sample Bank', dialog()).disabled, 'existing assignment ready');
    check(
        dialog().querySelector('.format-badge')?.textContent.trim() === 'a4k/a5k',
        'Later target shows stored later badge',
    );
    button('Assign to Sample Bank', dialog()).click();
    await dismissed();
    const assignment = JSON.parse(output('Bank submission'));
    check(
        assignment.mode === 'existing' && assignment.bankObjectId === 'later' && !('sampleFormat' in assignment),
        'Existing assignment submits identity without format conversion',
    );
    await open('Assign native Samples');
    key(dialog(), 'Escape');
    await dismissed();

    await open('Create image');
    const capacity = dialog().querySelector('select[aria-label="Capacity"]');
    await wait(() => capacity.options.length === 9, 'capacity profiles');
    check(
        [...capacity.options].map((option) => option.textContent).join('|') ===
            '1.44 MB|128 MiB|256 MiB|CD-R 650|CD-R 700|1 GiB|2 GiB|4 GiB|8 GiB',
        'Capacity profile order',
    );
    change(capacity, 'HDS_128_MIB');
    await wait(() => !button('6 partitions', dialog()).disabled, '128 MiB counts');
    button('6 partitions', dialog()).click();
    change(capacity, 'HDS_4_GIB');
    await wait(
        () => button('6 partitions', dialog()).getAttribute('aria-pressed') === 'true',
        'admitted count retained',
    );
    change(capacity, 'HDS_8_GIB');
    await wait(() => button('8 partitions', dialog()).getAttribute('aria-pressed') === 'true', '8 GiB default count');
    for (let count = 1; count <= 8; count++) {
        const option = button(`${count} ${count === 1 ? 'partition' : 'partitions'}`, dialog());
        check(option.disabled === count < 8, `8 GiB: ${count} partition availability`);
        if (count < 8) check(Boolean(option.title), `8 GiB: ${count} partition has disabled reason`);
    }
    button('1 partition', dialog()).click();
    check(
        button('8 partitions', dialog()).getAttribute('aria-pressed') === 'true',
        'Disabled count cannot change selection',
    );
    geometry('create-8-gib', 'Create');
    await capture('create-8-gib');
    button('Create', dialog()).click();
    await dismissed();
    const plan = JSON.parse(output('Image creation'));
    check(
        plan.profileId === 'HDS_8_GIB' && plan.partitionCount === 8,
        'Creation submits selected server-admitted profile/count',
    );
    await open('Create image');
    await wait(() => !button('Create', dialog()).disabled, 'image dialog ready');
    key(dialog(), 'Escape');
    await dismissed();

    if (new URLSearchParams(location.search).has('short-stereo')) {
        await open('Open preferences');
    } else {
        await open('Create image');
        const finalCapacity = dialog().querySelector('select[aria-label="Capacity"]');
        await wait(() => finalCapacity.options.length === 9, 'final capacity profiles');
        change(finalCapacity, 'HDS_8_GIB');
        await wait(
            () => button('8 partitions', dialog()).getAttribute('aria-pressed') === 'true',
            'final capacity state',
        );
    }
    return { failures, measurements, width: innerWidth, preferenceWrites: Number(output('Preference writes')), plan };
};
