/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { render, screen } from '@testing-library/svelte';
import { createRawSnippet } from 'svelte';
import { describe, expect, it, vi } from 'vitest';

import CollectionToolbar from './CollectionToolbar.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

describe('CollectionToolbar', () => {
    it('keeps search as the rightmost collection action', () => {
        render(CollectionToolbar, {
            props: {
                title: 'Samples',
                count: 1,
                query: '',
                onquerychange: vi.fn(),
                actionLabel: 'Import audio',
                actionIcon: 'upload',
                onaction: vi.fn(),
            },
        });

        const search = screen.getByRole('searchbox', { name: 'Search Samples' });
        expect(search.getAttribute('placeholder')).toBe('Search');
        const actions = search.closest('.collection-actions');
        expect(actions?.lastElementChild).toBe(search.closest('label'));
    });

    it('renders a cleaning action with the requested icon and accessible label', () => {
        const onaction = vi.fn();
        render(CollectionToolbar, {
            props: {
                title: 'Wave Data',
                count: 3,
                query: '',
                onquerychange: vi.fn(),
                actionLabel: 'Clean up unreferenced Wave Data',
                actionIcon: 'broom',
                onaction,
            },
        });

        const action = screen.getByRole('button', { name: 'Clean up unreferenced Wave Data' });
        expect(action.querySelector('[data-icon="broom"]')).toBeTruthy();
        action.click();
        expect(onaction).toHaveBeenCalledOnce();
    });

    it('renders custom count text and trailing controls immediately before search', () => {
        const trailingControls = createRawSnippet(() => ({
            render: () => '<button type="button">Multi view</button>',
        }));
        render(CollectionToolbar, {
            props: {
                title: 'Programs',
                count: 32,
                countText: '32 parts',
                query: '',
                onquerychange: vi.fn(),
                trailingControls,
            },
        });

        expect(screen.getByText('32 parts')).toBeTruthy();
        const actions = screen.getByRole('searchbox').closest('.collection-actions');
        expect(
            actions?.children
                .item(actions.children.length - 2)
                ?.contains(screen.getByRole('button', { name: 'Multi view' })),
        ).toBe(true);
        expect(actions?.lastElementChild).toBe(screen.getByRole('searchbox').closest('label'));
    });

    it('renders title controls immediately after the collection count', () => {
        const titleControls = createRawSnippet(() => ({
            render: () => '<button type="button">Saved System File details</button>',
        }));
        render(CollectionToolbar, {
            props: {
                title: 'Programs',
                count: 7,
                query: '',
                onquerychange: vi.fn(),
                titleControls,
            },
        });

        const title = screen.getByRole('heading', { name: 'Programs' }).closest('.collection-title');
        expect(title?.children.item(0)).toBe(screen.getByRole('heading', { name: 'Programs' }));
        expect(title?.children.item(1)?.textContent).toBe('7 items');
        expect(
            title?.children.item(2)?.contains(screen.getByRole('button', { name: 'Saved System File details' })),
        ).toBe(true);
    });

    it('keeps the compact filter checkmark centered inside its box', () => {
        render(CollectionToolbar, {
            props: {
                title: 'Samples',
                count: 4,
                query: '',
                onquerychange: vi.fn(),
                filterLabel: 'Show only standalone',
                filterChecked: true,
                onfilterchange: vi.fn(),
            },
        });

        const checkbox = screen.getByRole('checkbox', { name: 'Show only standalone' });
        expect(checkbox.classList).toContain('compact-checkbox');

        const controlRule = appStyles.match(/\.compact-checkbox\s*\{[^}]+\}/)?.[0];
        const glyphRule = appStyles.match(/\.compact-checkbox::before\s*\{[^}]+\}/)?.[0];
        expect(controlRule).toContain('width: 12px');
        expect(controlRule).toContain('height: 12px');
        expect(controlRule).toContain('appearance: none');
        expect(controlRule).toContain('place-content: center');
        expect(controlRule).toContain('overflow: hidden');
        expect(glyphRule).toContain('width: 3px');
        expect(glyphRule).toContain('height: 6px');

        const style = document.createElement('style');
        style.textContent = controlRule ?? '';
        document.head.append(style);

        const checkboxStyle = getComputedStyle(checkbox);
        expect(checkboxStyle.width).toBe('12px');
        expect(checkboxStyle.height).toBe('12px');
        expect(checkboxStyle.display).toBe('grid');
        expect(checkboxStyle.overflow).toBe('hidden');

        style.remove();
    });
});
