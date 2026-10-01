import { describe, expect, it } from 'vitest';
import { editorPaneHeight } from './paneLayout';

describe('editor pane sizing', () => {
    it('reserves a usable editor height, growing to one third on tall workspaces', () => {
        expect(editorPaneHeight(700, null, 360)).toBe(360);
        expect(editorPaneHeight(1200, null, 360)).toBe(400);
        expect(editorPaneHeight(500, null, 360)).toBe(320);
    });
    it('honors manual splits and constrains both panes in short windows', () => {
        expect(editorPaneHeight(700, 0.7, 360)).toBeCloseTo(210);
        expect(editorPaneHeight(700, 0.95, 360)).toBe(180);
        expect(editorPaneHeight(200, null, 360)).toBe(100);
        expect(editorPaneHeight(0, null, 360)).toBe(0);
    });
    it('keeps existing non-editor thirds', () => {
        expect(editorPaneHeight(900, null)).toBe(300);
    });
});
