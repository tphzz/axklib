import { render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { describe, expect, it, vi } from 'vitest';
import { EditorDraft } from '../../features/object-editor/draft.svelte';
import type { SamplerObject } from '../transport';
import ObjectSizeIdentity from './ObjectSizeIdentity.svelte';

const context = vi.hoisted(() => ({ workflow: {} as unknown }));
vi.mock('../../features/object-editor/context', () => ({ objectEditors: () => context.workflow }));

describe('object identity dirty marker', () => {
    it.each([undefined, 'stereo'] as const)('reserves the same slot for clean, dirty and undo (%s)', (indicator) => {
        const draft = new EditorDraft({ level: 100 });
        context.workflow = { documents: [{ detail: { object: { id: 'sample' } }, draft }] };
        const { container } = render(ObjectSizeIdentity, {
            name: 'Sample',
            object: { key: 'sample', storedSizeBytes: 356, sizeWithDependenciesBytes: 610304 } as SamplerObject,
            metadata: 'Standalone',
            indicator,
        });
        const slot = container.querySelector('.object-size-dirty');
        expect(slot).not.toBeNull();
        expect(slot?.getAttribute('aria-hidden')).toBe('true');
        expect(slot?.getAttribute('aria-label')).toBeNull();
        flushSync(() => draft.set('level', 99));
        expect(container.querySelector('.object-size-dirty')).toBe(slot);
        expect(slot?.getAttribute('aria-hidden')).toBe('false');
        expect(slot?.getAttribute('aria-label')).toBe('Unsaved Sample edits');
        flushSync(() => draft.undo());
        expect(container.querySelector('.object-size-dirty')).toBe(slot);
        expect(slot?.getAttribute('aria-hidden')).toBe('true');
        expect(container.querySelector('small')?.textContent).toContain('Standalone');
    });
});
