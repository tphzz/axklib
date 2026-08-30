/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it } from 'vitest';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

function rule(selector: string): string {
    const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    return appStyles.match(new RegExp(`(?:^|})\\s*(${escaped}\\s*\\{[^}]+\\})`))?.[1] ?? '';
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
        expect(brandRule).toContain('height: 100%');
        expect(brandRule).toContain('border: 0');
        expect(brandRule).toContain('background: transparent');
        expect(brandRule).not.toMatch(/(?:^|\n)\s*width:/);
        expect(mediumLayout).toContain('--library-column-width: 220px');
        expect(compactLayout).toContain('--library-column-width: 205px');
        expect(collapsedLabels).not.toMatch(/\.brand\s*\{[^}]*(?:^|\n)\s*width:/);
    });

    it('uses one compact section contract throughout the object inspector', () => {
        const headingStackRule = rule('.panel-heading > div');
        const eyebrowRule = rule('.eyebrow');
        const panelTitleRule = rule('.panel-heading h2');
        const inspectorTitleRule = rule('.inspector-title');
        const inspectorTypeRule = rule('.inspector-title span');
        const inspectorNameRule = rule('.inspector-title h3');
        const sectionRule = rule('.inspector-section');
        const sectionHeadingRule = rule('.inspector-section > h4');
        const contentRule = rule('.inspector-content');
        const metadataRowRule = rule('.metadata-list div');
        const metadataDividerRule = rule('.metadata-list div:not(:last-child)');
        const relationshipsRule = rule('.inspector-relationships');
        const relationshipGroupRule = rule('.inspector-relationship-group h5');
        const laterRelationshipGroupRule = rule('.inspector-relationship-group + .inspector-relationship-group');
        const relationshipRowRule = rule('.inspector-relationship-group li');
        const relationshipDividerRule = rule('.inspector-relationship-group li:not(:last-child)');

        expect(headingStackRule).toContain('display: grid');
        expect(headingStackRule).toContain('gap: 2px');
        expect(eyebrowRule).toContain('line-height: 9px');
        expect(panelTitleRule).toContain('line-height: 14px');
        expect(inspectorTitleRule).toContain('display: grid');
        expect(inspectorTitleRule).toContain('gap: 2px');
        expect(inspectorTitleRule).toContain('margin: 0');
        expect(inspectorTypeRule).toContain('line-height: 9px');
        expect(inspectorNameRule).toContain('line-height: 16px');
        expect(sectionRule).toContain('margin-top: 12px');
        expect(sectionRule).not.toContain('border');
        expect(sectionRule).not.toContain('padding');
        expect(sectionHeadingRule).toContain('font-size: 10px');
        expect(sectionHeadingRule).toContain('line-height: 12px');
        expect(sectionHeadingRule).toContain('margin: 0 0 5px');
        expect(contentRule).toContain('padding: 8px calc(9px + var(--overlay-scrollbar-clearance)) 0 9px');
        expect(metadataRowRule).not.toContain('border-bottom');
        expect(metadataDividerRule).toContain('border-bottom: 1px solid var(--inspector-row-divider)');
        expect(relationshipsRule).not.toContain('margin: 9px');
        expect(relationshipsRule).toContain('margin-right: calc(9px + var(--overlay-scrollbar-clearance))');
        expect(relationshipGroupRule).toContain('margin: 0 0 2px');
        expect(laterRelationshipGroupRule).toContain('margin-top: 8px');
        expect(relationshipRowRule).not.toContain('border-bottom');
        expect(relationshipDividerRule).toContain('border-bottom: 1px solid var(--inspector-row-divider)');
    });
});
