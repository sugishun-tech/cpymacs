# cpymacs

A native C editor with Python customization and independent terminal and X11 frontends.

The editing engine, text storage, built-in commands, protocol server, terminal UI,
GUI and incremental lexical scanner are written in C11. Python is embedded in the
backend for configuration, commands, hooks, major-mode declarations and plugins.
There is no Python interpreter or editor engine inside the GUI process.

**Status: experimental, tested on GNU/Linux.** The listed Emacs key bindings and
commands are implemented, but this is **not a fully compatible GNU Emacs replacement**.
In particular, undo grouping, search case handling, window layout, advanced numeric
arguments and complete major-mode behavior differ. The exact boundary is documented
in [Compatibility](docs/compatibility.html). No speed advantage over GNU Emacs or the
original simplepypad is claimed without a like-for-like benchmark.

## Build on Debian or Ubuntu

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build pkg-config \
    python3-dev libjson-c-dev libx11-dev xfonts-base

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements: C11 compiler, CMake 3.18+, json-c 0.15+, and CPython 3.10+ development
files. Use the same Python installation for the interpreter and embedding library.
The GUI additionally uses Xlib. Tests use the Python standard library; install
`xvfb` to run the real GUI tests. Without Xvfb those tests are explicitly skipped.

For a build that does not discover, link or require X11:

```sh
cmake -S . -B build-nox -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCPYMACS_GUI=OFF \
    -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build-nox -j
./build-nox/cpymacs --nox README.md
```

`--no-python` disables plugins at runtime, but the executable is still linked with
CPython. It is not a Python-free build option.

## Run

```sh
./build/cpymacs example.py             # GUI when DISPLAY is available; TUI otherwise
./build/cpymacs --nox example.py       # Terminal, even when DISPLAY is set
./build/cpymacs -nw example.py         # Emacs-style terminal alias
./build/cpymacs --batch                # JSON Lines backend, no UI
./build/cpymacs --no-python --nox      # Native editing with no plugin execution
```

Files must be valid UTF-8 without NUL bytes. UTF-8 BOM and uniform CRLF line endings
are preserved. The default TAB inserts four ASCII spaces, not a tab character.
The X11 frontend also works through XWayland; it is not a native Wayland frontend.
Windows and macOS are not supported by this implementation.

## Use the supplied .emacs equivalent

```sh
./build/cpymacs --config examples/dotemacs.py example.py
./build/cpymacs --nox --config examples/dotemacs.py page.html.j2
```

The profile installs Python/Cython, web, JavaScript, TypeScript, PHP, CSS and Jinja2
mode policies; an original cyberpunk-inspired palette; and `C-c C-r` Python
formatting. Mode detection and syntax highlighting are plugins, not GUI widgets.

Global defaults and Python/Cython use four spaces. The profile intentionally
changes **only web-related modes to two literal spaces**, matching the supplied
configuration. RET inserts a plain newline in these web modes; electric indentation
and reindent-on-yank are not enabled.

Formatting runs optional `isort --quiet`, then optional
`autopep8 --in-place --ignore=E501`, using a temporary file in the source directory
when writable. Missing executables are ignored. A failure, timeout, invalid UTF-8,
or oversized output leaves the buffer unchanged. Diagnostics go to
`*Python formatter*`, and temporary files are removed. Install the formatter
executables separately, for example with your distribution's packages or a virtual
environment on `PATH`. Startup never installs packages or contacts ELPA/MELPA.

These plugins recreate the configuration's editing policies. They do **not**
reimplement all of web-mode, js2-mode, CC Mode, tree-sitter or the original Emacs
cyberpunk theme. See the [setting-by-setting migration map](docs/migration.html).

## Terminal colours and PuTTY (0.1.2)

**0.1.2 fixes the red text and underlined blank rows caused by 0.1.1.** The old
indexed-then-RGB fallback was unsafe: pre-0.71 PuTTY can interpret unsupported
RGB operands as independent display attributes instead of ignoring them.

Automatic terminal mode now sends **256-colour controls only** for recognized
TERM names, including `xterm`, `putty`, `screen` and `tmux`. It never adds RGB
controls, even when `COLORTERM` says `truecolor`. Unknown TERM values use
monochrome. RGB requires an explicit `--tui-colors=truecolor` selection on a
terminal path that actually supports it. No live capability detection is claimed.

