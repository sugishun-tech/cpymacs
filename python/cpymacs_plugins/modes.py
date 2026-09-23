"""Bundled mode registry and extension detection, independent of any frontend."""
from . import python_mode, web_modes

# Longest suffix wins. A template rule must precede generic PHP or HTML handling.
SUFFIX_MODES = {
    ".py": "python-mode", ".pyw": "python-mode", ".pyi": "python-mode",
    ".pyx": "cython-mode", ".pxd": "cython-mode", ".pxi": "cython-mode",
    ".php": "php-mode", ".phtml": "web-mode", ".tpl.php": "web-mode", ".blade.php": "web-mode",
    ".html": "web-mode", ".htm": "web-mode", ".xhtml": "web-mode", ".shtml": "web-mode",
    ".css": "css-mode", ".scss": "web-mode", ".less": "web-mode",
    ".js": "js2-mode", ".mjs": "js2-mode", ".cjs": "js2-mode",
    ".ts": "typescript-mode", ".jsx": "web-mode", ".tsx": "web-mode",
    ".vue": "web-mode", ".svelte": "web-mode", ".twig": "web-mode",
    ".j2": "jinja2-mode", ".jinja": "jinja2-mode", ".jinja2": "jinja2-mode",
    ".c": "c-mode", ".h": "c-mode", ".cpp": "c-mode", ".cc": "c-mode",
    ".json": "json-mode", ".rs": "rust-mode", ".go": "go-mode",
    ".sh": "sh-mode", ".bash": "sh-mode", ".yaml": "yaml-mode", ".yml": "yaml-mode",
}
ACTIVATION_HOOKS = []


def activate(api, command):
    api.run_command(command)
    for callback in tuple(ACTIVATION_HOOKS):
        callback(api)


def detect(api):
    name = api.path.name.lower() if api.path else api.buffer["name"].lower()
    for suffix in sorted(SUFFIX_MODES, key=len, reverse=True):
        if name.endswith(suffix):
            activate(api, SUFFIX_MODES[suffix])
            return
    if name in {".bashrc", ".bash_profile", ".profile"}:
        activate(api, "sh-mode")
    else:
        activate(api, "fundamental-mode")


def install(api):
    python_mode.install(api)
    web_modes.install(api)
    definitions = {
        "fundamental-mode": ("text", 0, ""),
        "c-mode": ("c", 2, "auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Atomic _Bool _Generic _Noreturn _Static_assert _Thread_local"),
        "json-mode": ("json", 2, "true false null"),
        "rust-mode": ("rust", 2, "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while"),
        "go-mode": ("go", 2, "break case chan const continue default defer else fallthrough for func go goto if import interface map package range return select struct switch type var"),
        "sh-mode": ("shell", 1, "if then else elif fi for in do done case esac function while until select time export local readonly declare unset"),
        "yaml-mode": ("yaml", 1, "true false null yes no on off"),
    }
    for name, (syntax, kind, words) in definitions.items():
        def command(editor, name=name, syntax=syntax, kind=kind, words=words):
            editor.set_mode(name, syntax, kind, words)
            editor.set_option("tab_width", 4)
            editor.set_option("indent_width", 4)
            editor.set_option("auto_indent", False)
        api.add_command(name, command)
    api.add_command("normal-mode", detect)
    api.on("after_open", detect)
