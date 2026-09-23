"""Web major modes. The native scanner is lexical, not an AST validator."""
JS_WORDS = "async await break case catch class const continue debugger default delete do else export extends false finally for from function get if import in instanceof let new null of return set static super switch this throw true try typeof undefined var void while with yield"
TS_WORDS = JS_WORDS + " abstract any as asserts boolean constructor declare enum implements infer interface is keyof module namespace never number object private protected public readonly require string symbol type unique unknown"
PHP_WORDS = "abstract and array as break callable case catch class clone const continue declare default die do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile eval exit extends final finally fn for foreach function global goto if implements include include_once instanceof insteadof interface isset list match namespace new null or print private protected public readonly require require_once return static switch throw trait true false try unset use var while xor yield"
JINJA_WORDS = "and as block call do elif else endblock endcall endfilter endfor endif endmacro endraw endset extends false filter for from if import in include is macro not or raw recursive set true with without context super loop self"
CSS_WORDS = "important inherit initial unset revert auto none flex grid block inline absolute relative fixed sticky media supports keyframes import layer container"


def activate(api, mode, syntax, kind, words):
    api.set_mode(mode, syntax, kind, words)
    api.set_option("tab_width", 4)
    api.set_option("indent_width", 4)
    api.set_option("auto_indent", False)


def install(api):
    definitions = {
        "web-mode": ("html", 3, TS_WORDS + " " + PHP_WORDS + " " + JINJA_WORDS),
        "html-mode": ("html", 3, JINJA_WORDS),
        "mhtml-mode": ("html", 3, JS_WORDS),
        "css-mode": ("css", 4, CSS_WORDS),
        "css-ts-mode": ("css", 4, CSS_WORDS),
        "js-mode": ("javascript", 2, JS_WORDS),
        "js2-mode": ("javascript", 2, JS_WORDS),
        "js-ts-mode": ("javascript", 2, JS_WORDS),
        "typescript-mode": ("typescript", 2, TS_WORDS),
        "typescript-ts-mode": ("typescript", 2, TS_WORDS),
        "tsx-ts-mode": ("tsx", 3, TS_WORDS),
        "php-mode": ("php", 2, PHP_WORDS),
        "jinja2-mode": ("jinja", 5, JINJA_WORDS),
    }
    for name, (syntax, kind, words) in definitions.items():
        def command(editor, name=name, syntax=syntax, kind=kind, words=words):
            activate(editor, name, syntax, kind, words)
        api.add_command(name, command)