All terminal faces reset their previous attributes. Syntax, normal text, padding,
selection and the software cursor no longer request underlining. Selection and
mode lines use background colour, with reverse-video fallback. Monochrome uses
plain text and reverse video, not underlined syntax. The backend, GUI palette,
Python configuration and editing commands are unchanged.

```sh
# Check that the newly built executable is running, not an older PATH entry.
./build/cpymacs --version
# Expected: cpymacs 0.1.2
./build/cpymacs --nox --config examples/dotemacs.py example.py

# Override an old CPYMACS_TUI_COLORS=truecolor environment setting, if present.
./build/cpymacs --nox --tui-colors=auto --config examples/dotemacs.py example.py
```

`--tui-colors` accepts `auto` (default), `256`, `truecolor`, or `mono`.
`--tui-cursor` accepts `block` (default) or `terminal`. Both accept `=` or a
separate value, work with `--connect`, and imply terminal mode when supplied.
Environment defaults are `CPYMACS_TUI_COLORS` and `CPYMACS_TUI_CURSOR`;
command-line arguments override them.

Indexed output uses only slots 16-255, not PuTTY's configurable default/basic
16-colour palette. Foreground and background are always specified together.
The 7:1 computed sRGB contrast adapter and software block cursor remain enabled.
No terminal palette is rewritten. When the client disables indexed colours,
`auto` cannot force them back on: choose `mono` to use readable terminal defaults,
or explicitly choose `truecolor` only on a known-compatible client. Refusing all
colour changes with identical foreground/background defaults cannot be repaired
by a remote application. See [Terminal colours](docs/terminal-colors.html) for
root cause, exact behavior, test boundaries and real XTerm screenshots.

## Customize with Python

The default configuration location is `$XDG_CONFIG_HOME/cpymacs/init.py`, or
`~/.config/cpymacs/init.py`. `--config FILE` is repeatable and loads additional files
in order. `-q` / `--no-user-config` skips the default file, not bundled mode plugins.

```python
from cpymacs import api
from cpymacs_plugins import dotemacs

dotemacs.install(api)
api.set_option("line_numbers", True)

def insert_timestamp(editor):
    from datetime import datetime
    editor.insert(datetime.now().isoformat(timespec="seconds"))

api.add_command("insert-timestamp", insert_timestamp)
api.bind_key("<f5>", "insert-timestamp")
```

The [Python API](docs/api.html) is UI-independent. Text positions are zero-based
UTF-8 **byte** offsets; `goto_character()` uses zero-based Unicode code points.
The interactive `M-x goto-char` command follows Emacs's **one-based** convention.
Configuration and plugins are trusted code with your operating-system permissions.
Do not load code from untrusted repositories.

## Attach independent clients

```sh
# Terminal A: run a persistent, foreground backend.
./build/cpymacs --server "$XDG_RUNTIME_DIR/cpymacs.sock" \
    --config examples/dotemacs.py example.py

# Terminal B: attach the native terminal frontend.
./build/cpymacs --nox --connect "$XDG_RUNTIME_DIR/cpymacs.sock"

# Terminal C: attach the native GUI frontend to the same session.
./build/cpymacs-gui --connect "$XDG_RUNTIME_DIR/cpymacs.sock"
```

Use `M-x detach-client` to disconnect a frontend without discarding backend state.
Use `C-x C-c` to quit the shared editor, with a modified-buffer confirmation. The
server is a single shared session: clients share buffers, point, minibuffer and
window layout, not independent Emacs frames. Do not issue simultaneous editing
commands from multiple clients expecting collaboration or conflict-free merging.
Use a private directory such as `$XDG_RUNTIME_DIR`; create one if that variable is
unset. The socket has mode `0600` and Linux peer-UID checks. A preexisting socket
path is never overwritten automatically.

## Familiar editing keys

