// ui — msga's widget toolkit, on plat (windows/input), gfx (painting)
// and text (layout). Include this one header.
//
// ── The model ───────────────────────────────────────────────────────────────
// A retained tree of plain C++ objects. Every node is a ui::View (view.h):
// it owns its children (std::unique_ptr), has a frame relative to its parent
// in logical px, and virtual hooks — measureContent / layout / paint /
// paintOver / onEvent / hitTest / cursorAt / styleChanged / tick. There is no
// signal/slot machinery: behaviour hangs off a few std::function members
// (onClick, onChange, onSubmit, onLink …) that cost nothing when unset.
// Single-threaded: touch views only on the thread that runs the App.
//
//   auto app = ui::App::create();                  // plat + text::init
//   ui::Window win({.title = "msga", .size = {1200, 800}});
//   auto *screen = win.root().add<ui::View>();     // fills the window
//   screen->style().row();
//   auto *side = screen->add<ui::View>();
//   side->style().width(240).padding(8).spacing(2);
//   side->setBackground(ui::C::Sidebar);
//   auto *b = side->add<ui::Button>("Add channels", ui::Button::Kind::Ghost);
//   b->onClick = [] { … };
//   app->run();
//
// ── Layout (view.h: Style) ──────────────────────────────────────────────────
// Flexbox-lite per view: dir Row/Column/Stack/None, pad, margin, gap, fixed
// w/h, min/max, grow/shrink, align/self, justify. grow > 0 children start at
// 0 and share the free space (flex: N 1 0); others start at their measured
// size and shrink by `shrink` when the line overflows. Leaves report their
// content size from measureContent(availW, availH) (text wraps at availW).
// Measurements are cached per view until invalidateLayout(), which dirties
// the view and its ancestors up to the nearest *layout boundary* (fixed-size
// views, ScrollArea/VirtualList, the root): only that subtree is re-laid out
// in the next frame. Style& style() marks the layout dirty by itself.
//
// ── Painting ────────────────────────────────────────────────────────────────
// update() adds the view's window rect to the window's damage; the window
// asks plat for a Frame, then (once per frame) runs tick()s, the layout pass,
// and paints only the damaged rects into the persistent canvas: every view
// intersecting a rect paints (paint → children → paintOver) under a clip,
// others are skipped. Scrolling blits the canvas and repaints the exposed
// strip. Window::stats() reports damage rects and timings.
//
// ── Input ───────────────────────────────────────────────────────────────────
// Pointer events go to the deepest view under the pointer and bubble to the
// parent until a view returns true; the view that handles PointerDown
// captures the pointer until PointerUp (motion outside the window included).
// The hover chain (leaf to root) gets PointerEnter/Leave and View::hovered();
// setHoverRepaint(true) repaints on change. Cursors: setCursor() or
// cursorAt(). Tooltips: View::tooltip() / Clickable::setTooltip().
// Keyboard: focused view first (bubbling), then Window shortcuts
// (addShortcut, kPrimary = Cmd/Ctrl), then Tab/Shift+Tab focus traversal in
// tree order. Right click / long press / Menu key / Shift+F10 →
// EventType::ContextMenu (Menu::popupAt opens one at the point).
// DnD targets: handle DropEnter/DropMove (set e.dropAction, return true) and
// Drop. IME: TextEdit drives plat's setTextInput and preedit itself.
//
// ── Theme (theme.h) ─────────────────────────────────────────────────────────
// Views hold token ids (C::Sidebar, Font::Body, M::RadiusM), never colours;
// App::setThemeMode()/the OS dark-mode switch restyles every window live
// (styleChanged() on each view: cached text layouts are rebuilt, everything
// repaints). Rich text uses themed(C::Link) sentinel colours for the same
// reason. App::reducedMotion() turns glides/flings into jumps.
//
// ── Widgets ─────────────────────────────────────────────────────────────────
//   widgets.h   Label, Clickable, Button/IconButton, Badge, Separator, Image,
//               Popup (in-window overlay card), Menu (keyboard navigable)
//   scroll.h    ScrollArea (wheel/touchpad/kinetic/scrollbar), ScrollView,
//               VirtualList (lazy variable-height rows, anchored scrolling,
//               stick-to-bottom, smooth jump-to-item)
//   controls.h  CheckBox, Radio, Dropdown, SpinBox, TextField, Button::Form,
//               SectionList, Dialog (modal overlay)
//   textedit.h  TextEdit (rich composer, undo, clipboard html, IME, grows to N lines)
//   filebrowser.h  FileBrowser (in-app file chooser where the OS has none; Linux, BSDs, ports)
//
// ── Accessibility ───────────────────────────────────────────────────────────
// Not bridged yet. Every view carries role() and accessibleName() so an
// AT-SPI/UIA/NSAccessibility bridge can walk the tree later.
#pragma once

#include "ui/controls.h"
#if defined(MSGA_FREEDESKTOP) || defined(MSGA_PORT)
#include "ui/filebrowser.h"
#endif
#include "ui/scroll.h"
#include "ui/textedit.h"
#include "ui/theme.h"
#include "ui/view.h"
#include "ui/widgets.h"
