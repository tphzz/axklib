window.runSampleEditorRegression = async function () {
    const failures = [], measurements = [];
    const assert = (value, message) => { if (!value) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 100));
    const click = async (selector, text) => {
        const node = [...document.querySelectorAll(selector)].find(node => !text || node.textContent.trim() === text);
        if (!node) throw new Error(`Missing ${selector}: ${text}`);
        node.click(); await settle();
    };
    const color = (css) => css.match(/[\d.]+/g).map(Number);
    const luminance = (rgb) => rgb.slice(0, 3).map(value => {
        const channel = value / 255;
        return channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4;
    }).reduce((sum, value, index) => sum + value * [0.2126, 0.7152, 0.0722][index], 0);
    const badgeBorder = (state) => {
        const badge = document.querySelector('[aria-label="Inspect Sample A"] .format-badge');
        const row = badge.closest('.contained-row');
        const style = getComputedStyle(badge), rowStyle = getComputedStyle(row);
        const base = color(getComputedStyle(row.closest('.contained-object-workspace')).backgroundColor);
        const stops = rowStyle.backgroundImage.match(/rgba?\([^)]+\)/g) ?? [rowStyle.backgroundColor];
        const ink = luminance(color(style.borderTopColor));
        const contrasts = stops.map(stop => {
            const rgb = color(stop), alpha = rgb[3] ?? 1;
            const background = luminance(rgb.slice(0, 3).map((value, index) => value * alpha + base[index] * (1 - alpha)));
            return (Math.max(ink, background) + 0.05) / (Math.min(ink, background) + 0.05);
        });
        assert(contrasts.every(value => value >= 3), `${state} badge border remains distinct from the row background`);
        const widths = ['Top', 'Right', 'Bottom', 'Left'].map(side => {
            const width = Number.parseFloat(style[`border${side}Width`]);
            // Native page zoom can report device-pixel-snapped fractional CSS widths.
            assert(width > 0 && style[`border${side}Style`] === 'solid', `${state} badge keeps its ${side.toLowerCase()} border`);
            return width;
        });
        const rect = badge.getBoundingClientRect();
        measurements.push({ badgeState: state, border: style.borderTopColor, widths, contrasts });
        return [rect.width, rect.height];
    };
    const initialBadge = badgeBorder('active');
    await click('[aria-label="Inspect Sample A"]');
    const selectedBadge = badgeBorder('selected');
    assert(initialBadge.every((value, index) => Math.abs(value - selectedBadge[index]) < 1), 'selection keeps badge geometry');
    await click('[aria-label="Inspect Sample B"]');
    badgeBorder('unselected');
    await click('[aria-label="Inspect Sample A"]');
    await click('[role=tab]', 'Map/Out');
    const native = location.search.includes('short-stereo');
    const routing = async (a3k) => {
        await click('.sample-pages button', 'Mix & Key');
        for (const [first, label, lastNative] of [[true, a3k ? 'Main output' : 'Output 1', 4], [false, a3k ? 'Assignable output' : 'Output 2', 5]]) {
            const control = document.querySelector(`.editor-select[aria-label="${label}"]`);
            assert(!!control, `${label}: generation-specific routing label`);
            assert(!control.closest('.parameter-field').querySelector('.field-label .extended-parameter'), `${label}: no blanket extension marker`);
            const value = control.textContent;
            control.click(); await settle();
            const popup = document.querySelector('.editor-select-popup');
            const options = [...popup.querySelectorAll('[role=option]')];
            assert(options.length === 13, `${label}: complete destination list`);
            for (const [index, option] of options.entries()) {
                const extended = index > lastNative;
                assert((option.getAttribute('aria-disabled') === 'true') === (a3k && extended), `${label}: destination ${index} keeps its stored-format range`);
                assert(!!option.querySelector('.extended-parameter') === extended, `${label}: destination ${index} extension marker`);
            }
            const crossGroup = options[first ? 5 : 6];
            if (a3k) {
                assert(crossGroup.title.includes(first ? 'Assignable output' : 'Main output'), `${label}: unavailable destination points to the other native group`);
                crossGroup.click(); await settle();
                assert(control.textContent === value && popup.isConnected, `${label}: disabled choice cannot change routing`);
            }
            assert(options[first ? 6 : 2].title.includes('AIEB1'), `${label}: optional hardware explained separately`);
            for (const option of options.slice(10)) {
                assert(option.textContent.match(/A5000/g)?.length === 1, `${label}: later effects show one A5000 label`);
                assert(option.title.includes('requires A5000') && !option.title.includes('select this destination under'), `${label}: A5000-only effect has no false native-group redirect`);
            }
            popup.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
            await settle();
        }
        measurements.push({ routingFormat: a3k ? 'a3k' : 'a4k/a5k', viewport: innerWidth });
        await click('.sample-pages button', 'Pitch');
    };
    await routing(native);
    await click('.sample-pages button', 'Pitch');
    const accentReference = document.createElement('span');
    accentReference.style.color = 'var(--color-accent)';
    document.body.append(accentReference);
    const accentColor = getComputedStyle(accentReference).color;
    accentReference.remove();
    const inlineMarker = (node, name, textSelector = '.editor-option-text') => {
        const text = node.querySelector(textSelector), marker = node.querySelector('.extended-parameter');
        assert(text && marker, `${name}: label and extension marker exist`);
        if (!text || !marker) return;
        const label = text.getBoundingClientRect(), icon = marker.getBoundingClientRect();
        assert(Math.abs(label.y + label.height / 2 - icon.y - icon.height / 2) < 1.1, `${name}: inline centered marker`);
        assert(icon.x >= label.right - 1 && icon.x - label.right <= 5, `${name}: marker immediately follows text`);
        assert(icon.width >= 9 && icon.right <= node.getBoundingClientRect().right + 1, `${name}: marker fits without shrinking`);
        assert(getComputedStyle(marker).color === accentColor, `${name}: shared accent marker color`);
    };
    const optionMarkers = () => {
        const popup = document.querySelector('.editor-select-popup');
        assert(popup.parentElement === document.body, 'option marker styling survives the popup portal');
        const options = [...popup.querySelectorAll('[role=option]')];
        const height = options[0].getBoundingClientRect().height;
        for (const option of options) {
            assert(Math.abs(option.getBoundingClientRect().height - height) < 1, `${option.textContent}: same single-line height`);
            if (option.querySelector('[data-icon="plus"]')) {
                inlineMarker(option, option.textContent.trim());
                assert(option.title.includes(option.textContent.trim()), 'full option label remains in tooltip');
                if (option.getAttribute('aria-disabled') === 'true') {
                    assert(Number(getComputedStyle(option).opacity) < 1, 'unavailable portaled option retains disabled appearance');
                    assert(option.title.includes('Convert'), 'unavailable option retains conversion explanation');
                }
            }
        }
        assert(popup.scrollWidth <= popup.clientWidth + 1, 'narrow popup has no horizontal overflow');
        measurements.push({ markerOptions: options.length, rowHeight: height, popupWidth: popup.clientWidth });
    };
    const portamento = document.querySelector('button[aria-label="Portamento type"]');
    const choice = portamento.closest('.choice-control');
    const savedChoiceWidth = choice.style.width;
    choice.style.width = '120px'; await settle();
    portamento.click(); await settle();
    // Exercise truncation below the popup's preferred 200px minimum.
    document.querySelector('.editor-select-popup').style.width = '90px'; await settle();
    optionMarkers();
    assert([...document.querySelectorAll('.editor-option-text')].some(node => node.scrollWidth > node.clientWidth), 'narrow label truncation is exercised');
    document.querySelector('.editor-select-popup').dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true }));
    await settle(); optionMarkers();
    document.querySelector('.editor-select-popup').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    choice.style.width = savedChoiceWidth;
    await click('[role=tab]', 'EG');
    await click('.sample-pages button', 'Amplitude');
    const mode = [...document.querySelectorAll('.choice-control')].find(node => node.querySelector('[aria-label="Attack mode"]'));
    mode.style.width = '420px'; await settle();
    const segment = mode.querySelector('.editor-choice:last-child');
    assert(segment, 'attack mode uses segments when they fit');
    if (segment) inlineMarker(segment, 'Attack mode segment');
    const segmentWidth = mode.querySelector('.choice-measurement').scrollWidth;
    mode.style.width = `${segmentWidth + 10}px`; await settle();
    if (mode.querySelector('.editor-choices')) {
        for (const label of mode.querySelectorAll('.editor-choices .editor-option-text'))
            assert(label.scrollWidth <= label.clientWidth + 1, 'segmented measurement includes marker width');
    }
    mode.style.width = '70px'; await settle();
    assert(mode.querySelector('.editor-select'), 'tight segments adapt to a dropdown');
    mode.style.width = '';
    await click('[role=tab]', 'MIDI/CTRL');
    await click('.sample-pages button', 'Control');
    const editor = document.querySelector('.device-editor');
    const table = document.querySelector('table[aria-label="Sample MIDI controls"]');
    const oldWidth = editor.style.width;
    for (const width of [1000, 600, 390]) {
        editor.style.width = `${width}px`; await settle();
        const wide = !editor.matches('[data-editor-under~="700"]');
        const headers = [...table.querySelectorAll('thead th')];
        assert(headers.filter(node => node.querySelector('.extended-parameter')).length === 2, 'only Controller and Function headers have extension markers');
        for (const header of headers.filter(node => node.querySelector('.extended-parameter'))) {
            if (wide) inlineMarker(header, 'Control table heading', '.column-label');
        }
        for (const row of table.querySelectorAll('tbody tr')) {
            for (const cell of row.querySelectorAll('td')) {
                const marker = cell.querySelector('.field-label .extended-parameter');
                if (marker) {
                    assert((getComputedStyle(marker).display !== 'none') === !wide, 'field markers follow visible narrow labels only');
                    if (!wide) inlineMarker(cell.querySelector('.field-label'), 'Narrow MIDI label', '.parameter-label-text');
                }
                if (wide) {
                    const field = cell.querySelector('.parameter-field');
                    const control = cell.querySelector('.editor-autocomplete,.editor-number');
                    assert(Math.abs(field.getBoundingClientRect().x - control.getBoundingClientRect().x) < 1, 'table input reclaims former marker space');
                }
            }
        }
        measurements.push({ midiWidth: editor.clientWidth, headerVisible: wide });
    }
    editor.style.width = oldWidth; await settle();
    const functionInput = document.querySelector('input[aria-label="Control 1 Function"]');
    functionInput.value = 'Portamento'; functionInput.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    assert(document.querySelectorAll('.editor-select-popup [role=option]').length === 1, 'autocomplete searches labels without marker text');
    optionMarkers();
    functionInput.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    await settle();
    assert(!document.querySelector('.editor-select-popup'), 'Escape still dismisses autocomplete');
    await click('[role=tab]', 'Map/Out');
    await click('.sample-pages button', 'Pitch');
    const source = native ? 'a3k' : 'a4k/a5k', target = native ? 'a4k/a5k' : 'a3k';
    const input = document.querySelector('input[aria-label="Coarse tune"]');
    input.value = '1'; input.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    await click(`[aria-label="Convert to ${target} sample format"]`);
    let modal = document.querySelector('[role=dialog]');
    assert(modal.textContent.includes('Save or discard'), 'conversion requires saved draft');
    assert(modal.querySelector('.primary-button').disabled, 'dirty conversion cannot submit');
    await click('.dialog-footer button', 'Cancel');
    await click('.device-editor button', 'Save');
    assert(document.querySelector('.device-editor .format-badge').textContent === source, 'ordinary save retains format');
    const panel = document.querySelector('.sample-panel');
    await click(`[aria-label="Convert to ${target} sample format"]`);
    modal = document.querySelector('[role=dialog]');
    assert(!modal.querySelector('.primary-button').disabled, 'clean conversion needs only explicit confirmation');
    assert(!modal.querySelector('input[type=checkbox]'), 'confirmation has no acknowledgement checkbox');
    assert(!/bytes|prefix|extension|Do not treat/.test(modal.textContent), 'confirmation avoids storage implementation details');
    assert(modal.getAttribute('aria-label') === `Convert to ${target} sample format`, 'confirmation names the target');
    const actions = [...modal.querySelectorAll('.dialog-footer-actions button')];
    const sizes = actions.map(node => ({ height: node.getBoundingClientRect().height, margin: getComputedStyle(node).marginBlock }));
    assert(Math.abs(sizes[0].height - sizes[1].height) < 1 && sizes.every(item => item.margin === '0px'), 'shared footer geometry');
    assert(modal.scrollWidth <= modal.clientWidth + 1, 'dialog has no horizontal overflow');
    const writesBefore = document.querySelector('[aria-label="Write count"]').textContent;
    await click('.dialog-footer .primary-button', 'Convert');
    if (location.search.includes('conversion-lock')) {
        modal = document.querySelector('[role=dialog]');
        assert(!!modal && modal.textContent.includes('Conversion not started'), 'lock rejection remains a failed operation, not a refresh');
        assert(document.querySelector('[aria-label="Write count"]').textContent === writesBefore, 'lock rejection did not write');
        assert(!modal.querySelector('.secondary-button').disabled && !modal.querySelector('[aria-label=Close]').disabled, 'rejection restores both dismissal controls');
        await click('.dialog-footer button', 'Cancel');
        assert(!document.querySelector('[role=dialog]'), 'rejected conversion can be dismissed');
        await click('nav button', 'Close image');
        assert(document.querySelector('[aria-label=Closed]').textContent === 'true', 'rejected conversion releases image exit guard');
        await click(`[aria-label="Convert to ${target} sample format"]`);
        await click('.dialog-footer .primary-button', 'Convert');
    }
    assert(!document.querySelector('[role=dialog]'), 'confirmed conversion closes');
    assert(document.querySelector('.sample-panel') === panel, 'conversion retains editor document and page');
    assert(document.querySelector('.device-editor .format-badge').textContent === target, 'editor badge reflects new storage');
    const row = document.querySelector('[aria-label="Inspect Sample A"]');
    assert(row.querySelector('.format-badge').textContent === target, 'collection badge refreshes');
    assert(document.querySelector('[aria-label="Inspect Sample B"] .format-badge').textContent === source, 'other Sample format is unchanged');
    const coarse = document.querySelector('input[aria-label="Coarse tune"]');
    assert(!coarse.disabled && Number(coarse.value) === 1, 'conversion retains parameter and editing');
    coarse.value = '2'; coarse.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    await click('.device-editor button', 'Save');
    assert(!document.querySelector('.object-size-dirty.dirty'), 'further save clears dirty state');
    await routing(!native);
    measurements.push({ viewport: [innerWidth, innerHeight], sizes, source, target, writes: document.querySelector('[aria-label="Write count"]').textContent });
    return { failures, measurements };
};
