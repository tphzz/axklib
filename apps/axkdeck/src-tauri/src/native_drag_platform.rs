use std::path::PathBuf;
use std::sync::Arc;

use tauri::WebviewWindow;

pub(crate) fn file_uris(paths: &[PathBuf]) -> Result<Vec<String>, String> {
    paths
        .iter()
        .map(|path| {
            url::Url::from_file_path(path)
                .map(|url| url.to_string())
                .map_err(|_| "Native drag path must be absolute".to_owned())
        })
        .collect()
}

#[cfg(target_os = "linux")]
pub(crate) fn start(
    window: &WebviewWindow,
    paths: Vec<PathBuf>,
    finish: Arc<dyn Fn(bool) + Send + Sync>,
) -> Result<(), String> {
    use gtk::{gdk, glib, prelude::*};
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;
    let uris = file_uris(&paths)?;
    let window = window.gtk_window().map_err(|error| error.to_string())?;
    let source = window
        .focused_widget()
        .ok_or_else(|| "Native drag source is unavailable".to_owned())?;
    let pointer = window
        .display()
        .default_seat()
        .and_then(|seat| seat.pointer())
        .ok_or_else(|| "Native pointer is unavailable".to_owned())?;
    let surface = window
        .window()
        .ok_or_else(|| "Native window is unavailable".to_owned())?;
    if !surface
        .device_position(&pointer)
        .3
        .contains(gdk::ModifierType::BUTTON1_MASK)
    {
        return Err("File drag was released before preparation completed".to_owned());
    }
    let handlers = Rc::new(RefCell::new(Vec::<glib::SignalHandlerId>::new()));
    let failed = Rc::new(Cell::new(false));
    window.drag_source_set(gdk::ModifierType::BUTTON1_MASK, &[], gdk::DragAction::COPY);
    window.drag_source_add_uri_targets();
    handlers
        .borrow_mut()
        .push(window.connect_drag_data_get(move |_, _, data, _, _| {
            let uris: Vec<_> = uris.iter().map(String::as_str).collect();
            data.set_uris(&uris);
        }));
    let cancelled = failed.clone();
    handlers
        .borrow_mut()
        .push(window.connect_drag_failed(move |_, _, _| {
            cancelled.set(true);
            glib::Propagation::Stop
        }));
    let ended_handlers = handlers.clone();
    handlers
        .borrow_mut()
        .push(window.connect_drag_end(move |window, context| {
            let dropped = !failed.get() && context.selected_action() == gdk::DragAction::COPY;
            for handler in ended_handlers.borrow_mut().drain(..) {
                window.disconnect(handler);
            }
            window.drag_source_unset();
            finish_pointer_sequence(&source, context);
            finish(dropped);
        }));
    let started = window.drag_source_get_target_list().and_then(|targets| {
        window.drag_begin_with_coordinates(&targets, gdk::DragAction::COPY, 1, None, -1, -1)
    });
    if started.is_none() {
        for handler in handlers.borrow_mut().drain(..) {
            window.disconnect(handler);
        }
        window.drag_source_unset();
        return Err("Native file drag could not start".to_owned());
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn finish_pointer_sequence(source: &gtk::Widget, context: &gtk::gdk::DragContext) {
    use gtk::{gdk, glib::translate::*, prelude::*};
    let Some(surface) = source.window() else {
        return;
    };
    let device = context.device();
    let (_, x, y, state) = surface.device_position(&device);
    let (_, x_root, y_root) = device.position();
    let mut event = gdk::Event::new(gdk::EventType::ButtonRelease);
    event.set_device(Some(&device));
    let button = event.downcast_mut::<gdk::EventButton>().unwrap();
    let fields = button.as_mut();
    fields.window = surface.to_glib_full();
    fields.send_event = 1;
    fields.time = gdk::ffi::GDK_CURRENT_TIME as u32;
    fields.x = f64::from(x);
    fields.y = f64::from(y);
    fields.x_root = f64::from(x_root);
    fields.y_root = f64::from(y_root);
    fields.state = state.into_glib();
    fields.button = 1;
    // The native drag owns the real release; WebKit must also end its pointer sequence.
    source.event(&event);
}

#[cfg(not(target_os = "linux"))]
pub(crate) fn start(
    window: &WebviewWindow,
    paths: Vec<PathBuf>,
    finish: Arc<dyn Fn(bool) + Send + Sync>,
) -> Result<(), String> {
    file_uris(&paths)?;
    drag::start_drag(
        window,
        drag::DragItem::Files(paths),
        drag::Image::Raw(include_bytes!("../icons/32x32.png").to_vec()),
        move |result, _| finish(matches!(result, drag::DragResult::Dropped)),
        drag::Options {
            mode: drag::DragMode::Copy,
            ..Default::default()
        },
    )
    .map_err(|error| error.to_string())
}

#[cfg(test)]
mod tests {
    use super::file_uris;
    #[test]
    fn urls_escape_spaces_hash_and_percent_and_round_trip() {
        let path = std::env::temp_dir().join("Folder #1 %").join("RAW.bin");
        let values = file_uris(std::slice::from_ref(&path)).unwrap();
        assert!(values[0].contains("Folder%20%231%20%25"));
        assert_eq!(
            url::Url::parse(&values[0]).unwrap().to_file_path().unwrap(),
            path
        );
        assert!(file_uris(&["relative".into()]).is_err());
    }
}
