"""Behavioral port of the supplied dotemacs.el, using bundled Python plugins.

The base editor keeps four-space TAB insertion. This optional profile uses two
literal spaces in web modes, disables their automatic indentation, and enables
Python/Cython formatting on C-c C-r. See docs/migration.html for explicit limits.
"""
from . import modes, cyberpunk, python_format

WEB_MODES = {
    "web-mode", "html-mode", "mhtml-mode", "css-mode", "css-ts-mode",
    "js-mode", "js2-mode", "js-ts-mode", "typescript-mode", "typescript-ts-mode",
    "tsx-ts-mode", "php-mode", "jinja2-mode",
}


def editing_setup(api):
    if api.buffer["mode"] in WEB_MODES:
        api.set_option("tab_width", 2)
        api.set_option("indent_width", 2)
        api.set_option("auto_indent", False)
    elif api.buffer["mode"] in {"python-mode", "python-ts-mode", "cython-mode"}:
        api.set_option("tab_width", 4)
        api.set_option("indent_width", 4)


def insert_two_spaces(api):
    api.insert("  ")  # The supplied interactive Lisp function ignores its prefix.


def install(api):
    if getattr(api, "_dotemacs_installed", False):
        return
    api._dotemacs_installed = True
    cyberpunk.install(api)
    python_format.install(api)
    api.add_command("my/insert-two-spaces", insert_two_spaces)
    for mode in WEB_MODES:
        api.bind_key("TAB", "my/insert-two-spaces", mode=mode)
        api.bind_key("RET", "newline", mode=mode)
    modes.ACTIVATION_HOOKS.append(editing_setup)
    # Manual mode commands must apply the same local settings as extension detection.
    for mode in sorted(WEB_MODES):
        def command(editor, selected=mode):
            from . import web_modes
            if selected.startswith("css"):
                values = ("css", 4, web_modes.CSS_WORDS)
            elif selected.startswith("typescript"):
                values = ("typescript", 2, web_modes.TS_WORDS)
            elif selected.startswith("js"):
                values = ("javascript", 2, web_modes.JS_WORDS)
            elif selected == "php-mode":
                values = ("php", 2, web_modes.PHP_WORDS)
            elif selected == "jinja2-mode":
                values = ("jinja", 5, web_modes.JINJA_WORDS)
            else:
                values = ("html", 3, web_modes.TS_WORDS + " " + web_modes.PHP_WORDS + " " + web_modes.JINJA_WORDS)
            web_modes.activate(editor, selected, *values)
            editing_setup(editor)
        api.add_command(mode, command)
    modes.detect(api)
