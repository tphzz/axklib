// Hardware page ownership, local preferences, conversion notice and gain affordance.
window.runSampleEditorRegression = async function () {
    const failures = [], measurements = [];
    const assert = (value, label) => { if (!value) failures.push(label); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 90));
    const click = async (selector, text) => {
        const node = [...document.querySelectorAll(selector)].find(node => !text || node.textContent.trim() === text);
        if (!node) throw new Error(`Missing ${selector}: ${text}`);
        node.click(); await settle();
    };
    const input = async (label, value) => {
        const node = document.querySelector(`input[aria-label="${label}"]`);
        if (!node || node.disabled) throw new Error(`Missing editable input: ${label}`);
        node.value = String(value); node.dispatchEvent(new Event('input', { bubbles: true })); await settle();
    };
    const value = label => Number(document.querySelector(`input[aria-label="${label}"]`).value);
    const page = async (tab, subpage) => {
        await click('[role=tab]', tab);
        await click('.sample-pages button', subpage);
        const panel = document.querySelector('.sample-panel');
        assert(panel.scrollWidth <= panel.clientWidth + 1, `${subpage}: no horizontal overflow`);
    };
    const choice = async (label, option) => {
        const segment = document.querySelector(`button[aria-label="${label}: ${option}"]`);
        if (segment) { segment.click(); await settle(); }
        else {
            await click(`button[aria-label="${label}"][aria-haspopup="listbox"]`);
            await click(`[role="listbox"][aria-label="${label}"] [role="option"]`, option);
        }
    };
    await click('[aria-label="Inspect Sample A"]');
    await page('Trim/Loop', 'Waveform');
    const original = ['Wave start', 'Wave end', 'Loop start', 'Loop end'].map(value);
    assert(document.querySelector('.waveform-page [aria-label="Loop mode"]'), 'Waveform owns Loop mode');
    assert(!document.querySelector('.sample-transport [aria-label="Loop mode"]'), 'transport does not edit Loop mode');
    for (const [type, scale] of [['Length', 1], ['Time', 44100], ['Beat', 44100 * 60 / 126]]) {
        await page('Trim/Loop', 'Sample Info');
        await choice('End Type', type);
        assert(document.querySelector('[aria-label="Write count"]').textContent === '0', 'preferences do not submit');
        await page('Trim/Loop', 'Waveform');
        assert(value('Wave start') === original[0] && value('Loop start') === original[2], `${type}: starts remain absolute`);
        assert(Math.abs(value('Wave end') * scale - (original[1] - original[0])) <= 1, `${type}: wave end represents its own span`);
        assert(Math.abs(value('Loop end') * scale - (original[3] - original[2])) <= 1, `${type}: loop end represents its own span`);
        assert(!document.querySelector('[aria-label="Unsaved Sample edits"]'), `${type}: display is not dirty`);
    }
    await page('Trim/Loop', 'Sample Info');
    await choice('End Type', 'Time');
    await click('[aria-label="Inspect Sample B"]');
    assert(document.querySelector('.sample-info'), 'sample switch retains Sample Info');
    const retained = document.querySelector('[aria-label="End Type: Time"][aria-pressed="true"]') || document.querySelector('button[aria-label="End Type"]')?.textContent.includes('Time');
    assert(retained, 'End Type persists across sample switches');
    await click('[aria-label="Inspect Sample A"]');
    await page('Trim/Loop', 'Waveform');
    await input('Loop end', 0.5);
    await page('Trim/Loop', 'Sample Info');
    await choice('End Type', 'Address');
    await page('Trim/Loop', 'Waveform');
    assert(value('Loop end') === original[2] + 22050, 'editing Time writes exact absolute end address');
    await click('[aria-label="Undo Sample edit"]');
    assert(value('Loop end') === original[3], 'converted end edit is one undo');
    await choice('Loop mode', 'Reverse');
    assert(document.querySelector('[aria-label="Unsaved Sample edits"]'), 'Loop mode is a saved parameter');
    await click('[aria-label="Undo Sample edit"]');
    await page('Map/Out', 'Pitch');
    assert(document.querySelectorAll('.parameter-groups section').length === 2, 'Pitch has two bounded groups');
    assert(!document.querySelector('input[aria-label="Pitch bend range"]'), 'Pitch bend is not on Pitch');
    const subpages = [...document.querySelectorAll('.sample-pages button')].map(node => node.textContent.trim());
    assert(subpages.indexOf('Expansion & Velocity') < subpages.indexOf('Level scaling'), 'Expansion precedes scaling');
    if (location.search.includes('short-stereo')) {
        await click('[aria-label="Convert Sample format"]');
        await click('.dialog-checkbox');
        await click('.dialog-footer .primary-button', 'Convert');
    }
    await choice('Portamento type', 'Rate (fulltime)');
    await input('Portamento rate', 75);
    assert(document.querySelector('input[aria-label="Portamento time"]').disabled, 'Rate mode retains inactive Time');
    const panel = document.querySelector('.sample-panel');
    await click('.device-editor button', 'Save');
    assert(document.querySelector('.sample-panel') === panel && value('Portamento rate') === 75, 'Save retains page and converted values');
    assert(!document.querySelector('[aria-label="Unsaved Sample edits"]'), 'Save clears dirty marker');
    await input('Portamento rate', 76);
    await click('.device-editor button', 'Save');
    assert(!document.querySelector('[aria-label="Unsaved Sample edits"]'), 'second Save is editable and clean');
    await page('MIDI/CTRL', 'MIDI Set');
    assert(document.querySelector('input[aria-label="Pitch bend range"]'), 'MIDI Set owns Pitch bend');
    assert(document.querySelector('input[aria-label="Velocity Low Limit"]'), 'MIDI Set owns Velocity Low Limit');
    await page('Map/Out', 'Mix & Key');
    assert(document.querySelector('[aria-label="Poly/Mono"]'), 'Mix & Key owns Poly/Mono');
    await page('Filter', 'Filter');
    await choice('Filter type', 'BandElim');
    await input('Cutoff', 44); await input('Q / Width', 4); await input('Gain', 0);
    const gain = document.querySelector('[data-handle="gain"]');
    gain.focus(); await settle();
    measurements.push({ gainFocus: { active: document.activeElement === gain, focus: gain.matches(':focus'), visible: gain.matches(':focus-visible'), opacity: getComputedStyle(gain).opacity, hidden: gain.hidden, className: gain.className } });
    assert(getComputedStyle(gain).opacity === '1', 'gain affordance is keyboard visible');
    assert(getComputedStyle(gain).color !== getComputedStyle(gain).backgroundColor, 'selected gain icon contrasts with its background');
    assert(document.querySelector('[role=tooltip]')?.textContent.includes('Shift'), 'gain help describes fine movement');
    for (const type of ['keydown', 'keyup']) gain.dispatchEvent(new KeyboardEvent(type, { key: 'ArrowUp', bubbles: true }));
    await settle();
    assert(value('Gain') === 1 && value('Cutoff') === 44 && value('Q / Width') === 4, 'gain gesture only edits gain');
    await click('[aria-label="Undo Sample edit"]');
    assert(value('Gain') === 0, 'gain gesture undoes once');
    measurements.push({ viewport: [innerWidth, innerHeight], writes: document.querySelector('[aria-label="Write count"]').textContent });
    return { failures, measurements };
};
