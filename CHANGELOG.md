# Changelog

## 0.1.1 - 2026-09-24

### Terminal readability

- Add a native terminal-only high-contrast palette adapter, without changing the
  backend theme, Python theme API, GUI, or user configuration.
- Set foreground and background together; emit an indexed fallback before RGB
  in automatic mode. Never use configurable ANSI colour slots 0 through 15.
- Enforce 7:1 computed sRGB contrast for text, comments, line numbers, selections,
  mode lines and the cursor, including indexed conversion and custom themes.
- Paint a software block cursor, independent of terminal cursor colours, with
  row-cache invalidation on movement. Preserve wide-character positioning and
  minibuffer/split-pane cursor rendering.
- Add `--tui-colors` and `--tui-cursor`, their environment defaults, validation,
  and support for attaching to an existing backend with local display settings.
- Add a no-colour mode using underlines and reverse video, without bold colours.
- Paint all padding explicitly instead of relying on terminal erase backgrounds.
- Add 11 PTY regressions, a 257-palette native test, actual XTerm render evidence,
  and English terminal-colour documentation.

Terminal refusals cannot be overridden. With both extended colour paths disabled,
readability depends on the terminal's own defaults. Windows PuTTY was not executed;
its documented acceptance switches were modeled and XTerm was rendered directly.

## 0.1.0 - 2026-09-24

Initial C11 editor, independent X11 and terminal frontends, Python customization,
Emacs-style command subset, bundled development profile and static documentation.
See `docs/verification-0.1.0.json` for the original release's recorded evidence.
