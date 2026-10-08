export function graphDrag(
    start: PointerEvent,
    origin: { x: number; y: number },
    bounds: { width: number; height: number; unboundedX?: boolean; unboundedY?: boolean },
    change: (x: number, y: number) => void,
    end: (cancelled: boolean) => void,
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
        const y = origin.y - (lastY - start.clientY) / height;
        change(bounds.unboundedX ? x : clamp(x), bounds.unboundedY ? y : clamp(y));
    };
    const move = (event: PointerEvent) => {
        if (event.pointerId !== start.pointerId || (event.clientX === lastX && event.clientY === lastY)) return;
        lastX = event.clientX;
        lastY = event.clientY;
        pending = true;
        if (frame === undefined) frame = requestAnimationFrame(flush);
    };
    const finish = (cancelled = true) => {
        if (finished) return;
        finished = true;
        if (frame !== undefined) cancelAnimationFrame(frame);
        frame = undefined;
        target.removeEventListener('pointermove', move);
        target.removeEventListener('pointerup', release);
        target.removeEventListener('pointercancel', cancel);
        target.removeEventListener('lostpointercapture', cancel);
        if (target.hasPointerCapture(start.pointerId)) target.releasePointerCapture(start.pointerId);
        window.removeEventListener('keydown', escape);
        end(cancelled);
    };
    const release = (event: PointerEvent) => {
        if (event.pointerId !== start.pointerId) return;
        move(event);
        flush();
        finish(false);
    };
    const cancel = (event: PointerEvent) => {
        if (event.pointerId === start.pointerId) {
            finish();
        }
    };
    const escape = (event: KeyboardEvent) => {
        if (event.key === 'Escape') {
            event.preventDefault();
            finish();
        }
    };
    target.setPointerCapture(start.pointerId);
    target.addEventListener('pointermove', move);
    target.addEventListener('pointerup', release);
    target.addEventListener('pointercancel', cancel);
    target.addEventListener('lostpointercapture', cancel);
    window.addEventListener('keydown', escape);
    return finish;
}
