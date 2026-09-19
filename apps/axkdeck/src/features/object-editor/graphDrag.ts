export function graphDrag(
    start: PointerEvent,
    origin: { x: number; y: number },
    bounds: { width: number; height: number; unboundedX?: boolean },
    change: (x: number, y: number) => void,
    end: () => void,
): () => void {
    const target = start.currentTarget as HTMLElement;
    const fine = start.shiftKey ? 4 : 1;
    const width = Math.max(1, bounds.width) * fine,
        height = Math.max(1, bounds.height) * fine;
    const clamp = (value: number) => Math.max(0, Math.min(1, value));
    let frame: number | undefined;
    let pending = false;
    let finished = false,
        lastX = start.clientX,
        lastY = start.clientY;
    const flush = () => {
        if (frame !== undefined) cancelAnimationFrame(frame);
        frame = undefined;
        if (!pending || finished) return;
        pending = false;
        const x = origin.x + (lastX - start.clientX) / width;
        change(bounds.unboundedX ? x : clamp(x), clamp(origin.y - (lastY - start.clientY) / height));
    };
    const move = (event: PointerEvent) => {
        if (event.pointerId !== start.pointerId || (event.clientX === lastX && event.clientY === lastY)) return;
        lastX = event.clientX;
        lastY = event.clientY;
        pending = true;
        if (frame === undefined) frame = requestAnimationFrame(flush);
    };
    const finish = () => {
        if (finished) return;
        finished = true;
        if (frame !== undefined) cancelAnimationFrame(frame);
        frame = undefined;
        target.removeEventListener('pointermove', move);
        target.removeEventListener('pointerup', release);
        target.removeEventListener('pointercancel', cancel);
        target.removeEventListener('lostpointercapture', cancel);
        if (target.hasPointerCapture(start.pointerId)) target.releasePointerCapture(start.pointerId);
        end();
    };
    const release = (event: PointerEvent) => {
        if (event.pointerId !== start.pointerId) return;
        move(event);
        flush();
        finish();
    };
    const cancel = (event: PointerEvent) => {
        if (event.pointerId === start.pointerId) {
            flush();
            finish();
        }
    };
    target.setPointerCapture(start.pointerId);
    target.addEventListener('pointermove', move);
    target.addEventListener('pointerup', release);
    target.addEventListener('pointercancel', cancel);
    target.addEventListener('lostpointercapture', cancel);
    return finish;
}
