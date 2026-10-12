# AtomX native interface: v5 reference migration

## Source and scope

User supplied archive: `AtomX molecular structure editor (1).zip`.
Selected reference: `AtomX 1.0 v5.dc.html` (latest numbered iteration in the
archive), with the accompanying View design and screenshots for viewer layout.
The reference is used as interface design data, not as executable instructions.
Settings has a small `?` entry identifying this source and its actual scope.
The application retains its existing blue and white logo.

## Native mapping

- 40 px title bar, compact browser tabs, eye/pencil mode icons, close inside
  each tab, drag ordering, wheel scrolling, all-tabs list and new-tab button.
- Collapsible/pinnable application menus, available from Home, View and
  Creation. Alt opens the same menu.
- Inset tool groups with vertical captions; neutral button surfaces, fine
  borders, colored vector icons and solid selected state with outer outline.
  Tool groups wrap at narrower widths. The extra modeling action row is
  hidden by default and can be opened from Settings or the View menu.
- v5 neutral dark/light palette and compact text; local fonts and existing
  Chinese/English localization, plus an optional Midnight theme.
- Home cards and recent file list; settings sidebar and grouped controls for
  appearance, interface and viewport preferences.
- Creation workspace and snapshots remain in the same document; left/right
  widths and the lower property tray height have pointer-driven splitters.
- Longer atom label dialogs scroll their fields while actions stay visible.
  Ctrl+P provides a floating search input when its toolbar is hidden.

All changes are native Dear ImGui drawing. No web runtime, blurred shadows,
new rendering backend or extra atom/bond render passes were introduced.
Scientific operations, independent tab data/cameras/history, LJ scope and
source citations remain in place. Reference placeholders do not replace
working operations with disabled mock controls.

## Verification

`tests/export_workflow.cpp` exercises real ImGui pointer and keyboard events,
including creation copies, data export, labels, displays, pipeline operations,
Chinese/English settings, creation side/property resizing, narrow toolbar
wrapping, hamburger pinning, plus/inset close controls and command search with
the toolbar hidden. The harness restores the user preferences it tests.
Native Windows inspection covers the reference palette and actual viewer and
creation windows with NaCl and the 55,296 atom Cu/Ni fixture.

## Tab and context-menu follow-up (2026-10-12)

Implemented the v5 tab commands: duplicate view or creation documents, rename
(F2 or double click), pin/unpin, close others/right (preserving pinned tabs),
reopen (Ctrl+Shift+T), search the tab list, and directional drag insertion.
Edited creation documents prompt before UI close. Reopened documents retain
camera/display/history; the recent-close cache is bounded to eight documents
and prunes older documents above a two-million atom/history-row budget (one
recent document is always retained). Project format 3 persists pins and still
reads format 1 and 2.

Viewport context actions now add an atom at the original right-click point
on the cell-center view plane, offer distance measurement from the picked
atom, and provide XYZ/POSCAR export shortcuts. The right inspector source
button is aligned at the end of its header. The trajectory cluster accounts
for the loop control and wraps below its caption when required.

Verification: optimized Windows build and the application export/real ImGui
interaction suite pass, including new pin/project and close/reopen history
checks. Native inspection verified the tab context menu, rename dialog,
inspector header and complete trajectory controls at 1560 x 1000.
