# Changelog

## 0.1.2 - 2026-09-24

### Fix legacy PuTTY red text and unwanted underlines

- Remove the unsafe indexed-then-RGB sequence from automatic terminal mode.
  Recognized terminals now receive indexed colour only, including when a remote
  COLORTERM variable claims truecolor. RGB is explicit opt-in, not a fallback.
- Reset attributes for every face and remove intentional underline styling from
  syntax, padding, selections and the software cursor, in every colour mode.
- Use background colour and reverse video for selection/mode-line distinction.
  Keep the high-contrast adapter, fixed extended-colour indices and block cursor.
- Add a regression fixture captured from the actual 0.1.1 default-theme output.
  A model of the PuTTY 0.70 SGR switch reproduces its red, underlined rows; new
  output for both default and cyberpunk themes must pass without those defects.
- Test misleading environment variables, all colour modes, pre-existing underline
  state, both themes, hostile terminal palettes, and preserved syntax colours.
- Update English documentation and record new tests separately from old evidence.
  No Python plugin, backend editing behavior or X11 GUI code is changed.

The 0.1.1 test model consumed unknown RGB groups as a unit. That was an incorrect
assumption for old PuTTY and failed to exercise the reported bug. Its old results
are retained as historical records, not as proof of legacy-client compatibility.


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
