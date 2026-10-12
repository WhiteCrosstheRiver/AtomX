# Global settings and languages

The title bar gear, beside the window controls, opens Settings from Home,
Viewer and Creation tabs. Edit > Application Settings and the existing toolbar
entry open the same dialog.

- Languages: Simplified Chinese (`zh-CN`, default) and English (`en`).
- Language, theme, font and size apply immediately. No restart or network is needed.
- Preferences are saved in `%LOCALAPPDATA%/AtomX/settings.ini`; missing or
  unknown language codes fall back to Simplified Chinese.
- Settings has Appearance, Interface and 3D viewport pages, including window behavior help.
  Reset layout restores panel widths/heights; the panels remain draggable.
- Native Windows file dialogs follow the Windows display language.

## Adding interface text

`src/localization_strings.inc` holds source spelling, Chinese and English.
Use `localization::text` for captions/tooltips and `localization::label` for
ImGui controls/window titles. Labels keep a language-independent `###` ID.
`localization::items` preserves NUL-separated combo lists. Keep printf argument
types/order identical, including `%%` escapes. Both language glyph sets are
built into the existing font atlas, so switching does not rebuild GPU resources.

Only UI strings are localized. Do not translate filenames, user-defined labels,
element symbols, property identifiers, expressions, citations or serialized data.
Keep long text wrapped; use short captions in narrow segmented controls.

The existing `export_workflow` regression covers actual gear and language clicks,
live modal stability, saved preferences, unknown-code fallback, all three tab
modes, preserved tabs/history and language-independent widget IDs.

## v5 interface reference

The October 2026 user archive `AtomX molecular structure editor (1).zip`
contains several iterations. `AtomX 1.0 v5.dc.html` is the selected reference.
See [UI_V5_MIGRATION.md](UI_V5_MIGRATION.md) for the native mapping and checks.
The standard font size is now 14; older appearance profiles migrate once.
Subsequent user choices (13, 14, 16 or 18) persist. The fonts are locally
installed Segoe UI, Arial or Consolas, with the existing Chinese fallback.
The menu starts collapsed into the title bar hamburger and can be pinned.
Menu pin state and Creation property height persist with the panel layout.
