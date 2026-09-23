#!/usr/bin/env python3
"""Exercise native GUI/X11 and terminal/PTY frontends, not UI mocks."""
from __future__ import annotations
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
DRIVER = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv)>1 else None
ROOT = Path(__file__).resolve().parents[1]


def until(predicate, timeout=8):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if predicate():
            return
        time.sleep(0.03)
    raise AssertionError('Condition did not become true before timeout')


class FrontendTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='cpymacs-ui-')
        self.directory=Path(self.temp.name)
        self.processes=[]
        self.errors=tempfile.TemporaryFile()

    def tearDown(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait()
            if process.stdout:
                process.stdout.close()
        self.errors.seek(0)
        errors=self.errors.read().decode(errors='replace')
        self.errors.close()
        self.temp.cleanup()
        self.assertNotIn('AddressSanitizer',errors)
        self.assertNotIn('runtime error:',errors)

    def launch(self,args,**kwargs):
        process=subprocess.Popen(list(map(str,args)),stderr=self.errors,**kwargs)
        self.processes.append(process)
        return process

    def terminal(self,target):
        master,slave=pty.openpty()
        before=termios.tcgetattr(slave)
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',24,80,0,0))
        process=self.launch([BINARY,'--nox','--no-user-config',target],stdin=slave,stdout=slave,
                            env=dict(os.environ,TERM='xterm-256color'))
        output=bytearray()
        stop=threading.Event()
        def drain():
            while not stop.is_set():
                try:
                    if select.select([master],[],[],0.05)[0]:
                        chunk=os.read(master,65536)
                        if not chunk:return
                        output.extend(chunk)
                except OSError:return
        reader=threading.Thread(target=drain,daemon=True)
        reader.start()
        until(lambda:b'\x1b[?2004h' in output)
        return process,master,slave,before,output,stop,reader

    def test_terminal_keys_unicode_paste_save_and_restore(self):
        target=self.directory/'terminal.txt'
        p,master,slave,before,output,stop,reader=self.terminal(target)
        try:
            os.write(master,b'hello\r\tworld')
            os.write(master,b'\x1b[200~'+('\n\u65e5\u672c\u8a9e\n').encode()+b'\x1b[201~')
            os.write(master,b'\x18\x13')
            until(lambda:target.exists() and target.read_text()=='hello\n    world\n\u65e5\u672c\u8a9e\n')
            os.write(master,b'\x18\x03')
            self.assertEqual(p.wait(timeout=8),0)
            time.sleep(0.05)
            self.assertEqual(termios.tcgetattr(slave),before)
            self.assertIn(b'\x1b[?2004l',output)
        finally:
            stop.set();reader.join(timeout=2);os.close(master);os.close(slave)

    def test_terminal_signal_restores_terminal(self):
        p,master,slave,before,output,stop,reader=self.terminal(self.directory/'signal.txt')
        try:
            p.send_signal(signal.SIGTERM)
            self.assertEqual(p.wait(timeout=8),0)
            self.assertEqual(termios.tcgetattr(slave),before)
        finally:
            stop.set();reader.join(timeout=2);os.close(master);os.close(slave)

    @unittest.skipUnless(DRIVER and shutil.which('Xvfb'),'Xvfb and native driver are required')
    def test_gui_real_keys_clipboard_resize_mouse_detach_and_reattach(self):
        readfd,writefd=os.pipe()
        xvfb=self.launch(['Xvfb','-displayfd',str(writefd),'-screen','0','1200x800x24','-nolisten','tcp'],
                          pass_fds=(writefd,),stdout=subprocess.DEVNULL)
        os.close(writefd)
        with os.fdopen(readfd) as stream:
            display=':'+stream.readline().strip()
        self.assertNotEqual(display,':')
        env=dict(os.environ,DISPLAY=display,LC_ALL='C.UTF-8')
        sockpath=self.directory/'backend.sock'
        target=self.directory/'gui.py'
        server=self.launch([BINARY,'--server',sockpath,'--no-user-config','--config',ROOT/'examples/dotemacs.py',target])
        until(sockpath.exists)
        client=socket.socket(socket.AF_UNIX);client.settimeout(8);client.connect(str(sockpath));stream=client.makefile('rwb')
        def request(op,**kw):
            stream.write(json.dumps({'op':op,'no_snapshot':True,**kw}).encode()+b'\n');stream.flush()
            answer=json.loads(stream.readline())
            self.assertTrue(answer['ok'],answer)
            return answer
        def text():return request('get_text')['result']
        def drive(*args):return subprocess.run([DRIVER,*args],env=env,check=True,capture_output=True,timeout=10).stdout
        try:
            gui=self.launch([str(Path(BINARY).with_name('cpymacs-gui')),'--connect',sockpath],env=env)
            window=int(drive('wait').strip())
            time.sleep(0.5)
            drive('type','abc')
            drive('keys','C-a','C-f','X','C-x','C-s')
            until(lambda:target.exists() and target.read_text()=='aXbc')
            drive('keys','C-x','h','M-w')
            until(lambda:drive('read-clipboard')==b'aXbc')
            # Test the GUI-to-clipboard INCR path, larger than one X property.
            long_text='copy '*16000
            request('set_text',text=long_text)
            drive('keys','C-x','h','M-w')
            until(lambda:drive('read-clipboard')==long_text.encode())
            request('set_text',text='')
            request('point',byte=0)
            owner=self.launch([DRIVER,'serve-clipboard','external paste'],env=env,stdout=subprocess.PIPE)
            self.assertEqual(owner.stdout.readline(),b'ready\n')
            drive('keys','C-y')
            until(lambda:text()=='external paste')
            self.assertEqual(owner.wait(timeout=5),0)
            old_cols=request('snapshot',no_snapshot=False)['state']['cols']
            drive('resize','640','400')
            time.sleep(0.3)
            drive('click','1','1')
            time.sleep(0.1)
            state=request('snapshot',no_snapshot=False)['state']
            pane=next(p for p in state['panes'] if p['active'])
            self.assertEqual(pane['point'],0)
            self.assertLess(state['cols'],old_cols)
            # Optional screenshot is a real Xvfb rendering, not a mockup.
            screenshot=os.environ.get('CPYMACS_TEST_SCREENSHOT')
            if screenshot:
                drive('screenshot',screenshot)
            drive('keys','M-x')
            drive('type','detach-client')
            drive('keys','RET')
            self.assertEqual(gui.wait(timeout=8),0)
            self.assertEqual(text(),'external paste')
            second=self.launch([str(Path(BINARY).with_name('cpymacs-gui')),'--connect',sockpath],env=env)
            drive('wait');time.sleep(0.2)
            drive('keys','M-x');drive('type','detach-client');drive('keys','RET')
            self.assertEqual(second.wait(timeout=8),0)
            self.assertEqual(text(),'external paste')
            request('quit',force=True)
            self.assertEqual(server.wait(timeout=8),0)
        finally:
            stream.close();client.close()


if __name__=='__main__':unittest.main(verbosity=2)
