#!/usr/bin/env python3
"""A fake Slack Web API for msga_app_tests (python3 stdlib only).

    fake_slack.py [--port N]

Prints `PORT <n>` and `READY`, then serves until stdin closes. Point
MSGA_SLACK_API_BASE at http://127.0.0.1:<n>/api/ and SlackBackend talks to
it. Nothing here ever reaches the real Slack.

    POST /api/<method>      a Web API call: answered from the method's script
                            queue if one is queued, else by a default handler
                            (chat.postMessage keeps a per-channel history that
                            conversations.history / .replies serve, with a
                            post's metadata under include_all_metadata=true)
    POST /upload/<file_id>  the pre-signed upload URL files.getUploadURLExternal
                            hands out
    POST /_ctl/script       {"method": m, "responses": [answer, ...]} queues
                            answers; an answer is a JSON object sent as is, or
                            carries "__partial": true (send half a status line,
                            then close: the response is lost) and optionally
                            "__deliver": true (do the default handler's side
                            effects first, as if the call landed)
    POST /_ctl/reset        forget the log, the scripts and the history
    POST /_ctl/set          {key: answer-or-list, ...} merged into a table of
                            standing answers (the read-side tests' workspace):
                            a call takes the first matching key of
                            "<method>?<field>=<value>" (any form field, e.g.
                            "users.list?cursor=p2"), else "<method>"; each call
                            takes the first answer of the list, the last one
                            sticks. Scripts win over the table, the table over
                            the defaults. Any answer may carry "__status" (HTTP
                            status), "__headers" ({name: value}), "__delay"
                            (seconds to wait before answering) and "__raw" (a
                            text/html body sent instead of the JSON: a
                            gateway's error page).
    GET  /_ctl/log          every request so far: [{method, form, auth, cookie,
                            ctype, len}] (an upload's also: body, its first
                            256 bytes)
    GET  /files-pri/<path>  a file's url_private (logged as method "download"):
                            a canvas's HTML for .../canvas; .../<name>.bin: bytes
                            0..255 four times; .../missing: a 404; else the bytes
                            "bytes of <last path segment>"
    GET  /update/<v>/<sha>.manifest  the updater's manifest: {"version": v,
                            "sha256": sha} (sha "-" = none)

WebSockets (Socket Mode and the RTM presence link):

    GET  /ws/sm-<n>         a Socket Mode socket (apps.connections.open hands
                            these out): sends hello with num_connections = the
                            open sm sockets + state "ws_extra"; every text frame
                            received (the envelope acks) is logged as method
                            "ws:sm" with form {conn, text}
    GET  /ws/rtm-<n>        an RTM socket (rtm.connect): hello (or, without a
                            `d` cookie on the handshake, an invalid_auth error
                            frame); answers {"type":"ping"} with a pong unless
                            "rtm_nopong" is set; frames logged as "ws:rtm"
                            Every handshake is logged as "ws-open" {conn} with
                            its cookie, every client WebSocket ping (control
                            frame, answered with a pong) as "ws-ping" {conn}.
    POST /_ctl/ws/send      {"conn": id (default: the newest open sm socket),
                            "text": frame} — a server frame
    POST /_ctl/ws/close     {"conn": id (default: newest open sm), "code": n}
    POST /_ctl/ws/opt       {"ws_extra": n, "rtm_nopong": bool}
    GET  /_ctl/ws           [{conn, open}] of every socket so far
"""
import base64
import hashlib
import json
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

ME = 'UME'
lock = threading.Lock()
state = {}


WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
sockets = {}  # conn id → Handler (open or not), kept across resets
ws_no = [0]


def reset():
    state.clear()
    state.update(log=[], scripts={}, table={}, history={}, last_ts=0.0, file_no=0,
                 ws_extra=0, rtm_nopong=False)
    for h in list(sockets.values()):
        h.ws_close(1001)
    sockets.clear()


def ws_url(handler, kind):
    ws_no[0] += 1
    return 'ws://127.0.0.1:%d/ws/%s-%d' % (handler.server.server_address[1], kind, ws_no[0])


def ws_target(conn):
    if conn:
        return sockets.get(conn)
    live = [h for h in sockets.values() if h.ws_open and h.ws_id.startswith('sm-')]
    return live[-1] if live else None


reset()


def next_ts():
    t = max(time.time(), state['last_ts'] + 0.000001)
    state['last_ts'] = t
    return '%.6f' % t


