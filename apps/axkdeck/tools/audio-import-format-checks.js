window.runAudioImportFormatChecks = async function () {
    const failures = [];
    const check = (condition, message) => { if (!condition) failures.push(message); };
    const tick = () => new Promise(resolve => setTimeout(resolve, 50));
    let group;
    for (let attempt = 0; attempt < 100; attempt++) {
        group = document.querySelector('[aria-labelledby="audio-import-format-label"]');
        if (group && !group.querySelector('button').disabled) break;
        await tick();
    }
    if (!group) return {failures:['Sample format selector missing']};
    const buttons = [...group.querySelectorAll('button')];
    const dialog = document.querySelector('[role="dialog"]');
    const before = dialog.getBoundingClientRect();
    check(buttons[0].getAttribute('aria-pressed') === 'true', 'New dialog must start with a3k');
    buttons[1].click();
    await tick();
    check(buttons[1].getAttribute('aria-pressed') === 'true', 'Later format must be selectable');
    const after = dialog.getBoundingClientRect();
    check(before.x === after.x && before.y === after.y && before.width === after.width && before.height === after.height,
        'Changing format must not move or resize the dialog');
    check(after.x >= 0 && after.right <= innerWidth + 1 && after.y >= 0 && after.bottom <= innerHeight + 1,
        'Dialog must fit the viewport');
    const rects = buttons.map(button => button.getBoundingClientRect());
    check(rects[0].height === rects[1].height && rects[0].top === rects[1].top, 'Format segments must share geometry');
    const mode = document.querySelector('#audio-import-mode');
    mode.value = 'SAMPLE_BANK';
    mode.dispatchEvent(new Event('change', {bubbles:true}));
    await tick();
    const name = document.querySelector('#audio-import-sample-bank-name');
    check(!!name, 'Sample Bank name must remain accessible');
    check(group.getBoundingClientRect().bottom <= name.getBoundingClientRect().top,
        'Sample format must not overlap the bank name');
    check(dialog.scrollWidth <= dialog.clientWidth + 1, 'Dialog must not overflow horizontally');
    return {failures};
};
