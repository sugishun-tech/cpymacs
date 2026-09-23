#!/usr/bin/env python3
"""Black-box native-backend tests. No third-party Python packages are required."""
from __future__ import annotations
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 else str(Path('build/cpymacs').resolve())
ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / 'examples' / 'dotemacs.py'


class Session:
    def __init__(self, *args: str, env: dict | None = None):
        self.errors = tempfile.TemporaryFile()
        self.proc = subprocess.Popen([BINARY, '--batch', '--no-user-config', *map(str, args)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.errors, env=env)
        self.state = None
        self.request('hello', rows=12, cols=90)

    def request(self, op: str, expected: bool | None = True, **fields):
        message = {'op': op, **fields}
        self.proc.stdin.write(json.dumps(message, ensure_ascii=False).encode() + b'\n')
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            self.errors.seek(0)
            raise AssertionError(f'Backend terminated: {self.errors.read().decode(errors="replace")}')
        response = json.loads(line)
        if expected is not None and response.get('ok') != expected:
            raise AssertionError(f'{message!r}: {response!r}')
        if 'state' in response:
            self.state = response['state']
        return response

    def keys(self, *keys, expected=True):
        return self.request('keys', keys=list(keys), expected=expected)

    def command(self, name, argument=None, **kw):
        if argument is not None:
            kw['argument'] = argument
        return self.request('command', name=name, **kw)

    def text(self):
        return self.request('get_text', no_snapshot=True)['result']

    def set(self, text, point=0):
        self.request('set_text', text=text)
        self.request('point', byte=point)

    @property
    def pane(self):
        return next(p for p in self.state['panes'] if p['active'])

    def close(self):
        if self.proc.stdin and not self.proc.stdin.closed:
            self.proc.stdin.close()
        try:
            code = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise AssertionError('Backend did not stop after EOF')
        self.proc.stdout.close()
        self.errors.seek(0)
        errors = self.errors.read().decode(errors='replace')
        self.errors.close()
        if code != 0 or 'AddressSanitizer' in errors or 'runtime error:' in errors:
            raise AssertionError(f'Backend exit {code}: {errors}')


class EditorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cpymacs-test-')
        self.directory = Path(self.temp.name)
        self.sessions = []

    def tearDown(self):
        try:
            for session in self.sessions:
                session.close()
        finally:
            self.temp.cleanup()

    def session(self, *args, **kw):
        result = Session(*args, **kw)
        self.sessions.append(result)
        return result

    def config(self, content):
        path = self.directory / f'config-{len(list(self.directory.glob("config-*")))}.py'
        path.write_text(content)
        return path

    def test_default_four_spaces(self):
        s = self.session()
        s.keys('TAB', 'x', 'RET', 'TAB')
        self.assertEqual(s.text(), '    x\n    ')
        self.assertEqual(s.pane['tab_width'], 4)

    def test_without_python(self):
        s = self.session('--no-python', str(self.directory / 'sample.py'))
        self.assertEqual(s.pane['mode'], 'fundamental-mode')
        s.keys('TAB')
        self.assertEqual(s.text(), '    ')

    def test_emacs_char_and_line_motion(self):
        s = self.session()
        s.set('abc\ndef\nghi')
        s.keys('C-f', 'C-f', 'C-n')
        self.assertEqual(s.pane['point'], 6)
        s.keys('C-a', 'C-e', 'C-p', 'C-b')
        self.assertEqual(s.pane['point'], 2)
        s.keys('M->')
        self.assertEqual(s.pane['point'], 11)
        s.keys('M-<')
        self.assertEqual(s.pane['point'], 0)

    def test_goal_column_recovers_after_short_line(self):
        s = self.session()
        s.set('abcdef\nx\nabcdef', 5)
        s.keys('C-n')
        self.assertEqual(s.pane['point'], 8)
        s.keys('C-n')
        self.assertEqual(s.pane['point'], 14)

    def test_unicode_codepoints_and_byte_boundaries(self):
        s = self.session()
        s.set('a\u03bb\u65e5\U0001f600z')
        s.keys('C-f', 'C-f', 'C-f')
        self.assertEqual(s.pane['point'], 6)
        s.command('goto-char', '4')
        self.assertEqual(s.pane['point'], 6)
        s.request('point', byte=2, expected=False)
        s.request('replace', start=1, end=2, text='x', expected=False)
        s.keys('C-d')
        self.assertEqual(s.text(), 'a\u03bb\u65e5z')
        s.keys('DEL')
        self.assertEqual(s.text(), 'a\u03bbz')
        self.assertEqual(s.pane['characters'], 3)

    def test_numeric_prefix_and_cancel(self):
        s = self.session()
        s.keys('C-u', 'C-u', 'x', 'M-3', 'y')
        self.assertEqual(s.text(), 'x'*16 + 'yyy')
        s.keys('M--', 'M-2', 'C-f')
        self.assertEqual(s.pane['point'], 17)
        s.keys('C-x', 'C-g', 'z')
        self.assertEqual(s.text(), 'x'*16 + 'yzyy')

    def test_word_motion_and_case(self):
        s = self.session()
        s.set('one two_three FOUR')
        s.keys('M-f')
        self.assertEqual(s.pane['point'], 3)
        s.keys('M-f')
        self.assertEqual(s.pane['point'], 7)
        s.keys('M-b', 'M-u')
        self.assertEqual(s.text(), 'one TWO_three FOUR')
        s.keys('M->', 'M-b', 'M-c')
        self.assertEqual(s.text(), 'one TWO_three Four')

    def test_mark_kill_yank(self):
        s = self.session()
        s.set('alpha beta')
        s.keys('C-SPC', 'M-f', 'C-w')
        self.assertEqual(s.text(), ' beta')
        s.keys('M->', 'C-y')
        self.assertEqual(s.text(), ' betaalpha')
        s.keys('C-x', 'C-x')
        self.assertEqual(s.pane['point'], 5)
        self.assertTrue(s.pane['mark_active'])

    def test_consecutive_kills_append(self):
        s = self.session()
        s.set('one\ntwo\nthree')
        s.keys('C-k', 'C-k', 'C-k')
        self.assertEqual(s.text(), '\nthree')
        self.assertEqual(s.request('kill_ring', no_snapshot=True)['result'], 'one\ntwo')
        s.keys('C-y')
        self.assertEqual(s.text(), 'one\ntwo\nthree')

    def test_yank_pop(self):
        s = self.session()
        s.set('one two')
        s.keys('M-d', 'C-f', 'M-d', 'C-y', 'M-y')
        self.assertEqual(s.text(), ' one')
        s.keys('C-b')
        s.keys('M-y', expected=False)

    def test_external_yank_joins_ring(self):
        s = self.session()
        s.set('old')
        s.keys('C-k')
        s.request('yank_external', text='external')
        self.assertEqual(s.text(), 'external')
        s.keys('M-y')
        self.assertEqual(s.text(), 'old')

    def test_undo_redo_and_history_branch(self):
        s = self.session()
        s.keys('a', 'b', 'C-/', 'C-?')
        self.assertEqual(s.text(), 'ab')
        s.keys('C-/', 'c')
        self.assertEqual(s.text(), 'ac')
        s.command('undo-redo', expected=False)
        s.keys('C-/', 'C-/')
        self.assertEqual(s.text(), '')
        self.assertFalse(s.pane['modified'])

    def test_transpose_and_open_line(self):
        s = self.session()
        s.set('ab', 2)
        s.keys('C-t')
        self.assertEqual(s.text(), 'ba')
        s.keys('C-a', 'C-o')
        self.assertEqual(s.text(), '\nba')
        self.assertEqual(s.pane['point'], 0)

    def test_readonly_rejects_edits(self):
        s = self.session()
        s.set('safe')
        s.keys('C-x', 'C-q')
        s.keys('x', expected=False)
        s.command('undo', expected=False)
        self.assertEqual(s.text(), 'safe')
        s.keys('C-x', 'C-q', 'x')
        self.assertEqual(s.text(), 'xsafe')

    def test_incremental_search_and_paste(self):
        s = self.session()
        s.set('zero cat dog cat')
        s.keys('C-s', 'c', 'a', 't')
        self.assertEqual(s.pane['point'], 8)
        s.keys('C-s')
        self.assertEqual(s.pane['point'], 16)
        s.keys('RET', 'M-<', 'C-s')
        s.request('insert', text='dog')
        self.assertEqual(s.pane['point'], 12)
        s.keys('RET')
        self.assertEqual(s.state['prompt'], '')

    def test_incremental_search_cancel(self):
        s = self.session()
        s.set('alpha beta')
        s.keys('C-s', 'b', 'e', 'z')
        self.assertIn('Failing', s.state['prompt'])
        s.keys('C-g')
        self.assertEqual(s.state['input'], 'be')
        s.keys('C-g')
        self.assertEqual(s.pane['point'], 0)
        self.assertEqual(s.state['prompt'], '')

    def test_query_replace(self):
        s = self.session()
        s.set('cat cat cat')
        s.keys('M-%', 'c', 'a', 't', 'RET', 'd', 'o', 'g', 'RET', 'y', 'n', '!')
        self.assertEqual(s.text(), 'dog cat dog')

    def test_command_and_filename_completion(self):
        s = self.session()
        s.set('a\nb\nc')
        s.keys('M-x', *'goto-l', 'TAB', 'RET', '3', 'RET')
        self.assertEqual(s.pane['point'], 4)
        target = self.directory / 'unambiguous.txt'
        target.write_text('complete')
        s.keys('C-x', 'C-f')
        s.request('insert', text=str(self.directory / 'unambig'))
        s.keys('TAB', 'RET')
        self.assertEqual(s.text(), 'complete')

    def test_keyboard_macro(self):
        s = self.session()
        s.keys('C-x', '(', 'a', 'b', 'C-x', ')', 'C-x', 'e')
        self.assertEqual(s.text(), 'abab')
        s.command('call-last-kbd-macro', count=2)
        self.assertEqual(s.text(), 'abababab')

    def test_buffer_switch_and_kill_confirmation(self):
        s = self.session()
        s.set('keep')
        s.command('switch-to-buffer', 'other')
        s.keys('x')
        s.command('kill-buffer', 'other')
        self.assertIn('modified', s.state['prompt'])
        s.keys('n')
        self.assertEqual(s.text(), 'x')
        s.command('kill-buffer', 'other')
        s.keys('y')
        self.assertEqual(s.text(), 'keep')

    def test_window_split_switch_and_delete(self):
        s = self.session()
        s.set('one')
        s.keys('C-x', '3', 'C-x', 'o')
        s.command('switch-to-buffer', 'other')
        s.set('two')
        self.assertEqual(len(s.state['panes']), 2)
        s.keys('C-x', 'o')
        self.assertEqual(s.text(), 'one')
        s.keys('C-x', '1')
        self.assertEqual(len(s.state['panes']), 1)

    def test_exit_requires_confirmation(self):
        s = self.session()
        s.keys('x', 'C-x', 'C-c')
        self.assertFalse(s.state['quit'])
        s.keys('n')
        self.assertEqual(s.text(), 'x')

    def test_atomic_save_and_clean_undo(self):
        target = self.directory / 'document.txt'
        target.write_text('original')
        target.chmod(0o640)
        s = self.session(target)
        inode = target.stat().st_ino
        s.keys('M->', '!')
        s.keys('C-x', 'C-s')
        self.assertEqual(target.read_text(), 'original!')
        self.assertNotEqual(target.stat().st_ino, inode)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o640)
        self.assertFalse(s.pane['modified'])
        s.keys('x', 'C-/')
        self.assertFalse(s.pane['modified'])
        self.assertEqual(list(self.directory.glob('.cpymacs-*')), [])

    def test_conflicting_external_edit_not_overwritten(self):
        target = self.directory / 'document.txt'
        target.write_text('original')
        s = self.session(target)
        s.keys('x')
        target.write_text('changed elsewhere with different length')
        s.request('save', expected=False)
        self.assertEqual(target.read_text(), 'changed elsewhere with different length')
        s.request('save', force=True)
        self.assertEqual(target.read_text(), 'xoriginal')

    def test_refuse_unconfirmed_overwrite(self):
        target = self.directory / 'existing.txt'
        target.write_text('safe')
        s = self.session()
        s.set('new')
        s.request('save', path=str(target), expected=False)
        self.assertEqual(target.read_text(), 'safe')
        s.request('save', path=str(target), force=True)
        self.assertEqual(target.read_text(), 'new')

    def test_crlf_bom_and_unicode_roundtrip(self):
        target = self.directory / 'utf8.txt'
        data = b'\xef\xbb\xbf' + 'a\u65e5\r\nb\r\n'.encode()
        target.write_bytes(data)
        s = self.session(target)
        self.assertEqual(s.text(), 'a\u65e5\nb\n')
        s.request('save')
        self.assertEqual(target.read_bytes(), data)

    def test_invalid_utf8_binary_and_directory_rejected(self):
        s = self.session()
        for i, data in enumerate((b'\xff\xfea', b'a\x00b', b'\xc0\x80')):
            target = self.directory / str(i)
            target.write_bytes(data)
            s.request('open', path=str(target), expected=False)
        s.request('open', path=str(self.directory), expected=False)
        self.assertEqual(s.text(), '')

    def test_symlink_save_keeps_link(self):
        target = self.directory / 'real.txt'
        target.write_text('old')
        link = self.directory / 'link.txt'
        link.symlink_to(target)
        s = self.session(link)
        s.set('new')
        s.request('save')
        self.assertTrue(link.is_symlink())
        self.assertEqual(target.read_text(), 'new')

    def test_mode_detection_and_profile_precedence(self):
        modes = {
            'x.py': 'python-mode', 'x.pyw': 'python-mode', 'x.pyx': 'cython-mode',
            'x.pxd': 'cython-mode', 'x.pxi': 'cython-mode', 'x.php': 'php-mode',
            'x.blade.php': 'web-mode', 'x.tpl.php': 'web-mode', 'x.phtml': 'web-mode',
            'x.html': 'web-mode', 'x.htm': 'web-mode', 'x.xhtml': 'web-mode',
            'x.shtml': 'web-mode', 'x.css': 'css-mode', 'x.scss': 'web-mode',
            'x.less': 'web-mode', 'x.js': 'js2-mode', 'x.mjs': 'js2-mode',
            'x.cjs': 'js2-mode', 'x.ts': 'typescript-mode', 'x.jsx': 'web-mode',
            'x.tsx': 'web-mode', 'x.vue': 'web-mode', 'x.svelte': 'web-mode',
            'x.twig': 'web-mode', 'x.html.j2': 'jinja2-mode', 'x.jinja': 'jinja2-mode',
            'x.jinja2': 'jinja2-mode',
        }
        s = self.session('--config', PROFILE)
        self.assertEqual(s.state['theme'], 'cyberpunk')
        for filename, mode in modes.items():
            with self.subTest(filename=filename):
                s.request('open', path=str(self.directory / filename))
                self.assertEqual(s.pane['mode'], mode)
                width = 4 if mode in ('python-mode', 'cython-mode') else 2
                self.assertEqual(s.pane['tab_width'], width)
                s.keys('TAB')
                self.assertEqual(s.text(), ' '*width)

    def test_profile_web_literal_tab_and_raw_return(self):
        s = self.session('--config', PROFILE, self.directory / 'index.tsx')
        s.set('    <div>', 9)
        s.keys('RET', 'TAB')
        self.assertEqual(s.text(), '    <div>\n  ')
        self.assertFalse(s.pane['auto_indent'])
        s.keys('C-u', 'TAB')
        self.assertEqual(s.text(), '    <div>\n    ')  # The original command ignores prefix arguments.
        s.command('switch-to-buffer', '*scratch*')
        s.keys('TAB')
        self.assertEqual(s.text(), '    ')

    def test_python_indent_and_highlight_plugin(self):
        s = self.session(self.directory / 'example.py')
        s.set('def func():', 11)
        s.keys('RET', *'return 1')
        self.assertEqual(s.text(), 'def func():\n    return 1')
        styles = {span[2] for line in s.pane['lines'] for span in line['spans']}
        self.assertIn(5, styles)
        self.assertGreater(len(styles), 2)

    def test_multiline_lexical_state_and_edit_invalidation(self):
        s = self.session(self.directory / 'example.py')
        s.set('"""hello\ninside\n"""\nreturn 1\n')
        first = s.pane['lines'][1]['spans']
        self.assertTrue(first)
        s.request('replace', start=0, end=3, text='')
        second = s.pane['lines'][1]['spans']
        self.assertNotEqual(first, second)

    def test_config_print_and_exception_isolation(self):
        config = self.config("print('config output')\nfrom cpymacs import api\ndef broken(api):\n    raise ValueError('intentional failure')\napi.add_command('broken', broken)\n")
        s = self.session('--config', config)
        r = s.command('broken', expected=False)
        self.assertIn('intentional failure', r['error'])
        s.keys('x')
        self.assertEqual(s.text(), 'x')

    def test_before_save_hook_aborts_save(self):
        config = self.config("from cpymacs import api\ndef block(api):\n    raise RuntimeError('blocked by hook')\napi.on('before_save', block)\n")
        target = self.directory / 'file.txt'
        target.write_text('disk')
        s = self.session('--config', config, target)
        s.set('edited')
        s.request('save', expected=False)
        self.assertEqual(target.read_text(), 'disk')

    def test_custom_python_command_and_utf8_api(self):
        config = self.config("from cpymacs import api\ndef custom(api):\n    api.set_text('a\\u65e5z')\n    api.goto_character(2)\n    api.insert('!' * api.argument_count)\napi.add_command('custom', custom)\napi.bind_key('C-c t', 'custom')\n")
        s = self.session('--config', config)
        s.keys('M-3', 'C-c', 't')
        self.assertEqual(s.text(), 'a\u65e5!!!z')
        s.keys('C-/')
        self.assertEqual(s.text(), '')

    def test_malformed_protocol_and_control_character_rendering(self):
        s = self.session()
        s.request('unknown', expected=False)
        s.request('key', key=1, expected=False)
        s.request('point', byte=-1, expected=False)
        s.request('insert', text='a\x00b', expected=False)
        s.request('command', name='kill-emacs\x00ignored', expected=False)
        r = s.request('snapshot', rows=10**15, cols=-1, id='correlation')
        self.assertEqual(r['id'], 'correlation')
        self.assertEqual(r['state']['rows'], 200)
        self.assertEqual(r['state']['cols'], 12)
        s.request('insert', text='a\x1b[31mb')
        rendered = ''.join(line['text'] for line in s.pane['lines'])
        self.assertNotIn('\x1b', rendered)
        self.assertIn('^[', rendered)

    def test_long_line_navigation_and_edits(self):
        s = self.session()
        text = 'a'*20000 + '\t' + '\u65e5'*50 + '\nshort'
        s.set(text, 19000)
        s.keys('C-f', 'x', 'C-f')
        self.assertEqual(s.pane['cursor_col'], s.pane['width'] - 1)
        s.request('point', byte=0)
        s.keys('x', 'C-/')
        s.command('goto-char', '19001')
        self.assertEqual(s.pane['point'], 19000)
        self.assertEqual(s.text(), text[:19001]+'x'+text[19001:])

    def fake_formatters(self, isort_body, autopep8_body):
        directory = self.directory / 'bin'
        directory.mkdir(exist_ok=True)
        for name, body in (('isort', isort_body), ('autopep8', autopep8_body)):
            if body is None:
                continue
            path = directory / name
            path.write_text('#!/usr/bin/python3\nimport sys, os, pathlib, time\np=pathlib.Path(sys.argv[-1])\n'+body+'\n')
            path.chmod(0o755)
        return dict(os.environ, PATH=str(directory))

    def test_formatter_success_exact_flags_and_single_undo(self):
        target = self.directory / 'example.py'
        target.write_text('import z\nimport a\nx=1\n')
        env = self.fake_formatters(
            "assert sys.argv[1] == '--quiet'\nassert p.parent == pathlib.Path.cwd()\nassert p.suffix == '.py'\np.write_text('import a\\nimport z\\nx=1\\n')",
            "assert sys.argv[1:3] == ['--in-place', '--ignore=E501']\np.write_text(p.read_text().replace('x=1', 'x = 1'))")
        s = self.session('--config', PROFILE, target, env=env)
        s.keys('C-c', 'C-r')
        self.assertEqual(s.text(), 'import a\nimport z\nx = 1\n')
        self.assertEqual(target.read_text(), 'import z\nimport a\nx=1\n')
        s.keys('C-/')
        self.assertEqual(s.text(), target.read_text())
        self.assertFalse(s.pane['modified'])
        self.assertEqual(list(self.directory.glob('cpymacs-python-format-*')), [])

    def test_formatter_failure_rolls_back_and_cleans_up(self):
        target = self.directory / 'example.pyx'
        target.write_text('original\n')
        env = self.fake_formatters("assert p.suffix == '.pyx'\np.write_text('stage one\\n')", "print('formatter failed')\nsys.exit(7)")
        s = self.session('--config', PROFILE, target, env=env)
        r = s.command('python-format-buffer', expected=False)
        self.assertIn('exit code 7', r['error'])
        self.assertEqual(s.text(), 'original\n')
        s.command('switch-to-buffer', '*Python formatter*')
        self.assertIn('formatter failed', s.text())
        self.assertEqual(list(self.directory.glob('cpymacs-python-format-*')), [])

    def test_missing_formatters_are_silently_skipped(self):
        env = self.fake_formatters(None, None)
        s = self.session('--config', PROFILE, self.directory / 'example.py', env=env)
        s.set('x=1')
        s.keys('C-c', 'C-r')
        self.assertEqual(s.text(), 'x=1')

    def test_formatter_timeout_preserves_buffer(self):
        env = self.fake_formatters('time.sleep(2)', None)
        config = self.config("from cpymacs_plugins import dotemacs, python_format\nfrom cpymacs import api\ndotemacs.install(api)\npython_format.TIMEOUT_SECONDS = 0.05\n")
        s = self.session('--config', config, self.directory / 'example.py', env=env)
        s.set('x=1')
        s.command('python-format-buffer', expected=False)
        self.assertEqual(s.text(), 'x=1')
        self.assertEqual(list(self.directory.glob('cpymacs-python-format-*')), [])

    def test_formatter_invalid_output_is_rejected(self):
        env = self.fake_formatters("p.write_bytes(b'\\xff')", None)
        s = self.session('--config', PROFILE, self.directory / 'example.py', env=env)
        s.set('x=1')
        s.command('python-format-buffer', expected=False)
        self.assertEqual(s.text(), 'x=1')

    def test_private_server_shared_session_and_detach(self):
        path = self.directory / 'editor.sock'
        errors = tempfile.TemporaryFile()
        server = subprocess.Popen([BINARY, '--server', str(path), '--no-user-config'], stderr=errors)
        sockets = []
        try:
            deadline = time.monotonic()+5
            while not path.exists() and time.monotonic()<deadline and server.poll() is None:
                time.sleep(0.01)
            self.assertTrue(path.exists())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            def client():
                sock = socket.socket(socket.AF_UNIX)
                sock.settimeout(5)
                sock.connect(str(path))
                stream = sock.makefile('rwb')
                sockets.append((sock, stream))
                return stream
            def request(stream, op, **kw):
                stream.write(json.dumps({'op':op, **kw}).encode()+b'\n')
                stream.flush()
                return json.loads(stream.readline())
            a, b = client(), client()
            self.assertTrue(request(a, 'insert', text='shared')['ok'])
            self.assertEqual(request(b, 'get_text', no_snapshot=True)['result'], 'shared')
            self.assertTrue(request(a, 'command', name='detach-client')['state']['detach'])
            self.assertFalse(request(b, 'snapshot')['state']['detach'])
            self.assertEqual(request(b, 'get_text', no_snapshot=True)['result'], 'shared')
            request(b, 'quit', force=True)
            self.assertEqual(server.wait(timeout=5), 0)
            self.assertFalse(path.exists())
        finally:
            for sock, stream in sockets:
                stream.close()
                sock.close()
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
            errors.seek(0)
            diagnostic = errors.read().decode(errors='replace')
            errors.close()
            self.assertNotIn('AddressSanitizer', diagnostic)
            self.assertNotIn('runtime error:', diagnostic)


if __name__ == '__main__':
    unittest.main(verbosity=2)
