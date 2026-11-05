#!/usr/bin/env python3
"""kopi.27: the Wi-Fi file transfer server (webfiles.c) end to end over loopback: login, list, upload (nested,
large, cut off), download (whole and ranged), rename, new folder, delete, and the path guards."""
import os, shutil, subprocess, sys, tempfile, http.client, urllib.parse, socket, json
HERE = os.path.dirname(os.path.abspath(__file__))
root = tempfile.mkdtemp(prefix='wfroot')
os.makedirs(root + '/Music/Old'); open(root + '/Music/a.mp3', 'wb').write(b'x' * 1000)
os.makedirs(root + '/.diskos'); open(root + '/.diskos/secret', 'w').write('s')
outside = tempfile.mkdtemp(prefix='wfout'); open(outside + '/o.txt', 'w').write('no')
os.symlink(outside, root + '/Music/link')
exe = os.path.join(tempfile.mkdtemp(), 'wft')
cc = os.environ.get('CC', 'cc')
extra = ['-static'] if os.environ.get('RUNNER') else []    # e.g. CC=<mipsel cc> RUNNER="qemu-mipsel -cpu P5600": the device build
subprocess.check_call([cc, *extra, '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-pthread', '-D_GNU_SOURCE', '-DWF_ROOT="%s"' % root, '-DWF_IFACE="lo"',
                       '-o', exe, os.path.join(HERE, 'webfiles_test.c')])
p = subprocess.Popen(os.environ.get('RUNNER', '').split() + [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
port, pin = p.stdout.readline().split()
port = int(port)
fails = 0
def ok(c, m):
    global fails
    if not c: fails += 1; print('FAIL', m)
def req(m, path, body=None, cookie=None, hdr=None):
    c = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    h = dict(hdr or {})
    if cookie: h['Cookie'] = cookie
    c.request(m, path, body=body, headers=h); r = c.getresponse(); d = r.read(); c.close()
    return r.status, d, r
q = urllib.parse.quote
try:
    st, d, _ = req('GET', '/'); ok(st == 200 and b'Disc files' in d, 'page')
    st, _, _ = req('GET', '/api/ls?p=/'); ok(st == 401, 'list needs the code')
    st, _, _ = req('POST', '/api/login?pin=' + ('0000' if pin != '0000' else '1111')); ok(st == 403, 'wrong code')
    st, _, r = req('POST', '/api/login?pin=' + pin); ok(st == 200, 'login')
    ck = r.getheader('Set-Cookie').split(';')[0]
    st, d, _ = req('GET', '/api/ls?p=/Music', cookie=ck); j = json.loads(d)
    names = sorted(i['n'] for i in j['items']); ok(st == 200 and 'a.mp3' in names and 'Old' in names, 'list %s' % names)
    st, d, _ = req('GET', '/api/ls?p=/', cookie=ck); ok('.diskos' not in d.decode(), 'hidden not listed')
    # uploads: nested folders made, big one, unicode name
    blob = os.urandom(3 * 1024 * 1024 + 17)
    st, _, _ = req('PUT', '/f' + q('/Music/New Album/01 Sång.flac'), body=blob, cookie=ck); ok(st == 201, 'upload nested %d' % st)
    ok(open(root + '/Music/New Album/01 Sång.flac', 'rb').read() == blob, 'upload bytes')
    ok(not [f for f in os.listdir(root + '/Music/New Album') if f.endswith('.part')], 'no part left')
    # cut-off upload leaves nothing
    s = socket.create_connection(('127.0.0.1', port))
    s.sendall(('PUT /f/Music/cut.mp3 HTTP/1.1\r\nHost: x\r\nCookie: %s\r\nContent-Length: 100000\r\n\r\n' % ck).encode() + b'y' * 5000); s.close()
    import time; time.sleep(0.5)
    ok(not os.path.exists(root + '/Music/cut.mp3') and not [f for f in os.listdir(root + '/Music') if f.endswith('.part')], 'cut upload cleaned')
    # downloads
    st, d, r = req('GET', '/f' + q('/Music/New Album/01 Sång.flac') + '?dl=1', cookie=ck)
    ok(st == 200 and d == blob and 'attachment' in r.getheader('Content-Disposition'), 'download')
    st, d, r = req('GET', '/f/Music/a.mp3', cookie=ck, hdr={'Range': 'bytes=100-199'})
    ok(st == 206 and len(d) == 100 and r.getheader('Content-Range') == 'bytes 100-199/1000', 'range')
    st, d, r = req('GET', '/f/Music/a.mp3', cookie=ck, hdr={'Range': 'bytes=900-'}); ok(st == 206 and len(d) == 100, 'open range')
    st, d, r = req('HEAD', '/f/Music/a.mp3', cookie=ck); ok(st == 200 and d == b'', 'head')
    # guards
    for bad in ['/f/../etc/passwd', '/f/Music/../../x', '/f/.diskos/secret', '/f/Music/link/o.txt', '/f/Music/%2e%2e/%2e%2e/x']:
        st, d, _ = req('GET', bad, cookie=ck); ok(st == 404, 'guard get ' + bad)
    st, _, _ = req('PUT', '/f/Music/link/evil.txt', body=b'e', cookie=ck); ok(st == 400 and not os.path.exists(outside + '/evil.txt'), 'guard put through link')
    st, _, _ = req('PUT', '/f/.diskos/x', body=b'e', cookie=ck); ok(st == 400, 'guard put hidden')
    st, _, _ = req('DELETE', '/f/', cookie=ck); ok(st == 404 and os.path.isdir(root), 'no deleting the card')
    st, _, _ = req('DELETE', '/f/Music/link', cookie=ck); ok(os.path.exists(outside + '/o.txt'), 'delete link does not follow')
    st, _, _ = req('GET', '/f/Music/a.mp3'); ok(st == 401, 'download needs the code')
    # folder, rename, delete
    st, _, _ = req('POST', '/api/mkdir?p=' + q('/Podcasts/2026'), cookie=ck); ok(st == 200 and os.path.isdir(root + '/Podcasts/2026'), 'mkdir')
    st, _, _ = req('POST', '/api/mv?p=' + q('/Music/a.mp3') + '&to=' + q('/Music/b.mp3'), cookie=ck); ok(st == 200 and os.path.exists(root + '/Music/b.mp3'), 'rename')
    st, _, _ = req('POST', '/api/mv?p=' + q('/Music/b.mp3') + '&to=' + q('/Music/Old'), cookie=ck); ok(st == 409, 'rename onto existing')
    st, _, _ = req('DELETE', '/f' + q('/Music/New Album'), cookie=ck); ok(st == 200 and not os.path.exists(root + '/Music/New Album'), 'delete folder')
    p.stdin.write('c\n'); p.stdin.flush(); ok(p.stdout.readline().strip() == '1', 'changed flag')
    # lockout
    for i in range(12): req('POST', '/api/login?pin=x')
    st, _, _ = req('POST', '/api/login?pin=' + pin); ok(st == 429, 'locked after ten wrong codes')
finally:
    p.stdin.close(); p.wait(timeout=10)
    shutil.rmtree(root, ignore_errors=True); shutil.rmtree(outside, ignore_errors=True)
print('webfiles_test: %s' % ('OK' if not fails else '%d FAILED' % fails))
sys.exit(1 if fails else 0)
