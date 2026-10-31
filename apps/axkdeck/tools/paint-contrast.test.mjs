import assert from 'node:assert/strict';
import test from 'node:test';
import { contrastRatio, minimumGradientContrast } from './paint-contrast.js';

test('contrast includes black/white and symmetric pairings', () => {
    assert.equal(contrastRatio([0, 0, 0], [255, 255, 255]), 21);
    assert.equal(contrastRatio([255, 255, 255], [255, 255, 255]), 1);
    assert.equal(contrastRatio([12, 45, 78], [200, 150, 100]), contrastRatio([200, 150, 100], [12, 45, 78]));
});

test('white labels pass across approved default and hover gradients', () => {
    assert.ok(minimumGradientContrast([255, 255, 255], [[179, 84, 19], [152, 67, 15]]) >= 5);
    assert.ok(minimumGradientContrast([255, 255, 255], [[184, 92, 26], [163, 78, 22]]) >= 4.5);
});

test('gradient checks do not substitute an accessible ancestor for a failing bright stop', () => {
    assert.ok(contrastRatio([255, 255, 255], [17, 19, 21]) > 4.5);
    assert.ok(minimumGradientContrast([255, 255, 255], [[204, 120, 50], [157, 91, 39]]) < 4.5);
    assert.ok(minimumGradientContrast([255, 255, 255], [[0, 0, 0], [255, 255, 255], [0, 0, 0]]) < 4.5);
});

test('missing gradient stops fail instead of returning a passing infinity', () => {
    assert.throws(() => minimumGradientContrast([255, 255, 255], []), RangeError);
    assert.throws(() => minimumGradientContrast([255, 255, 255], [[0, 0, 0]]), RangeError);
});
