import { describe, expect, it } from 'vitest';
import { connectionEdges, programRoutes } from './routing';
describe('Program effect routing', () => {
    it('implements exactly the five hardware topologies in both triples', () => {
        const expected = [
            [],
            [[1, 2]],
            [
                [1, 2],
                [2, 3],
            ],
            [[2, 3]],
            [
                [1, 2],
                [3, 2],
            ],
        ];
        expected.forEach((edges, choice) => {
            expect(connectionEdges(choice, 1)).toEqual(edges);
            expect(connectionEdges(choice, 4)).toEqual(edges.map(([a, b]) => [a! + 3, b! + 3]));
        });
        expect(connectionEdges(99, 1)).toEqual([]);
    });
    it('uses effect destination 6..8 for cross-triple routing and ignores dormant output destinations', () => {
        const result = programRoutes(
            {
                'effect_connections.1': 1,
                'effect_connections.2': 0,
                'effects.1.destination': 8,
                'effects.2.destination': 6,
                'effects.3.destination': 5,
            },
            6,
        );
        expect(result.find((edge) => edge.from === 1)).toEqual({ from: 1, to: 2, kind: 'connection' });
        expect(result.find((edge) => edge.from === 2)).toEqual({ from: 2, to: 4, kind: 'destination' });
        expect(result.find((edge) => edge.from === 3)).toEqual({ from: 3, to: 'DIG & OPT', kind: 'destination' });
        expect(programRoutes({ 'effects.1.destination': 6 }, 3).find((edge) => edge.from === 1)).toBeUndefined();
    });
});
