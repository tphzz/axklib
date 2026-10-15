import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { nativeDocumentFixture, nativeObjects } from '../../../test/nativeDocumentFixture';
import { InspectorPanels } from '../../../lib/inspectorPanels.svelte';
import type { components } from '../../../lib/generated/axklibApiV1';
import ObjectDetail from './ObjectDetail.svelte';
import ObjectDetailHarness from './ObjectDetail.testHarness.svelte';

type Detail = components['schemas']['Tx16wObjectDetail'];
const panel = (name: string) => screen.getByRole('button', { name: new RegExp(`^${name}(?:\\s|$)`) });
function region(button: HTMLElement): HTMLElement {
    const id = button.getAttribute('aria-controls');
    expect(id).toBeTruthy();
    const result = document.getElementById(id!);
    expect(result).not.toBeNull();
    return result!;
}
async function fixture(kind: 'wave' | 'voice' | 'performance' | 'setup' = 'wave') {
    const transport = nativeDocumentFixture();
    const wave = await transport.nativeObjectDetail(1, 'wave-20');
    if (wave.formatId !== 'yamaha.tx16w') throw new Error('Expected TX16W fixture detail');
    const details: Detail['details'] =
        kind === 'voice'
            ? { type: 'voice', slot: 1, regions: [] }
            : kind === 'performance'
              ? { type: 'performance', slot: 1, voices: [] }
              : kind === 'setup'
                ? { type: 'setup', waves: [], writeProtected: false }
                : wave.details;
    return {
        detail: { ...wave, type: kind, details },
        relationships: await transport.nativeRelationships(1),
        objects: nativeObjects,
        sources: [],
        onselect: vi.fn(),
        onrevealFiles: vi.fn(),
    };
}

describe('TX16W shared inspector panels', () => {
    it('uses shared sections with accessible regions and the A-series expansion defaults', async () => {
        render(ObjectDetail, await fixture());
        const ids: string[] = [];
        for (const [name, expanded] of [
            ['Preview', true],
            ['Properties', true],
            ['Relationships', false],
            ['Stored format', false],
        ] as const) {
            const button = panel(name);
            expect(button.getAttribute('aria-expanded')).toBe(String(expanded));
            const body = region(button);
            expect(body.hidden).toBe(!expanded);
            expect(button.closest('.inspector-section')).not.toBeNull();
            ids.push(body.id);
        }
        expect(new Set(ids).size).toBe(ids.length);
        const footer = screen.getByRole('button', { name: 'To Files' }).closest('.inspector-mode-footer');
        expect(footer).not.toBeNull();
        expect(footer?.closest('.inspector-content')).toBeNull();
    });

    it.each([
        ['voice', 'Key regions'],
        ['performance', 'Voice assignments'],
        ['setup', 'Wave slots'],
    ] as const)('starts native %s details expanded', async (kind, heading) => {
        render(ObjectDetail, await fixture(kind));
        expect(panel(heading).getAttribute('aria-expanded')).toBe('true');
        expect(region(panel(heading)).hidden).toBe(false);
        expect(screen.queryByRole('button', { name: /^Preview/ })).toBeNull();
    });

    it('hides collapsed relationship targets and preserves working navigation when reopened', async () => {
        const props = await fixture();
        render(ObjectDetail, props);
        const button = panel('Relationships');
        expect(screen.queryByRole('button', { name: 'Candidate: Wave 21' })).toBeNull();
        await fireEvent.click(button);
        const target = within(region(button)).getByRole('button', { name: 'Candidate: Wave 21' });
        await fireEvent.click(target);
        expect(props.onselect).toHaveBeenCalledExactlyOnceWith('wave-21');
        await fireEvent.click(button);
        expect(region(button).hidden).toBe(true);
        await fireEvent.click(button);
        expect(within(region(button)).getByRole('button', { name: 'Candidate: Wave 21' })).toBe(target);
    });

    it('retains choices through same-kind changes, document remounts and inspector hide/show', async () => {
        const props = await fixture();
        const panels = new InspectorPanels();
        const view = render(ObjectDetailHarness, { ...props, panels });
        await fireEvent.click(panel('Properties'));
        await fireEvent.click(panel('Relationships'));
        await fireEvent.click(panel('Stored format'));
        await view.rerender({ detail: { ...props.detail, objectId: 'wave-21', name: 'Wave 21' } });
        expect(panel('Properties').getAttribute('aria-expanded')).toBe('false');
        await view.rerender({ visible: false });
        expect(screen.queryByRole('complementary')).toBeNull();
        await view.rerender({ visible: true, detail: { ...props.detail, documentId: 'second-document' } });
        expect(panel('Properties').getAttribute('aria-expanded')).toBe('false');
        expect(panel('Relationships').getAttribute('aria-expanded')).toBe('true');
        expect(panel('Stored format').getAttribute('aria-expanded')).toBe('true');
    });

    it('isolates choices by native object kind and from an independent inspector context', async () => {
        const props = await fixture();
        const panels = new InspectorPanels();
        panels.setExpanded('wave', 'properties', false);
        const view = render(ObjectDetailHarness, { ...props, panels });
        expect(panel('Properties').getAttribute('aria-expanded')).toBe('true');
        await fireEvent.click(panel('Properties'));
        await view.rerender({ detail: (await fixture('voice')).detail });
        expect(panel('Properties').getAttribute('aria-expanded')).toBe('true');
        await view.rerender({ detail: props.detail });
        expect(panel('Properties').getAttribute('aria-expanded')).toBe('false');
        view.unmount();
        render(ObjectDetailHarness, { ...props, panels: new InspectorPanels() });
        expect(panel('Properties').getAttribute('aria-expanded')).toBe('true');
        expect(panel('Relationships').getAttribute('aria-expanded')).toBe('false');
        expect(panel('Stored format').getAttribute('aria-expanded')).toBe('false');
    });
});
