/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it } from 'vitest';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

function rule(selector: string): string {
    const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    return appStyles.match(new RegExp(`${escaped}\\s*\\{[^}]+\\}`))?.[0] ?? '';
}

function section(start: string, end: string): string {
    return appStyles.slice(appStyles.indexOf(start), appStyles.indexOf(end, appStyles.indexOf(start) + start.length));
}

describe('WorkspaceShell layout contract', () => {
    it('uses one responsive library track for the navigator and workspace tabs', () => {
        const shellRule = rule('.app-shell');
        const headerRule = rule('.app-header');
        const brandRule = rule('.brand');
        const mediumLayout = section('@media (max-width: 1100px)', '@media (max-width: 850px)');
        const compactLayout = section('@media (max-width: 850px)', '@media (prefers-reduced-motion: reduce)');
        const collapsedLabels = section('@media (max-width: 1200px)', '@media (max-width: 720px)');

        expect(shellRule).toContain('--library-column-width: 248px');
        expect(shellRule).toContain('grid-template-columns: var(--library-column-width) minmax(520px, 1fr) 268px');
        expect(headerRule).toContain('grid-template-columns: var(--library-column-width) minmax(0, 1fr) auto auto');
        expect(brandRule).not.toMatch(/(?:^|\n)\s*width:/);
        expect(mediumLayout).toContain('--library-column-width: 220px');
        expect(compactLayout).toContain('--library-column-width: 205px');
        expect(collapsedLabels).not.toMatch(/\.brand\s*\{[^}]*(?:^|\n)\s*width:/);
    });
});
