import { readFileSync } from 'node:fs';
import { cleanup, fireEvent, render } from '@testing-library/svelte';
import { afterEach, expect, it, vi } from 'vitest';
import InspectorModeFooter from './InspectorModeFooter.svelte';

afterEach(cleanup);

it.each([
    ['device', 'To Device', 'Show this entry in Device'],
    ['files', 'To Files', 'Reveal this object in Files'],
] as const)('uses the shared fixed footer for %s navigation', async (mode, label, title) => {
    const onclick = vi.fn();
    const view = render(InspectorModeFooter, { mode, onclick });
    const button = view.getByRole('button', { name: label });
    expect(button.getAttribute('title')).toBe(title);
    expect(button.classList.contains('secondary-button')).toBe(true);
    expect(button.closest('footer.inspector-mode-footer')).toBeTruthy();
    await fireEvent.click(button);
    expect(onclick).toHaveBeenCalledOnce();
});

it('keeps identical fixed control geometry independent of destination mode', () => {
    const source = readFileSync('src/lib/components/InspectorModeFooter.svelte', 'utf8');
    expect(source).toMatch(/flex:\s*none/);
    expect(source).toMatch(/height:\s*30px/);
    expect(source).toMatch(/margin:\s*0/);
    expect(source).toMatch(/width:\s*100%/);
    expect(source.match(/<button\b/g)).toHaveLength(1);
});
