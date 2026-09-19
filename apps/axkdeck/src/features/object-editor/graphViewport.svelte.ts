export class GraphViewport {
    span = $state(1);
    constructor(
        initial: number,
        readonly minimum: number,
        readonly maximum: number,
    ) {
        this.fit(initial);
    }
    fit(extent: number) {
        this.span = Math.max(this.minimum, Math.min(this.maximum, extent));
    }
    zoom(factor: number) {
        this.fit(this.span * factor);
    }
}