def escape(text):  # how Slack stores bare & < >
    return text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def post_message(form):
    ch = form.get('channel', '')
    msg = {'type': 'message', 'user': ME, 'text': escape(form.get('text', '')),
           'ts': next_ts()}
    if form.get('thread_ts'):
        msg['thread_ts'] = form['thread_ts']
    if form.get('metadata'):
        msg['metadata'] = json.loads(form['metadata'])
    state['history'].setdefault(ch, []).append(msg)
    return {'ok': True, 'channel': ch, 'ts': msg['ts'], 'message': without_metadata(msg)}


def without_metadata(msg):  # Slack lists metadata only when include_all_metadata asks
    return {k: v for k, v in msg.items() if k != 'metadata'}


def history(form, replies):
    msgs = state['history'].get(form.get('channel', ''), [])
    oldest = float(form.get('oldest', '0') or 0)
    if replies:
        root = form.get('ts', '')
        msgs = [m for m in msgs if m['ts'] == root or m.get('thread_ts') == root]
    else:
        msgs = [m for m in msgs if not m.get('thread_ts')]
    msgs = [m for m in msgs if float(m['ts']) > oldest]
    if form.get('include_all_metadata') != 'true':
        msgs = [without_metadata(m) for m in msgs]
    return {'ok': True, 'messages': sorted(msgs, key=lambda m: m['ts'], reverse=not replies)}


def upload_url(handler, form):
    state['file_no'] += 1
    fid = 'F%04d' % state['file_no']
    port = handler.server.server_address[1]
    return {'ok': True, 'file_id': fid,
            'upload_url': 'http://127.0.0.1:%d/upload/%s' % (port, fid)}


def complete_upload(form):
    files = json.loads(form.get('files', '[]'))
    msg = {'type': 'message', 'user': ME, 'text': escape(form.get('initial_comment', '')),
           'ts': next_ts(), 'files': [{'id': f['id'], 'name': f.get('title', '')} for f in files]}
    if form.get('thread_ts'):
        msg['thread_ts'] = form['thread_ts']
    state['history'].setdefault(form.get('channel_id', ''), []).append(msg)
    return {'ok': True, 'files': [{'id': f['id']} for f in files]}


def members(form):
    if form.get('cursor') == 'page2':
        return {'ok': True, 'members': ['U3'], 'response_metadata': {'next_cursor': ''}}
    return {'ok': True, 'members': ['U1', 'U2'], 'response_metadata': {'next_cursor': 'page2'}}


