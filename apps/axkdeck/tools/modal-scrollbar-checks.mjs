// Runs unchanged in Chromium and the Linux WebKitGTK web view.
export async function checkModalScrollbars({ folders = false } = {}) {
    const assert = (condition, message) => {
        if (!condition) throw new Error(message);
    };
    const settle = () => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    const wait = async (predicate) => {
        const deadline = performance.now() + 5000;
        while (!predicate()) {
            assert(performance.now() < deadline, 'Timed out waiting for modal state');
            await new Promise((resolve) => setTimeout(resolve, 20));
        }
        await settle();
    };
    const button = (text, scope = document) =>
        [...scope.querySelectorAll('button')].find((node) => node.textContent.trim() === text);
    const geometry = (node) => [
        node.clientWidth,
        node.clientHeight,
        node.scrollWidth,
        node.scrollHeight,
        node.scrollTop,
        node.scrollLeft,
    ];
    const transparent = (node) => getComputedStyle(node).scrollbarColor === 'rgba(0, 0, 0, 0) rgba(0, 0, 0, 0)';
    await wait(() => document.querySelectorAll('.background-pane').length === 4);
    const panes = [...document.querySelectorAll('.background-pane')];
    assert(panes.length === 4, 'Four background panes required');
    for (const pane of panes) {
        assert(pane.scrollHeight > pane.clientHeight, 'Background must overflow vertically');
        pane.scrollTop = 240;
        pane.scrollLeft = 60;
    }
    await settle();
    const before = panes.map(geometry);
    const originalColors = panes.map((node) => getComputedStyle(node).scrollbarColor);
    const checkBackground = (hidden) => {
        assert(
            JSON.stringify(panes.map(geometry)) === JSON.stringify(before),
            'Background geometry or scroll position changed',
        );
        panes.forEach((pane, index) => {
            assert(Boolean(pane.closest('[inert]')) === hidden, 'Background inert state wrong');
            assert(
                hidden ? transparent(pane) : getComputedStyle(pane).scrollbarColor === originalColors[index],
                'Scrollbar paint state wrong',
            );
            const removedOverlay = hidden && document.documentElement.classList.contains('modal-overlay-scrollbars');
            assert(
                getComputedStyle(pane).scrollbarWidth === (removedOverlay ? 'none' : 'thin'),
                'Scrollbar mode wrong',
            );
            if (hidden) {
                assert(
                    getComputedStyle(pane, '::-webkit-scrollbar-thumb').visibility === 'hidden',
                    'WebKit thumb remains visible',
                );
            }
        });
    };
    const openPicker = async () => {
        button('Import floppy').focus();
        button('Import floppy').click();
        await wait(() => document.querySelectorAll('.storage-picker-row').length > 10);
    };
    const escape = async () => {
        document.activeElement.dispatchEvent(
            new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }),
        );
        await settle();
    };
    await openPicker();
    checkBackground(true);
    const picker = document.querySelector('.storage-picker');
    const list = picker.querySelector('.storage-picker-list');
    assert(!list.closest('[inert]') && !transparent(list), 'Foreground scrolling suppressed');
    assert(list.scrollHeight > list.clientHeight, 'Picker must overflow');
    const footer = picker.querySelector('.dialog-footer');
    const footerBefore = JSON.stringify(footer.getBoundingClientRect().toJSON());
    list.focus();
    list.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true, cancelable: true }));
    await settle();
    assert(list.scrollTop > 0, 'Foreground keyboard scrolling failed');
    const foregroundScroll = list.scrollTop;
    assert(JSON.stringify(footer.getBoundingClientRect().toJSON()) === footerBefore, 'Footer moved during scrolling');
    if (folders) {
        const parentGeometry = geometry(list);
        button('New folder').focus();
        button('New folder').click();
        await wait(() => document.querySelector('[aria-label="Create folder"]'));
        checkBackground(true);
        assert(list.closest('[inert]') && transparent(list), 'Underlying picker scrollbar remains painted');
        assert(!document.querySelector('[aria-label="Folder name"]').closest('[inert]'), 'Nested dialog is inert');
        await escape();
        assert(!document.querySelector('[aria-label="Create folder"]'), 'Nested Escape failed');
        assert(!list.closest('[inert]') && !transparent(list), 'Parent scrollbar was not restored');
        assert(
            JSON.stringify(geometry(list)) === JSON.stringify(parentGeometry),
            'Nested modal changed parent geometry',
        );
        assert(document.activeElement === button('New folder'), 'Nested focus not restored');
        checkBackground(true);
    }
    await escape();
    assert(!document.querySelector('[aria-modal="true"]'), 'Picker did not close');
    checkBackground(false);
    assert(document.activeElement === button('Import floppy'), 'Picker focus not restored');
    button('About').focus();
    button('About').click();
    await wait(() => document.querySelector('.about-dialog'));
    checkBackground(true);
    await escape();
    checkBackground(false);
    assert(document.activeElement === button('About'), 'About focus not restored');
    await openPicker();
    checkBackground(true);
    const finalPicker = document.querySelector('.storage-picker');
    const firstRow = finalPicker.querySelector('.storage-picker-row').getBoundingClientRect();
    const paneRight = Math.round(panes[0].getBoundingClientRect().right);
    const pixelProbe = {
        left: paneRight - 14,
        right: paneRight - 2,
        y: Math.ceil(firstRow.bottom) + 2,
        referenceX: paneRight - 28,
        color: getComputedStyle(finalPicker).backgroundColor.match(/\d+/g).slice(0, 3).map(Number),
    };
    return { folders, panes: before, foregroundScroll, pixelProbe, userAgent: navigator.userAgent };
}
