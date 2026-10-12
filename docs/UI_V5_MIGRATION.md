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
