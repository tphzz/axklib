/// <reference types="node" />

import { readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

function source(path: string): string {
    return readFileSync(resolve(process.cwd(), path), 'utf8');
}

const appStyles = source('src/app.css');
const checkboxDialogs = [
    'src/lib/components/MidiImportDialog.svelte',
    'src/lib/components/ObjectDeletionDialog.svelte',
    'src/lib/components/PackageBatchItemsTable.svelte',
    'src/lib/components/ProgramGenerationDialog.svelte',
    'src/lib/components/WaveDataCleanupDialog.svelte',
    'src/lib/components/WorkspaceManager.svelte',
];
const segmentedControls = [
    'src/lib/components/CreateHardDiskImageDialog.svelte',
    'src/lib/components/ImportDestinationChooser.svelte',
    'src/lib/components/PackageBatchDestinationChooser.svelte',
    'src/lib/components/SfzExportDialog.svelte',
];
const componentDirectory = resolve(process.cwd(), 'src/lib/components');
const dialogComponents = readdirSync(componentDirectory)
    .filter((name) => name.endsWith('.svelte'))
    .map((name) => `src/lib/components/${name}`)
    .filter((path) => source(path).includes('role="dialog"'));

describe('dialog visual contract', () => {
    it('defines one compact typography scale for dialog content', () => {
        expect(appStyles).toMatch(/--dialog-title-font-size:\s*13px/);
        expect(appStyles).toMatch(/--dialog-body-font-size:\s*11px/);
        expect(appStyles).toMatch(/--dialog-section-font-size:\s*11px/);
        expect(appStyles).toMatch(/--dialog-table-header-font-size:\s*10px/);
        expect(appStyles).toMatch(/--dialog-metadata-font-size:\s*10px/);
        expect(appStyles).toMatch(/\.dialog-shell\s*\{[^}]*font-size:\s*var\(--dialog-body-font-size\)/s);

        const sharedDialogStyles = appStyles.slice(
            appStyles.indexOf('.dialog-shell {'),
            appStyles.indexOf('.connection-mode {'),
        );
        expect(sharedDialogStyles).not.toMatch(/font-size:\s*\d+px/);
        for (const path of dialogComponents) {
            expect(source(path), path).not.toMatch(/font-size:\s*\d+px/);
        }
    });

    it('uses the shared checkbox geometry in every dialog with checkbox selection', () => {
        expect(appStyles).toMatch(/\.dialog-checkbox\s*\{[^}]*width:\s*14px[^}]*height:\s*14px/s);
        expect(appStyles).toMatch(/\.dialog-checkbox::before\s*\{[^}]*width:\s*4px[^}]*height:\s*7px/s);
        expect(appStyles).not.toContain('.deletion-checkbox');
        for (const path of checkboxDialogs) {
            const component = source(path);
            expect(component, path).toContain('class="dialog-checkbox"');
            expect(component, path).not.toContain('class="deletion-checkbox"');
        }
    });

    it('uses the shared compact segmented-control treatment', () => {
        expect(appStyles).toMatch(
            /\.dialog-segmented-control\s*>\s*button\s*\{[^}]*min-height:\s*27px[^}]*font-size:\s*var\(--dialog-table-header-font-size\)/s,
        );
        expect(appStyles).toMatch(
            /\.dialog-segmented-control\s*>\s*button:focus-visible\s*\{[^}]*outline-offset:\s*-2px/s,
        );
        for (const path of segmentedControls) {
            expect(source(path), path).toContain('dialog-segmented-control');
        }
    });

    it('keeps dense dialog table headings and metadata on the compact scale', () => {
        const programs = source('src/lib/components/ProgramGenerationDialog.svelte');
        const tx16w = source('src/lib/components/Tx16wImportDialog.svelte');
        const companions = source('src/lib/components/CompanionDiskDialog.svelte');

        expect(programs).toMatch(
            /\.program-generation-header\s*\{[^}]*font-size:\s*var\(--dialog-table-header-font-size\)/s,
        );
        expect(tx16w).toMatch(/\.import-mode legend\s*\{[^}]*font-size:\s*var\(--dialog-label-font-size\)/s);
        expect(tx16w).toMatch(/\.mapping-notice\s*\{[^}]*font-size:\s*var\(--dialog-body-font-size\)/s);
        expect(companions).toMatch(
            /\.companion-disk-content h3\s*\{[^}]*font-size:\s*var\(--dialog-section-font-size\)/s,
        );
    });
});