| Keys | Commands |
| --- | --- |
| `C-f C-b C-n C-p`, `C-a C-e`, `M-f M-b`, `M-< M->` | Character, line, word and buffer movement |
| `C-SPC`, `C-x h`, `C-x C-x` | Mark, whole-buffer selection, exchange point/mark |
| `C-k`, `C-w`, `M-w`, `M-d`, `M-DEL`, `C-y`, `M-y` | Kill, copy, kill words, yank, rotate kill ring |
| `C-/`, `C-_`, `C-x u`, `C-?` | Undo; explicit redo with `C-?` |
| `C-s`, `C-r`, `M-%` | Incremental literal search and query replace |
| `C-x C-f`, `C-x C-s`, `C-x C-w`, `C-x C-c` | Open, save, write, quit |
| `C-x b`, `C-x k`, `C-x C-b` | Switch, kill, list buffers |
| `C-x 2`, `C-x 3`, `C-x o`, `C-x 0`, `C-x 1` | Split, select, delete windows |
| `C-u`, `M-0` ... `M-9`, `M--` | Numeric arguments |
| `C-x (`, `C-x )`, `C-x e` | Record, end, replay keyboard macro |
| `M-x`, `M-g g`, `C-g`, `C-h b`, `C-h m` | Commands, goto line, cancel, help |

See [Commands](docs/commands.html) for behavior and exceptions. In the terminal,
Alt is represented by ESC-prefixed keys, with a 60 ms disambiguation timeout.
`C-h` is help; DEL is backward deletion. GUI clipboard integration supports X11
UTF8_STRING and INCR transfers. The terminal supports bracketed paste, not OSC 52
clipboard export.

## Documentation and GitHub Pages

Open `docs/index.html` directly in a browser. The site is static, English-only,
uses relative links, and needs no JavaScript, external fonts or documentation build.

For branch-based Pages publishing, choose your publishing branch and `/docs` under
**Settings > Pages**. Alternatively, choose **GitHub Actions** as the publishing
source and use `.github/workflows/pages.yml`, which uploads `docs/` unchanged.
No repository has been published automatically by this source distribution.

## Verification and performance

```sh
ctest --test-dir build --output-on-failure
./build/bench_text 64 20000

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCPYMACS_SANITIZERS=ON -DCPYMACS_LTO=OFF \
    -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ./build-asan/test_text
ASAN_OPTIONS=detect_leaks=1 ./build-asan/test_term_style
```

The text-store property test performs 60,000 randomized UTF-8 edits, 600,000
invariant checks and 50,000 contiguous inserts. Black-box tests cover commands,
UTF-8 boundaries, CRLF/BOM, atomic saves, external edits, plugin failures, formatter
transactions, mode precedence, server attachment, real X11 events and real PTYs.
The terminal suite adds 11 PTY cases and a native 257-palette contrast test.
Full-process LeakSanitizer is disabled because embedded CPython retains process-wide
allocations; the native text-store test runs separately with leak checking enabled.
[Performance](docs/performance.html) explains the benchmark and its limitations.
[Verification](docs/verification.html) records what was actually run for this release.

## Layout and installation

```text
src/                     Native text engine, commands, protocol, Python bridge, UIs
include/                 C interfaces
python/cpymacs.py         Public customization API
python/cpymacs_plugins/   Major modes, highlighting policies, theme, formatter
examples/                Python configuration examples
tests/                   Property, backend integration and real frontend tests
bench/                   Reproducible native and protocol benchmarks
docs/                    Static English documentation and verification report
.github/workflows/       Build/test and optional Pages deployment
```

```sh
cmake --install build --prefix "$HOME/.local"
```

The installed executable looks for plugins in `../share/cpymacs/python` relative to
its executable directory, then the configured source/installation directories.
`CPYMACS_PYTHON_DIR` explicitly overrides the plugin directory. Add `~/.local/bin` to
`PATH` when installing locally.

## Safety and known limits

Save uses a same-directory temporary file, file fsync, rename, and best-effort directory fsync.
External-change checks prevent a known stale buffer from silently overwriting a
changed file. Permission bits are copied when possible; hard-link identity, ACLs and extended
attributes are not. See [Safety](docs/safety.html) before editing valuable files.
There is no automatic crash recovery, autosave journal, backup generation, binary
editing, legacy-encoding conversion, LSP, debugger, terminal emulator or Lisp runtime.
Keep backups or version control. An out-of-memory condition terminates the process.

## License and origin

MIT, preserving the simplepypad copyright in [LICENSE](LICENSE). This is a native
architectural rewrite of the supplied Python/Tkinter/curses project, not an Emacs
fork. GNU Emacs Lisp packages cannot be loaded. Old simplepypad plugins that access
Tk widgets or curses windows must be rewritten against the UI-independent API.
