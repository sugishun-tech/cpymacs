"""Python and Cython major modes, including native multiline highlighting."""
import keyword
import builtins

KEYWORDS = " ".join(keyword.kwlist + ["match", "case", "type"])
BUILTINS = " ".join(name for name in dir(builtins) if not name.startswith("_"))
CYTHON_KEYWORDS = " cdef cpdef cimport ctypedef extern public readonly struct union enum nogil gil include DEF IF ELIF ELSE"


def activate(api, *, cython=False):
    api.set_mode("cython-mode" if cython else "python-mode",
                 "cython" if cython else "python", 1,
                 KEYWORDS + (CYTHON_KEYWORDS if cython else ""), BUILTINS)
    api.set_option("tab_width", 4)
    api.set_option("indent_width", 4)
    api.set_option("auto_indent", True)


def install(api):
    api.add_command("python-mode", lambda editor: activate(editor))
    api.add_command("python-ts-mode", lambda editor: activate(editor))
    api.add_command("cython-mode", lambda editor: activate(editor, cython=True))
