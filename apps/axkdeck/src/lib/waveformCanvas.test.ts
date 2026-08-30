import { describe, expect, it } from 'vitest';
import { canvasPixelSize, waveformPixelColumns } from './waveformCanvas';

describe('canvasPixelSize', () => {
    it.each([
        [1, 100, 28],
        [1.25, 125, 35],
        [1.5, 150, 42],
        [2, 200, 56],
    ])('sizes a 100 by 28 CSS pixel canvas at DPR %s', (devicePixelRatio, width, height) => {
        expect(canvasPixelSize(100, 28, devicePixelRatio)).toEqual({ width, height });
    });
});

describe('waveformPixelColumns', () => {
    it.each([
        [1, 4],
        [1.25, 5],
        [1.5, 6],
        [2, 8],
    ])('covers every content pixel exactly once at DPR %s', (_devicePixelRatio, pixelWidth) => {
        const columns = waveformPixelColumns([{ minimum: -1, maximum: 1 }], 1, 4, pixelWidth, 10);
        const lastColumn = columns.at(-1);

        expect(columns[0]?.x).toBe(0);
        expect(lastColumn).toBeDefined();
        expect(lastColumn!.x + lastColumn!.width).toBe(pixelWidth);
        for (let index = 1; index < columns.length; index += 1) {
            expect(columns[index]!.x).toBe(columns[index - 1]!.x + columns[index - 1]!.width);
        }
        expect(columns.every((column) => Number.isInteger(column.x) && Number.isInteger(column.width))).toBe(true);
        expect(columns.every((column) => column.width >= 1)).toBe(true);
    });

    it('limits columns to the waveform content ratio', () => {
        const columns = waveformPixelColumns([{ minimum: -1, maximum: 1 }], 0.5, 4, 6, 10);
        const lastColumn = columns.at(-1);

        expect(lastColumn).toBeDefined();
        expect(lastColumn!.x + lastColumn!.width).toBe(3);
    });
});
