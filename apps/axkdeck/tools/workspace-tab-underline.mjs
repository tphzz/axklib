import assert from 'node:assert/strict';

export async function checkWorkspaceTabUnderline(tabs, label) {
    const active = tabs.locator('button.active');
    await active.waitFor({ state: 'visible' });
    const geometry = await active.evaluate(async (node) => {
        await Promise.all(node.getAnimations({ subtree: true }).map((animation) => animation.finished.catch(() => {})));
        const indicator = getComputedStyle(node, '::after');
        return {
            label: node.getAttribute('aria-label') ?? node.textContent.trim(),
            buttonWidth: node.getBoundingClientRect().width,
            underlineWidth: parseFloat(indicator.width),
            left: indicator.left,
            right: indicator.right,
            bottom: indicator.bottom,
            height: indicator.height,
            radius: indicator.borderRadius,
            opacity: indicator.opacity,
            color: indicator.backgroundColor,
            brand: getComputedStyle(node).getPropertyValue('--color-brand').trim(),
            transform: indicator.transform,
        };
    });
    assert.equal(geometry.left, '0px', `${label}: underline starts at the full tab edge`);
    assert.equal(geometry.right, '0px', `${label}: underline ends at the full tab edge`);
    assert.equal(geometry.bottom, '0px', `${label}: underline sits on the tab baseline`);
    assert.equal(geometry.height, '2px', `${label}: stable two-pixel underline`);
    assert.equal(geometry.radius, '0px', `${label}: square full-width underline`);
    assert.equal(geometry.opacity, '1', `${label}: active underline is visible`);
    assert.equal(geometry.transform, 'none', `${label}: active underline is not shortened`);
    assert(Math.abs(geometry.buttonWidth - geometry.underlineWidth) < 0.5, `${label}: underline spans padding and icon`);
    assert.equal(geometry.color, 'rgb(204, 120, 50)', `${label}: canonical brand underline`);
    return geometry;
}
