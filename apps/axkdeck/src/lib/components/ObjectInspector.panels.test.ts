import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { inspectorRelationshipFixture } from '../../test/inspectorRelationshipFixture';
import InspectorPanelsSessionHarness from '../../test/InspectorPanelsSessionHarness.svelte';
import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import type { InspectorSelection } from '../types';
import ObjectInspector from './ObjectInspector.svelte';

function selection(kind = 'sample', name = 'First sample'): NonNullable<InspectorSelection> {
    const fixture = inspectorRelationshipFixture(kind)!;
    if (fixture.kind !== 'sample') return fixture;
    if (kind === 'sequence') {
        const object = { ...fixture.item.object, objectType: 'SEQU', key: 'sequence', name: 'Test sequence' };
        return {
            kind,
            sequence: { id: object.key, objectId: object.key, name: object.name, object },
            relationships: [],
        };
    }
    const item = {
        ...fixture.item,
        id: name,
        objectId: name,
        name,
        object: {
            ...fixture.item.object,
            key: name,
            name,
            sampleFormat: sampleFormatFixture('A3000_188').sampleFormat,
        },
    };
    return { ...fixture, item, preview: { ...fixture.preview, item } };
}

function panelButton(title: string): HTMLElement {
    return screen.getByRole('button', { name: new RegExp(`^${title}(?:\\s|$)`) });
}

function controlledRegion(button: HTMLElement): HTMLElement {
    const id = button.getAttribute('aria-controls');
    expect(id).toBeTruthy();
    const region = document.getElementById(id!);
    expect(region).not.toBeNull();
    return region!;
}

describe('ObjectInspector expansion panels', () => {
    it('orders Stored format after Relationships inside the common padded content', () => {
        const { container } = render(ObjectInspector, { selection: selection() });
        const headings = Array.from(container.querySelectorAll('h4'));

        expect(headings.map((heading) => heading.textContent?.trim().replace(/\s+/g, ' '))).toEqual([
            'Preview',
            'Properties',
            'Relationships',
            'Stored format a3k',
        ]);
        const content = container.querySelector('.inspector-content');
        expect(content).not.toBeNull();
        for (const heading of headings) {
            expect(heading.closest('.inspector-content')).toBe(content);
            expect(heading.closest('.inspector-section')).not.toBeNull();
        }
    });

    it.each(['program', 'sample-bank', 'sample', 'wave-data', 'sequence'])(
        'starts %s sections with the intended defaults and individually labelled controls',
        (kind) => {
            const { container } = render(ObjectInspector, { selection: selection(kind) });
            const headings = Array.from(container.querySelectorAll('h4'));
            const regionIds: string[] = [];
            for (const heading of headings) {
                const button = within(heading).getByRole('button');
                const sectionId = heading.closest('.inspector-section')?.getAttribute('data-inspector-section');
                const collapsed = sectionId === 'relationships' || sectionId === 'stored-format';
                expect(button.getAttribute('type')).toBe('button');
                expect(button.getAttribute('aria-expanded')).toBe(String(!collapsed));
                const region = controlledRegion(button);
                expect(region.hidden).toBe(collapsed);
                expect(region.hasAttribute('inert')).toBe(false);
                regionIds.push(region.id);
            }
            expect(regionIds.length).toBeGreaterThanOrEqual(2);
            expect(new Set(regionIds).size).toBe(regionIds.length);
        },
    );

    it('keeps collapse choices across sample changes without leaking them into other inspector kinds', async () => {
        const view = render(ObjectInspector, { selection: selection() });
        await fireEvent.click(panelButton('Properties'));
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('false');
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('false');

        await view.rerender({ selection: selection('sample', 'Second sample') });
        expect(screen.getByRole('heading', { name: 'Second sample' })).toBeTruthy();
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('false');

        await view.rerender({ selection: selection('program') });
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('true');
        await fireEvent.click(panelButton('Relationships'));
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('true');

        await view.rerender({ selection: selection() });
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('false');
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('false');

        await view.rerender({ selection: selection('program') });
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('true');
    });

    it('keeps Stored format identification visible while its details start collapsed', () => {
        render(ObjectInspector, { selection: selection() });
        const button = panelButton('Stored format');
        const body = controlledRegion(button);
        expect(body.textContent).toContain('188 bytes');

        expect(button.getAttribute('aria-expanded')).toBe('false');
        expect(screen.getByText('a3k').closest('[hidden]')).toBeNull();
        const description = document.getElementById(button.getAttribute('aria-describedby') ?? '');
        expect(description?.textContent).toContain('a3k');
        expect(body.textContent).toContain('188 bytes');
        expect(body.closest('[hidden], [inert]')).not.toBeNull();
    });

    it('retains relationships when collapsed and restores working navigation when reopened', async () => {
        const onrelationshipnavigate = vi.fn();
        render(ObjectInspector, { selection: selection(), onrelationshipnavigate });
        const button = panelButton('Relationships');
        const body = controlledRegion(button);
        expect(button.getAttribute('aria-expanded')).toBe('false');
        await fireEvent.click(button);
        const link = within(body).getAllByRole('button')[0];
        const linkName = link.textContent!.trim().replace(/\s+/g, ' ');

        await fireEvent.click(button);
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('false');
        expect(screen.queryByRole('button', { name: linkName })).toBeNull();
        expect(body.contains(link)).toBe(true);
        expect(screen.getByRole('heading', { name: 'First sample' })).toBeTruthy();

        await fireEvent.click(button);
        expect(within(body).getAllByRole('button')[0]).toBe(link);
        await fireEvent.click(link);
        expect(onrelationshipnavigate).toHaveBeenCalledOnce();
    });

    it('restores the defaults in a new independently mounted inspector session', async () => {
        const first = render(ObjectInspector, { selection: selection() });
        await fireEvent.click(panelButton('Properties'));
        await fireEvent.click(panelButton('Stored format'));
        await fireEvent.click(panelButton('Relationships'));
        expect(panelButton('Stored format').getAttribute('aria-expanded')).toBe('true');
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('true');
        first.unmount();

        render(ObjectInspector, { selection: selection() });
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('true');
        expect(panelButton('Stored format').getAttribute('aria-expanded')).toBe('false');
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('false');
    });

    it('retains choices through app-owned inspector hide/show and isolates Files from Sample sections', async () => {
        const view = render(InspectorPanelsSessionHarness, { selection: selection(), mode: 'object' });
        await fireEvent.click(panelButton('Properties'));
        await fireEvent.click(panelButton('Stored format'));

        await view.rerender({ mode: 'files' });
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('true');
        expect(panelButton('Storage details').getAttribute('aria-expanded')).toBe('true');
        await fireEvent.click(panelButton('Storage details'));

        await view.rerender({ mode: null });
        expect(screen.queryByRole('complementary')).toBeNull();
        await view.rerender({ mode: 'object', selection: selection('sample', 'Another sample') });
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('false');
        expect(panelButton('Stored format').getAttribute('aria-expanded')).toBe('true');
        expect(panelButton('Relationships').getAttribute('aria-expanded')).toBe('false');

        await view.rerender({ mode: 'files' });
        expect(panelButton('Properties').getAttribute('aria-expanded')).toBe('true');
        expect(panelButton('Storage details').getAttribute('aria-expanded')).toBe('false');
    });
});
