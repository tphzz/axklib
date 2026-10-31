window.runAppearanceChecks = async function () {
    const { contrastRatio, minimumGradientContrast } = await import('/tools/paint-contrast.js');
    const failures = [], measurements = [];
    const check = (value, message) => { if (!value) failures.push(message); };
    const settle = () => new Promise(resolve => setTimeout(resolve, 180));
    const style = node => getComputedStyle(node);
    const context = document.createElement('canvas').getContext('2d');
    const rgb = value => { context.clearRect(0, 0, 1, 1); context.fillStyle = value; context.fillRect(0, 0, 1, 1); return [...context.getImageData(0, 0, 1, 1).data].slice(0, 3); };
    const paint = (node, name) => {
        const value = style(node);
        const colors = value.backgroundImage === 'none' ? [value.backgroundColor] : value.backgroundImage.match(/rgba?\([^)]*\)/g);
        check(!!colors?.length, `${name}: measurable paint`);
        if (!colors?.length) return;
        const stops = colors.map(rgb);
        const contrast = stops.length > 1 ? minimumGradientContrast(rgb(value.color), stops) : contrastRatio(rgb(value.color), stops[0]);
        measurements.push({ name, color: value.color, background: value.backgroundImage === 'none' ? value.backgroundColor : value.backgroundImage, contrast, opacity: value.opacity, width: node.getBoundingClientRect().width, height: value.height });
        if (!node.disabled) check(contrast >= 4.5, `${name}: contrast ${contrast.toFixed(3)}`);
        return value;
    };
    for (let i = 0; i < 80 && !document.querySelector('.brand, .startup-brand'); i++) await settle();
    const fileInput = document.querySelector('.tx16w-import-dialog input[type="file"]');
    if (fileInput) {
        const busy = new URLSearchParams(location.search).get('stage') === 'tx16w-busy';
        const label = fileInput.closest('label');
        check(fileInput.disabled === busy, 'TX16W file-input busy lock retained');
        check(label.getAttribute('aria-disabled') === String(busy), 'TX16W file-picker label exposes disabled state');
        check(Math.abs(Number(style(label).opacity) - (busy ? 0.45 : 1)) < 0.001, 'TX16W label shares disabled action opacity');
        paint(label, `TX16W/${busy ? 'Busy' : 'Editable'}`);
        return { failures, measurements, stage: new URLSearchParams(location.search).get('stage') };
    }
    const startup = document.querySelector('.startup-brand');
    if (startup) {
        check(startup.textContent === 'axkdeck' && startup.children.length === 1, 'Startup title only');
        check(!startup.querySelector('span, svg, img, button'), 'Startup has no mark or new About action');
        check(style(startup.querySelector('strong')).fontSize === '14px', 'Startup title shared 14px size');
        check(document.querySelector('.startup-header').getBoundingClientRect().height === 40 * (parseFloat(style(document.documentElement).zoom) || 1), 'Startup header height retained');
        return { failures, measurements, stage: new URLSearchParams(location.search).get('stage') };
    }
    const brand = document.querySelector('.brand');
    check(brand.textContent.trim() === 'axkdeck' && !brand.querySelector('svg, img, .brand-mark'), 'Workspace title only');
    check(brand.getAttribute('aria-label') === 'About axkdeck', 'Workspace About label retained');
    check(style(brand.querySelector('strong')).fontSize === '14px', 'Workspace title 14px');
    for (const row of document.querySelectorAll('[data-action-state]')) {
        const disabled = row.dataset.actionState === 'Disabled';
        for (const node of row.querySelectorAll('button')) {
            const value = paint(node, `${row.dataset.actionState}/${node.className}`);
            check(value.height === '30px' && value.marginTop === '0px' && value.marginBottom === '0px', 'Action geometry retained');
            check(value.color === 'rgb(255, 255, 255)', 'Action foreground is white');
            check(value.filter === 'none', 'No action brightness filter');
            check(disabled ? Math.abs(Number(value.opacity) - 0.45) < 0.001 && node.disabled : value.opacity === '1', 'Disabled state distinct');
            if (node.classList.contains('primary-button')) check(value.backgroundImage.startsWith('linear-gradient('), 'Primary is gradient');
            else check(value.backgroundColor === 'rgb(11, 13, 15)', 'Secondary base is deep');
        }
    }
    for (const node of document.querySelectorAll('.image-empty-actions button')) {
        const value = paint(node, `Compact/${node.textContent.trim()}`);
        check(value.height === '28px' && value.fontSize === '10px', 'Compact actions retain geometry/type');
    }
    const footer = document.querySelector('.dialog-footer-actions');
    for (const node of footer.querySelectorAll('button')) {
        const value = paint(node, `Footer/${node.textContent.trim()}`);
        check(value.height === '30px' && value.marginTop === '0px' && value.marginBottom === '0px', 'Mixed footer alignment');
        check(node.scrollWidth <= node.clientWidth + 1, 'Long command label contained');
    }
    const blue = document.querySelector('[aria-label="Blue access choice"] [aria-pressed="true"]');
    check(style(blue).color === 'rgb(11, 13, 15)' && style(blue).backgroundColor === 'rgb(104, 151, 187)', 'Blue control pairing unchanged');
    const danger = footer.querySelector('.danger-button');
    check(style(danger).backgroundColor === 'rgb(124, 48, 48)' && style(danger).color === 'rgb(255, 255, 255)', 'Danger pairing unchanged');
    const mode = document.querySelector('[aria-label="Workspace mode"]');
    const device = mode.querySelector('button:first-child'), files = mode.querySelector('button:last-child');
    device.click(); await settle();
    const selectedDevice = paint(device, 'Mode/Device');
    check(selectedDevice.backgroundImage.startsWith('linear-gradient(') && selectedDevice.color === 'rgb(255, 255, 255)', 'Device shares primary paint');
    check(style(document.querySelector('.app-header')).borderBottomColor === 'rgb(220, 139, 53)', 'Device decorative accent unchanged');
    files.click(); await settle();
    const selectedFiles = paint(files, 'Mode/Files');
    check(selectedFiles.backgroundImage === 'none' && selectedFiles.backgroundColor === 'rgb(88, 189, 180)' && selectedFiles.color === 'rgb(16, 18, 20)', 'Files pairing unchanged');
    check(style(document.querySelector('.app-header')).borderBottomColor === 'rgb(88, 189, 180)', 'Files decorative accent unchanged');
    device.click(); await settle();
    const focus = document.querySelector('[data-action-state="Focus"] .primary-button');
    focus.focus(); await settle();
    check(document.activeElement === focus, 'Action focus retained');
    const rowBounds = focus.getBoundingClientRect();
    check(rowBounds.right <= document.documentElement.clientWidth + 1, 'Supported narrow configuration contains controls');
    brand.focus(); brand.click(); await settle();
    const about = document.querySelector('[role="dialog"][aria-labelledby="about-title"]');
    check(!!about?.querySelector('.about-mark svg'), 'About artwork retained');
    about?.querySelector('.dialog-footer button').click(); await settle();
    check(!document.querySelector('[aria-labelledby="about-title"]') && document.activeElement === brand, 'About restores title focus');
    return { failures, measurements, stage: 'workspace' };
};
