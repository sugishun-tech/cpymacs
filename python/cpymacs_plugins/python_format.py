"""Transactional isort + autopep8 integration matching the supplied .emacs.

No shell is invoked. The edited buffer changes only after every available
formatter exits successfully and its output passes strict UTF-8 validation.
"""
from __future__ import annotations
import difflib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

TIMEOUT_SECONDS = 30
MAX_FORMAT_BYTES = 16 * 1024 * 1024


def _map_position(before: str, after: str, position: int) -> int:
    """Preserve point across equal lines, then preserve its in-line column."""
    old_lines = before.splitlines(keepends=True)
    new_lines = after.splitlines(keepends=True)
    before_point = before[:position]
    row = before_point.count("\n")
    column = len(before_point.rsplit("\n", 1)[-1])
    mapping = difflib.SequenceMatcher(None, old_lines, new_lines, autojunk=True)
    target = len(new_lines)
    for tag, a, z, x, y in mapping.get_opcodes():
        if a <= row < z:
            target = x + min(row - a, max(0, y - x - 1))
            break
        if row == z:
            target = y
    prefix = sum(map(len, new_lines[:target]))
    line = new_lines[target].rstrip("\r\n") if target < len(new_lines) else ""
    return min(len(after), prefix + min(column, len(line)))


def format_buffer(api):
    info = api.buffer
    if info["mode"] not in {"python-mode", "python-ts-mode", "cython-mode"}:
        raise RuntimeError("Python formatter requires a Python or Cython buffer")
    programs = []
    for name, flags in (("isort", ["--quiet"]),
                        ("autopep8", ["--in-place", "--ignore=E501"])):
        executable = shutil.which(name)
        if executable:
            programs.append((executable, flags))
    if not programs:
        # The supplied .emacs deliberately ignores absent executables.
        return
    original = api.get_text()
    encoded = original.encode("utf-8")
    if len(encoded) > MAX_FORMAT_BYTES:
        raise RuntimeError("Formatter size limit exceeded (16 MiB)")
    old_character_point = len(encoded[:info["point"]].decode("utf-8"))
    source_dir = api.path.parent if api.path else None
    temp_dir = source_dir if source_dir and os.access(source_dir, os.W_OK) else None
    suffix = ".pyx" if info["mode"] == "cython-mode" else ".py"
    logs = []
    api.log("*Python formatter*", "")
    try:
        with tempfile.NamedTemporaryFile(prefix="cpymacs-python-format-", suffix=suffix,
                                         dir=temp_dir, delete=False) as temporary:
            temp_path = Path(temporary.name)
            temporary.write(encoded)
        for executable, flags in programs:
            result = subprocess.run([executable, *flags, str(temp_path)],
                                    cwd=source_dir, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, check=False,
                                    timeout=TIMEOUT_SECONDS)
            logs.append(result.stdout.decode("utf-8", errors="replace"))
            api.log("*Python formatter*", "".join(logs))
            if result.returncode:
                raise RuntimeError(f"{Path(executable).name} failed with exit code "
                                   f"{result.returncode}; see *Python formatter*")
        # Limit malicious or broken tool output before allocating a large string.
        if temp_path.stat().st_size > MAX_FORMAT_BYTES:
            raise RuntimeError("Formatter output exceeds the 16 MiB safety limit")
        formatted = temp_path.read_bytes().decode("utf-8")
        if "\x00" in formatted:
            raise RuntimeError("Formatter output contains a NUL byte")
        # Hooks or future asynchronous integrations must not overwrite newer work.
        if api.buffer["id"] != info["id"] or api.buffer["revision"] != info["revision"]:
            raise RuntimeError("Buffer changed during formatting; no result applied")
        target = _map_position(original, formatted, old_character_point)
        api.set_text(formatted)
        api.goto_character(target)
        api.status("Python formatting completed")
    except subprocess.TimeoutExpired as exc:
        logs.append(f"Formatter exceeded {TIMEOUT_SECONDS} seconds\n")
        api.log("*Python formatter*", "".join(logs))
        raise RuntimeError("Formatter timed out; buffer was not changed") from exc
    finally:
        if "temp_path" in locals():
            temp_path.unlink(missing_ok=True)


def install(api):
    api.add_command("my/python-format-buffer", format_buffer)
    api.add_command("python-format-buffer", format_buffer)
    for mode in ("python-mode", "python-ts-mode", "cython-mode"):
        api.bind_key("C-c C-r", "my/python-format-buffer", mode=mode)
