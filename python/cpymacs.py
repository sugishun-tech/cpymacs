"""Public customization API. All text storage, commands and rendering are native C.

Positions are zero-based UTF-8 byte offsets unless explicitly documented otherwise.
Configuration files execute trusted Python, not sandboxed Python.
"""
from __future__ import annotations
import importlib
from pathlib import Path
from typing import Callable, Any
import _cpymacs as _native


class EditorAPI:
    argument_count: int = 1
    argument: str | None = None

    @property
    def buffer(self) -> dict[str, Any]:
        return _native.info()

    @property
    def path(self) -> Path | None:
        value = self.buffer["path"]
        return Path(value) if value else None

    @property
    def point(self) -> int:
        return self.buffer["point"]

    @point.setter
    def point(self, value: int) -> None:
        _native.set_point(value)

    def get_text(self, start: int = 0, end: int = -1) -> str:
        return _native.get_text(start, end)

    def set_text(self, text: str) -> None:
        _native.set_text(text)

    def insert(self, text: str) -> None:
        _native.insert(text)

    def replace(self, start: int, end: int, text: str) -> None:
        """Replace the half-open byte range [start, end), in one undo group."""
        if end < start:
            raise ValueError("end precedes start")
        _native.replace(start, end - start, text)

    def get_selection(self) -> str:
        info = self.buffer
        if not info["mark_active"]:
            return ""
        start, end = sorted((info["point"], info["mark"]))
        return self.get_text(start, end)

    def replace_selection(self, text: str) -> bool:
        info = self.buffer
        if not info["mark_active"]:
            return False
        start, end = sorted((info["point"], info["mark"]))
        self.replace(start, end, text)
        return True

    def goto_character(self, position: int) -> None:
        """Move to a zero-based Unicode code-point offset, not a byte offset."""
        _native.set_point(position, True)

    def add_command(self, name: str, callback: Callable[[EditorAPI], Any]) -> None:
        _native.register(name, callback)

    def run_command(self, name: str, argument: str | None = None, count: int = 1) -> None:
        _native.command(name, argument, count)

    def bind_key(self, sequence: str, command: str, *, mode: str | None = None) -> None:
        """Bind canonical tokens, for example 'C-c C-r', 'M-x', or '<f5>'."""
        _native.bind(sequence, command, mode)

    def on(self, event: str, callback: Callable[[EditorAPI], Any]) -> None:
        _native.on(event, callback)

    def set_option(self, name: str, value: Any) -> None:
        _native.set_option(name, value)

    def get_option(self, name: str) -> Any:
        return self.buffer[name]

    def set_font(self, family: str = "fixed", size: int = 16) -> None:
        self.set_option("font", family)
        self.set_option("font_size", size)

    def set_theme(self, name: str, colors: list[int]) -> None:
        _native.set_theme(name, colors)

    def set_mode(self, name: str, syntax: str, kind: int,
                 keywords: str = "", builtins: str = "") -> None:
        _native.set_mode(name, syntax, kind, keywords, builtins)

    def status(self, message: str) -> None:
        _native.message(message)

    def open_file(self, path: str | Path) -> None:
        _native.open_file(str(path))

    def save_file(self, path: str | Path | None = None, *, force: bool = False) -> None:
        _native.save_file(str(path) if path is not None else None, force)

    def log(self, name: str, text: str) -> None:
        _native.log(name, text)

    def load_plugin(self, module_name: str) -> None:
        """Import a trusted Python module and call its install(api) function."""
        importlib.import_module(module_name).install(self)


api = EditorAPI()
