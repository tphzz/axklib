"""Bounded native WebKitGTK export-dialog paint, Escape and keyboard-scroll checks."""
import json
import sys
from ctypes import CDLL, c_char_p, c_int, c_ubyte, c_uint, c_ulong, c_void_p
from pathlib import Path

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("WebKit2", "4.1")
gi.require_version("GdkX11", "3.0")
from gi.repository import Gdk, GdkX11, GLib, Gtk, WebKit2  # noqa: E402

base, output_arg = sys.argv[1:]
output = Path(output_arg)
output.mkdir(parents=True, exist_ok=True)
checks = Path(__file__).with_name("volume-package-export-checks.js").read_text()
results = []
Gtk.init([])
x11, xtest = CDLL("libX11.so.6"), CDLL("libXtst.so.6")
x11.XOpenDisplay.argtypes, x11.XOpenDisplay.restype = [c_char_p], c_void_p
x11.XSetInputFocus.argtypes = [c_void_p, c_ulong, c_int, c_ulong]
x11.XKeysymToKeycode.argtypes, x11.XKeysymToKeycode.restype = [c_void_p, c_ulong], c_ubyte
x11.XSync.argtypes = [c_void_p, c_int]
x11.XCloseDisplay.argtypes = [c_void_p]
xtest.XTestFakeKeyEvent.argtypes = [c_void_p, c_uint, c_int, c_ulong]


def run(zoom):
    window = Gtk.Window(title="axkdeck export-dialog regression")
    window.set_default_size(1366, 900)
    view = WebKit2.WebView()
    view.get_settings().set_hardware_acceleration_policy(WebKit2.HardwareAccelerationPolicy.NEVER)
    view.set_zoom_level(zoom)
    window.add(view)
    window.show_all()
    window.present()
    view.grab_focus()
    result = {"zoom": zoom, "failures": ["Timed out"], "webkit": f"{WebKit2.get_major_version()}.{WebKit2.get_minor_version()}.{WebKit2.get_micro_version()}"}
    started = False
    stage = "checks"

    def native_key(keyval):
        native = window.get_window()
        if not isinstance(native, GdkX11.X11Window):
            raise RuntimeError("Native keyboard regression requires an X11 test display")
        display = x11.XOpenDisplay(None)
        if not display:
            raise RuntimeError("Native keyboard regression cannot open the test display")
        try:
            x11.XSetInputFocus(display, native.get_xid(), 1, 0)
            code = x11.XKeysymToKeycode(display, keyval)
            if not code or not xtest.XTestFakeKeyEvent(display, code, 1, 0):
                raise RuntimeError("Native keyboard event could not be delivered")
            xtest.XTestFakeKeyEvent(display, code, 0, 0)
            x11.XSync(display, 0)
        finally:
            x11.XCloseDisplay(display)

    def evaluate(script, callback):
        def done(obj, value, _data):
            try:
                callback(json.loads(obj.evaluate_javascript_finish(value).to_string()))
            except Exception as error:
                result["failures"].append(str(error))
                Gtk.main_quit()
        view.evaluate_javascript(script, -1, None, None, None, done, None)

    def snapshot(name):
        native = window.get_window()
        pixels = Gdk.pixbuf_get_from_window(native, 0, 0, native.get_width(), native.get_height())
        pixels.savev(str(output / f"{zoom}-{name}.png"), "png", [], [])

    def show(scenario, extended=False):
        script = f"window.__nativeExportResult=null; window.volumePackageExportFixture.show({json.dumps(scenario)}, {json.dumps(extended)}); setTimeout(()=>window.__nativeExportResult={{ready:true,opacity:Number(getComputedStyle([...document.querySelectorAll('.dialog-footer button')].find(n=>n.textContent.trim()==='Retry')).opacity)}},250); void 0;"
        view.evaluate_javascript(script, -1, None, None, None, None, None)
        GLib.timeout_add(100, poll)

    def received(value):
        nonlocal stage
        if value is None:
            GLib.timeout_add(100, poll)
            return
        if stage == "checks":
            result.update(value)
            snapshot("failed")
            stage = "inspecting"
            show("inspecting")
        elif stage == "inspecting":
            result["disabledOpacity"] = value["opacity"]
            if abs(value["opacity"] - 0.45) > 0.01:
                result["failures"].append("Retry disabled paint did not settle")
            snapshot("inspecting")
            native_key(Gdk.KEY_Escape)
            stage = "escape"
            GLib.timeout_add(150, poll)
        elif stage == "escape":
            if not value:
                result["failures"].append("Native Escape did not dismiss the inspecting dialog")
            stage = "long"
            show("failed", True)
        elif stage == "long":
            stage = "scroll"
            evaluate("(()=>{const n=document.querySelector('[role=alert]');n.scrollTop=0;n.focus();return JSON.stringify(document.activeElement===n)})()", send_scroll)
        else:
            result["keyboardScrollTop"] = value
            if value <= 0:
                result["failures"].append("Native PageDown did not scroll focused error details")
            snapshot("keyboard-scroll")
            Gtk.main_quit()

    def send_scroll(focused):
        if not focused:
            result["failures"].append("Error details did not receive native focus")
        native_key(Gdk.KEY_Page_Down)
        GLib.timeout_add(250, poll)

    def poll():
        expression = "window.__nativeExportResult ?? null"
        if stage == "escape":
            expression = "!document.querySelector('[role=dialog]')"
        elif stage == "scroll":
            expression = "document.querySelector('[role=alert]').scrollTop"
        evaluate(f"JSON.stringify({expression})", received)
        return False

    def loaded(_view, event):
        nonlocal started
        if event != WebKit2.LoadEvent.FINISHED or started:
            return
        started = True
        view.evaluate_javascript(checks + "\nwindow.runVolumePackageExportChecks().then(r=>window.__nativeExportResult=r).catch(e=>window.__nativeExportResult={failures:[e.stack]}); void 0;", -1, None, None, None, None, None)
        GLib.timeout_add(100, poll)

    view.connect("load-changed", loaded)
    view.load_uri(base + "/tools/layout-fixtures/volume-package-export.html")
    timeout = GLib.timeout_add_seconds(30, lambda: (Gtk.main_quit(), False)[1])
    Gtk.main()
    GLib.source_remove(timeout)
    window.destroy()
    print(json.dumps(result), flush=True)
    results.append(result)


for scale in [1, 1.25, 1.5]:
    run(scale)
(output / "webkit-results.json").write_text(json.dumps(results, indent=2) + "\n")
raise SystemExit(int(any(result["failures"] for result in results)))
