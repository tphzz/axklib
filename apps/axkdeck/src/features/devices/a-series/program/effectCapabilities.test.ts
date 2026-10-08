import { describe, expect, it } from 'vitest';
import { programEditorFixture } from '../../../../test/programEditorFixture';
import { effectParameterExtended } from './effectCapabilities';
describe('effect feature markers', () => {
    it('compares algorithms by raw ID and word index rather than Program generation or printed number', () => {
        const current = programEditorFixture().format,
            native = programEditorFixture(true).format;
        current.effects[0]!.printedNumber = 56;
        current.effects[0]!.parameters.reverse();
        expect(effectParameterExtended(current, native, 1, 0)).toBe(false);
        current.effects[0]!.parameters.find((parameter) => parameter.index === 0)!.max++;
        expect(effectParameterExtended(current, native, 1, 0)).toBe(true);
        expect(effectParameterExtended(current, native, 2, 0)).toBe(false);
    });
    it('marks later algorithms and additional visible parameters but not unused words', () => {
        const current = programEditorFixture().format,
            native = programEditorFixture(true).format;
        native.effects.splice(0, 1);
        expect(effectParameterExtended(current, native, 1, 0)).toBe(true);
        expect(effectParameterExtended(current, native, 1, 15)).toBe(false);
        expect(effectParameterExtended(native, native, 2, 0)).toBe(false);
    });
});