DEFAULTS = {
    'chat.postMessage': lambda h, f: post_message(f),
    'conversations.history': lambda h, f: history(f, False),
    'conversations.replies': lambda h, f: history(f, True),
    'files.getUploadURLExternal': upload_url,
    'files.completeUploadExternal': lambda h, f: complete_upload(f),
    'conversations.members': lambda h, f: members(f),
    'conversations.open': lambda h, f: {'ok': True, 'channel': {'id': 'D0NEW'}},
    'users.profile.get': lambda h, f: {'ok': True, 'profile': {
        'display_name': 'Me', 'real_name': 'Me Myself', 'email': 'me@example.com',
        'phone': '123', 'image_192': 'https://img/192.png'}},
    'users.setPhoto': lambda h, f: {'ok': True, 'profile': {'image_512': 'https://img/new512.png'}},
    'search.messages': lambda h, f: {'ok': True, 'messages': {'matches': []}},
    'apps.connections.open': lambda h, f: {'ok': True, 'url': ws_url(h, 'sm')},
    'rtm.connect': lambda h, f: {'ok': True, 'url': ws_url(h, 'rtm')},
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def send_json(self, obj, status=200):
        headers = {}
        if '__status' in obj or '__headers' in obj or '__raw' in obj:
            obj = dict(obj)
            status = obj.pop('__status', status)
            headers = obj.pop('__headers', {})
        raw = obj.pop('__raw', None) if '__raw' in obj else None
        body = raw.encode() if raw is not None else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'text/html' if raw is not None else 'application/json')
        self.send_header('Content-Length', str(len(body)))
        for k, v in headers.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get('Content-Length', '0'))
        return self.rfile.read(n) if n else b''

    # ── WebSockets ──────────────────────────────────────────────────────
    ws_open = False
    ws_id = ''

    def websocket(self):
        key = self.headers.get('Sec-WebSocket-Key', '')
        self.ws_id = self.path[len('/ws/'):]
        self.ws_lock = threading.Lock()
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        rtm = self.ws_id.startswith('rtm-')
        cookie = self.headers.get('Cookie', '')
        with lock:
            state['log'].append({'method': 'ws-open', 'form': {'conn': self.ws_id},
                                 'auth': self.headers.get('Authorization', ''),
                                 'cookie': cookie, 'ctype': '', 'len': 0})
            self.ws_open = True
            sockets[self.ws_id] = self
            n = len([h for h in sockets.values() if h.ws_open and h.ws_id.startswith('sm-')])
            extra = state['ws_extra']
        if rtm and not cookie.startswith('d='):
            self.ws_text({'type': 'error', 'error': {'msg': 'invalid_auth', 'code': 401}})
        elif rtm:
            self.ws_text({'type': 'hello'})
        else:
            self.ws_text({'type': 'hello', 'num_connections': n + extra,
                          'debug_info': {'host': 'fake'}})
        try:
            while True:
                fr = self.ws_read()
                if fr is None:
                    break
                fin, op, payload = fr
                if op == 8:
                    self.ws_close(struct.unpack('!H', payload[:2])[0] if len(payload) >= 2
                                  else 1000)
                    break
                if op == 9:
                    with lock:
                        state['log'].append({'method': 'ws-ping', 'form': {'conn': self.ws_id},
                                             'auth': '', 'cookie': '', 'ctype': '', 'len': 0})
                    self.ws_send(10, payload)
                    continue
                if op != 1:
                    continue
                text = payload.decode()
                with lock:
                    state['log'].append({'method': 'ws:rtm' if rtm else 'ws:sm',
                                         'form': {'conn': self.ws_id, 'text': text},
                                         'auth': '', 'cookie': '', 'ctype': '', 'len': len(text)})
                    nopong = state['rtm_nopong']
                if rtm and not nopong:
                    try:
                        frame = json.loads(text)
                    except ValueError:
                        frame = {}
                    if frame.get('type') == 'ping':
                        self.ws_text({'type': 'pong', 'reply_to': frame.get('id')})
        except (OSError, ValueError):
            pass
        self.ws_open = False

    def ws_text(self, obj):
        self.ws_send(1, json.dumps(obj).encode())

    def ws_close(self, code):
        if not self.ws_open:
            return
        self.ws_open = False
        try:
            self.ws_send(8, struct.pack('!H', code))
        except (OSError, ValueError):  # the client is gone already
            pass

    def ws_read(self):
        def exact(n):
            data = b''
            while len(data) < n:
                chunk = self.rfile.read(n - len(data))
                if not chunk:
                    return None
                data += chunk
            return data
        h = exact(2)
        if h is None:
            return None
        fin, op, n = h[0] & 0x80, h[0] & 0x0f, h[1] & 0x7f
        if n == 126:
            n = struct.unpack('!H', exact(2))[0]
        elif n == 127:
            n = struct.unpack('!Q', exact(8))[0]
        mask = exact(4) if h[1] & 0x80 else b'\0\0\0\0'
        data = bytearray(exact(n) or b'')
        for i in range(len(data)):
            data[i] ^= mask[i & 3]
        return bool(fin), op, bytes(data)

    def ws_send(self, op, payload):
        n = len(payload)
        head = bytes([0x80 | op])
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126]) + struct.pack('!H', n)
        else:
            head += bytes([127]) + struct.pack('!Q', n)
        with self.ws_lock:
            self.wfile.write(head + payload)
            self.wfile.flush()

    def do_GET(self):
        if self.path.startswith('/ws/'):
            return self.websocket()
        if self.path == '/_ctl/ws':
            with lock:
                self.send_json([{'conn': k, 'open': h.ws_open} for k, h in sockets.items()])
            return
        if self.path == '/_ctl/log':
            with lock:
                self.send_json(state['log'])
        elif self.path.startswith('/update/'):  # /update/<version>/<sha256>.manifest
            version, sha = self.path[len('/update/'):].rsplit('.', 1)[0].split('/', 1)
            self.send_json({'version': int(version), 'updatedAt': '2026-10-01T00:00:00Z',
                            'sha256': '' if sha == '-' else sha})
        elif self.path.startswith('/files-pri/'):
            with lock:
                state['log'].append({'method': 'download', 'form': {'path': self.path},
                                     'auth': self.headers.get('Authorization', ''),
                                     'cookie': self.headers.get('Cookie', ''),
                                     'ctype': '', 'len': 0})
            if self.path.endswith('/missing'):
                self.send_json({'ok': False, 'error': 'file_not_found'}, 404)
                return
            ctype = 'text/html'
            body = b'<div class="quip-canvas-content"><h1 id="t">Plan</h1><p id="a">Hello</p></div>'
            if self.path.endswith('.bin'):  # every byte value, 4 times
                ctype, body = 'application/octet-stream', bytes(range(256)) * 4
            elif not self.path.endswith('/canvas'):
                ctype = 'application/octet-stream'
                body = ('bytes of %s' % self.path.rsplit('/', 1)[-1]).encode()
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_json({'ok': False, 'error': 'not_found'}, 404)

    def do_POST(self):
        raw = self.body()
        path = self.path.split('?')[0]
        with lock:
            if path == '/_ctl/reset':
                reset()
                return self.send_json({'ok': True})
            if path == '/_ctl/script':
                s = json.loads(raw)
                state['scripts'].setdefault(s['method'], []).extend(s['responses'])
                return self.send_json({'ok': True})
            if path == '/_ctl/set':
                for k, v in json.loads(raw).items():
                    state['table'][k] = v if isinstance(v, list) else [v]
                return self.send_json({'ok': True})
            if path == '/_ctl/ws/opt':
                state.update(json.loads(raw))
                return self.send_json({'ok': True})
            if path in ('/_ctl/ws/send', '/_ctl/ws/close'):
                req = json.loads(raw)
                h = ws_target(req.get('conn'))
                if not h or not h.ws_open:
                    return self.send_json({'ok': False, 'error': 'no_socket'})
                if path.endswith('/send'):
                    h.ws_send(1, req['text'].encode())
                else:
                    h.ws_close(req.get('code', 1000))
                return self.send_json({'ok': True, 'conn': h.ws_id})
            ctype = self.headers.get('Content-Type', '')
            form = {}
            if ctype.startswith('application/x-www-form-urlencoded'):
                form = {k: v[0] for k, v in parse_qs(raw.decode(), keep_blank_values=True).items()}
            elif ctype.startswith('multipart/form-data'):
                text = raw.decode('latin-1')
                at = text.find('filename="')
                if at >= 0:
                    form['filename'] = text[at + 10:text.find('"', at + 10)]
                form['field'] = 'image' if 'name="image"' in text else ''
            if path.startswith('/upload/'):
                method = 'upload'
                form['file_id'] = path[len('/upload/'):]
            elif path.startswith('/api/'):
                method = path[len('/api/'):]
            else:
                return self.send_json({'ok': False, 'error': 'not_found'}, 404)
            state['log'].append({'method': method, 'form': form,
                                 'auth': self.headers.get('Authorization', ''),
                                 'cookie': self.headers.get('Cookie', ''),
                                 'ctype': ctype, 'len': len(raw)})
            if method == 'upload':
                state['log'][-1]['body'] = raw[:256].decode('latin-1')
                body = ('OK - %d' % len(raw)).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                return self.wfile.write(body)
            queue = state['scripts'].get(method, [])
            answer = queue.pop(0) if queue else None
            default = DEFAULTS.get(method, lambda h, f: {'ok': True})
            if answer is None:
                standing = None
                for k, v in form.items():
                    standing = state['table'].get('%s?%s=%s' % (method, k, v))
                    if standing:
                        break
                standing = standing or state['table'].get(method)
                if standing:
                    answer = standing[0] if len(standing) == 1 else standing.pop(0)
            if answer is None:
                return self.send_json(default(self, form))
            if answer.get('__partial'):
                if answer.get('__deliver'):
                    default(self, form)
                # Some bytes of an answer, then the connection dies: the
                # client can't tell whether the call landed (and must not
                # silently resend it).
                self.wfile.write(b'HTTP/1.1 200')
                self.wfile.flush()
                self.close_connection = True
                return
            delay = answer.get('__delay')
            if delay:
                # A slow Slack: this answer waits, the others don't.
                answer = {k: v for k, v in answer.items() if k != '__delay'}
                lock.release()
                try:
                    time.sleep(delay)
                finally:
                    lock.acquire()
            return self.send_json(answer)


class QuietServer(ThreadingHTTPServer):
    # A client that goes away mid-answer (a backend destroyed with calls in
    # flight cancels them) is normal here, not a traceback.
    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            super().handle_error(request, client_address)


def main():
    port = 0
    if '--port' in sys.argv:
        port = int(sys.argv[sys.argv.index('--port') + 1])
    srv = QuietServer(('127.0.0.1', port), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print('PORT %d' % srv.server_address[1])
    print('READY', flush=True)
    sys.stdin.read()  # until the test closes our stdin (or exits)


if __name__ == '__main__':
    main()
