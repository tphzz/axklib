/// <reference types="node" />

import { readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { parse } from 'svelte/compiler';
import { describe, expect, it } from 'vitest';

interface Declaration {
    property: string;
    value: string;
    states: string;
}

function declarations(node: unknown, states = ''): Declaration[] {
    if (!node || typeof node !== 'object') return [];
    const value = node as Record<string, unknown>;
    if (value.type === 'Rule') states = JSON.stringify(value.prelude);
    if (value.type === 'Declaration') return [{ property: String(value.property), value: String(value.value), states }];
    return Object.values(value).flatMap((child) =>
        Array.isArray(child) ? child.flatMap((node) => declarations(node, states)) : declarations(child, states),
    );
}

const sources = readdirSync(resolve('src'), { recursive: true })
    .map(String)
    .filter((path) => /\.(svelte|css)$/.test(path))
    .map((path) => {
        const source = readFileSync(resolve('src', path), 'utf8');
        const ast = parse(path.endsWith('.css') ? `<style>${source}</style>` : source, { modern: true });
        return { path, source, declarations: declarations(ast.css) };
    });
const definitions = new Map(
    sources.flatMap((source) => source.declarations.map((entry) => [entry.property, entry.value])),
);
const app = sources.find((source) => source.path === 'app.css')!.source;

describe('design system contract', () => {
    it('resolves semantic color and typography names without obsolete aliases', () => {
        const unresolved = sources.flatMap(({ path, declarations }) =>
            declarations.flatMap(({ value }) =>
                [...value.matchAll(/var\((--[\w-]+)/g)]
                    .map((match) => match[1]!)
                    .filter((name) => /^(--color-|--text-|--danger$|--font-size-value$)/.test(name))
                    .filter((name) => !definitions.has(name))
                    .map((name) => `${path}: ${name}`),
            ),
        );
        expect(unresolved).toEqual([]);
        for (const { path, source } of sources) {
            expect(source, path).not.toMatch(/var\(--color-warning\s*,/);
            expect(source, path).not.toMatch(
                /--(?:text-muted|text-secondary|danger|color-bg-panel|color-surface|color-danger-text|color-border-strong|color-border-subtle|font-size-value)\b/,
            );
        }
    });

    it('defines approved contrast and interaction roles once', () => {
        expect(definitions.get('--color-warning')).toBe('#dfbd7b');
        expect(definitions.get('--color-action-text')).toBe('#0b0d0f');
        expect(definitions.get('--color-text-inverse')).toBe('#ffffff');
        expect(definitions.get('--color-danger-surface')).toBe('#7c3030');
        expect(definitions.get('--color-control-border')).toBe('#69747d');
        expect(definitions.get('--state-hover-surface')).toBe('var(--color-panel-raised)');
        expect(definitions.get('--state-selected-surface')).toBe('var(--color-selection)');
        expect(definitions.get('--state-active-border')).toBe('var(--color-accent)');
        expect(definitions.get('--state-focus')).toBe('var(--color-accent)');
        expect(app).toMatch(
            /\.primary-button\s*\{[^}]*color:\s*var\(--color-action-text\)[^}]*background:\s*var\(--color-brand\)/s,
        );
    });

    it('routes existing focus and active boundary consumers through the state aliases', () => {
        const bypasses = sources.flatMap(({ path, declarations }) =>
            declarations
                .filter(
                    ({ property, value, states }) =>
                        /^(border|outline|box-shadow)/.test(property) &&
                        value.includes('var(--color-accent)') &&
                        [
                            '"name":"focus"',
                            '"name":"focus-visible"',
                            '"name":"focus-within"',
                            '"name":"active"',
                            '"name":"selected"',
                            '"name":"aria-selected"',
                            '"name":"aria-pressed"',
                        ].some((state) => states.includes(state)),
                )
                .map(({ property }) => `${path}: ${property}`),
        );
        expect(bypasses).toEqual([]);
    });

    it('keeps brand and startup tracking at zero without enlarging text', () => {
        expect(app).toMatch(/\.brand strong\s*\{[^}]*font-size:\s*14px[^}]*letter-spacing:\s*0\s*;/s);
        const startup = sources.find((source) => source.path === 'StartupShell.svelte')!.source;
        expect(startup).toMatch(/\.startup-library p\s*\{[^}]*font-size:\s*9px[^}]*letter-spacing:\s*0\s*;/s);
    });

    it('shares compact Autoplay marks and removes independently styled switch copies', () => {
        for (const path of [
            'lib/components/AuditionBar.svelte',
            'features/devices/a-series/sample/SampleTransport.svelte',
        ]) {
            expect(sources.find((source) => source.path === path)!.source, path).toContain('class="compact-checkbox"');
        }
        expect(app.includes('.audition-autoplay input')).toBe(false);
        expect(
            sources.find((source) => source.path === 'features/diagnostics/logs.css')!.source.includes('.log-switch >'),
        ).toBe(false);
        expect(
            sources
                .find((source) => source.path === 'features/object-editor/editor.css')!
                .source.includes('.editor-switch'),
        ).toBe(false);
    });
});
