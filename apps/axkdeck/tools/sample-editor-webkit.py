"""Bounded native WebKitGTK zoom regression; requires system PyGObject/WebKit 4.1."""
import argparse
import json
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.1")
from gi.repository import GLib, Gtk, WebKit2  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("base")
parser.add_argument("output", type=Path)
parser.add_argument("--zoom", type=float, action="append")
parser.add_argument("--width", type=int, default=1600)
parser.add_argument("--height", type=int, default=900)
parser.add_argument("--workspace", action="store_true")
parser.add_argument("--conversion-lock", action="store_true")
parser.add_argument("--checks", type=Path, default=Path(__file__).with_name("sample-editor-regression-checks.js"))
parser.add_argument("--fixture", default="/tools/layout-fixtures/sample-editor.html")
parser.add_argument("--checks-function", default="runSampleEditorRegression")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
script = args.checks.read_text()
Gtk.init([])
results = []


def run(zoom, stereo):
    name = f"webkit-{args.width}-{args.height}-{zoom}-{'stereo' if stereo else 'mono'}{'-workspace' if args.workspace else ''}"
    win = Gtk.OffscreenWindow()
    win.set_default_size(args.width, args.height)
    view = WebKit2.WebView()
    view.get_settings().set_media_playback_requires_user_gesture(False)
    view.get_settings().set_hardware_acceleration_policy(WebKit2.HardwareAccelerationPolicy.NEVER)
    view.set_zoom_level(zoom)
    win.add(view)
    win.show_all()
    outcome = {"name": name, "zoom": zoom, "webkit": f"{WebKit2.get_major_version()}.{WebKit2.get_minor_version()}.{WebKit2.get_micro_version()}", "failures": ["Timed out"]}
    finished = False
    started = False

    def evaluated(obj, result, _data):
        nonlocal outcome, finished
        try:
            value = json.loads(obj.evaluate_javascript_finish(result).to_string())
            if value is None:
                GLib.timeout_add(100, poll)
                return
            outcome.update(value)
        except Exception as error:
            outcome["failures"] = [str(error)]
        finished = True
        try:
            win.get_pixbuf().savev(str(args.output / f"{name}.png"), "png", [], [])
        except Exception as error:
            outcome["failures"].append(f"Snapshot: {error}")
        Gtk.main_quit()

    def poll():
        if not finished:
            view.evaluate_javascript("JSON.stringify(window.__editorResult ?? null)", -1, None, None, None, evaluated, None)
        return False

    def start():
        nonlocal started
        if started or finished:
            return False
        started = True
        view.evaluate_javascript(script + f"\nwindow[{json.dumps(args.checks_function)}]().then(r=>window.__editorResult=r).catch(e=>window.__editorResult={{failures:[e.stack]}}); void 0;", -1, None, None, None, None, None)
        GLib.timeout_add(100, poll)
        return False

    def loaded(_obj, event):
        if event == WebKit2.LoadEvent.FINISHED and args.fixture in (view.get_uri() or ""):
            GLib.timeout_add(1000, start)

    view.connect("load-changed", loaded)
    query = "?" + "&".join(name for enabled, name in [(stereo, "short-stereo"), (args.workspace, "workspace"), (args.conversion_lock, "conversion-lock")] if enabled)
    view.load_uri(args.base + args.fixture + query)
    timeout = GLib.timeout_add_seconds(45, lambda: (Gtk.main_quit(), False)[1])
    Gtk.main()
    GLib.source_remove(timeout)
    finished = True
    win.destroy()
    print(json.dumps(outcome), flush=True)
    results.append(outcome)


for zoom in args.zoom or [1, 1.25, 1.5, 2]:
    for stereo in [False, True]:
        run(zoom, stereo)
(args.output / "webkit-results.json").write_text(json.dumps(results, indent=2) + "\n")
raise SystemExit(int(any(result["failures"] for result in results)))
