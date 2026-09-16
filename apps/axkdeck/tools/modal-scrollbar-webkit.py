"""Run the shared modal regression in Linux WebKitGTK (system Python + GI)."""

import argparse
import json
import os
import struct
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.1")
gi.require_foreign("cairo")
from gi.repository import GLib, Gtk, WebKit2  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://127.0.0.1:5191")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=1366)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--folders", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    # Match axkdeck's default Linux renderer configuration.
    os.environ.setdefault("WEBKIT_DISABLE_DMABUF_RENDERER", "1")
    window = Gtk.Window()
    window.set_default_size(args.width, args.height)
    view = WebKit2.WebView()
    window.add(view)
    result = {"status": "pending"}
    version = ".".join(
        str(getattr(WebKit2, f"get_{part}_version")())
        for part in ("major", "minor", "micro")
    )

    def finish(error=None):
        if error:
            result.update(status="failed", error=str(error))
        result["webkitGtkVersion"] = version
        (args.output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        Gtk.main_quit()
        return False

    def checked(widget, task, _):
        try:
            value = widget.evaluate_javascript_finish(task)
            state = json.loads(value.to_string())
            if state["status"] == "pending":
                GLib.timeout_add(100, poll)
            else:
                result.update(state)
                GLib.timeout_add(200, capture)
        except Exception as error:
            finish(error)

    def snapshot_done(widget, task, _):
        try:
            surface = widget.get_snapshot_finish(task)
            surface.write_to_png(str(args.output / "webview.png"))
            if result["status"] == "passed":
                measurement = result["pixelMeasurement"]
                surface.flush()
                data = surface.get_data()

                def rgb(x, y):
                    value = struct.unpack_from(
                        "=I", data, y * surface.get_stride() + x * 4
                    )[0]
                    return [(value >> shift) & 255 for shift in (16, 8, 0)]

                for y in range(measurement["y"], measurement["y"] + 8):
                    if rgb(measurement["referenceX"], y) != measurement["color"]:
                        raise AssertionError(
                            "Dialog is blank or pixel reference is not its opaque background"
                        )
                    for x in range(measurement["left"], measurement["right"]):
                        if rgb(x, y) != measurement["color"]:
                            raise AssertionError(
                                f"Background scrollbar bleeds through dialog at {x}, {y}"
                            )
                result["pixelCheck"] = "passed"
            finish()
        except Exception as error:
            finish(error)

    def capture():
        view.get_snapshot(
            WebKit2.SnapshotRegion.VISIBLE,
            WebKit2.SnapshotOptions.NONE,
            None,
            snapshot_done,
            None,
        )
        return False

    def poll():
        view.evaluate_javascript(
            "JSON.stringify(window.modalScrollbarResult || {status:'pending'})",
            -1,
            None,
            None,
            None,
            checked,
            None,
        )
        return False

    def loaded(widget, event):
        if event != WebKit2.LoadEvent.FINISHED:
            return
        script = """
        import('/tools/modal-scrollbar-checks.mjs')
            .then(({checkModalScrollbars}) => checkModalScrollbars({folders: %s}))
            .then(result => { window.modalScrollbarResult = {status: 'passed', ...result}; })
            .catch(error => { window.modalScrollbarResult = {status: 'failed', error: String(error), stack: error.stack}; });
        """ % json.dumps(args.folders)
        widget.evaluate_javascript(script, -1, None, None, None, None, None)
        GLib.timeout_add(100, poll)

    view.connect("load-changed", loaded)
    view.load_uri(
        args.base
        + "/tools/layout-fixtures/modal-scrollbars.html"
        + ("?folders" if args.folders else "")
    )
    window.show_all()
    GLib.timeout_add_seconds(30, lambda: finish("WebKitGTK test timed out"))
    Gtk.main()
    window.destroy()
    raise SystemExit(0 if result["status"] == "passed" else 1)


if __name__ == "__main__":
    main()
