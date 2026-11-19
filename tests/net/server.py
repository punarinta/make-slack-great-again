#!/usr/bin/env python3
"""Local HTTP + WebSocket server for msga_net_tests (python3 stdlib only).

    server.py [--tls [--tls-cert <cert.pem> <key.pem> <ca.pem>]] [--port N] [--host 127.0.0.1]

Prints, one per line, then serves until stdin closes (or it is killed):
    PORT <n>                  plain HTTP / ws:// server
    TLS <n> <ca-bundle>       with --tls and an `openssl` binary: HTTPS / wss://
                              with a fresh self-signed cert for "localhost" (no
                              IP SAN); <ca-bundle> = that cert + the system CAs
                              (--tls-cert: that certificate and key, and the
                              CA that signed it as <ca-bundle>)
    TLS none <why>            --tls asked, but no cert could be made
    TLS12 <n>                 with TLS: the same, capped at TLS 1.2
    READY

Run it by hand (e.g. `server.py --host 0.0.0.0 --port 8099`) and point
NET_TEST_URL=http://<host>:8099 at it to test a build on another machine
(Wine, the Mac). Endpoints: see Handler.do_GET.
"""
import base64
import hashlib
import json
import os
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


class Handler(BaseHTTPRequestHandler):
    swallowed = 0  # /post-swallow requests seen (the no-re-send test)
    protocol_version = 'HTTP/1.1'  # keep-alive unless we say otherwise

    def log_message(self, *args):  # quiet
        pass

    # ── helpers ──────────────────────────────────────────────────────────
    def send_body(self, body, status=200, ctype='text/plain', extra=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def redirect(self, location, status=302, extra=()):
        self.send_response(status)
        self.send_header('Location', location)
        self.send_header('Content-Length', '0')
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()

    def query(self):
        return {k: v[0] for k, v in parse_qs(urlsplit(self.path).query).items()}

    def read_body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n else b''

    def echo_headers(self):
        return {k.lower(): v for k, v in self.headers.items()}

    # ── endpoints ────────────────────────────────────────────────────────
    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = urlsplit(self.path).path
        q = self.query()
        if path == '/plain':
            self.send_body('hello world')
        elif path == '/headers':  # the request's headers as JSON
            self.send_body(json.dumps(self.echo_headers()), ctype='application/json')
        elif path == '/chunked' or path == '/chunked-ext':
            # -ext adds chunk extensions, which our client must skip (CFNetwork
            # rejects them, so the shared case has none).
            ext = b';ext=1' if path == '/chunked-ext' else b''
            self.send_response(200)
            self.send_header('Transfer-Encoding', 'chunked')
            self.send_header('Trailer', 'X-Check')
            self.end_headers()
            for part in (b'chunk one, ', b'chunk two, ', b'x' * 70000, b'end'):
                self.wfile.write(b'%x' % len(part) + ext + b'\r\n' + part + b'\r\n')
                self.wfile.flush()
            self.wfile.write(b'0\r\nX-Check: done\r\n\r\n')
        elif path == '/close':  # no length: the body runs until the close
            self.send_response(200)
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(b'until close ' + str(self.client_address[1]).encode())
            self.close_connection = True
        elif path == '/closehdr':  # a length, but Connection: close
            self.send_body(str(self.client_address[1]), extra=[('Connection', 'close')])
            self.close_connection = True
        elif path == '/tls-session':  # "reused" when the TLS handshake resumed a session
            reused = getattr(self.connection, 'session_reused', False)
            self.send_body('reused' if reused else 'new', extra=[('Connection', 'close')])
            self.close_connection = True
        elif path == '/keepalive':  # the client's port: same port = same connection
            self.send_body(str(self.client_address[1]))
        elif path == '/swallowed':  # how many /post-swallow requests arrived
            self.send_body(str(Handler.swallowed))
        elif path == '/drop':  # answers as keep-alive, then drops the connection
            self.send_body(str(self.client_address[1]))
            self.close_connection = True
        elif path == '/slow':
            time.sleep(int(q.get('ms', '1000')) / 1000)
            self.send_body('slow done')
        elif path == '/big':
            n = int(q.get('n', '1000000'))
            pattern = bytes(i % 251 for i in range(251))
            body = (pattern * (n // 251 + 1))[:n]
            spread = int(q.get('ms', '0'))
            if spread:
                # Over `ms` milliseconds in 20 pieces: a download that takes
                # a while even on loopback, so every transport reports
                # progress along the way (NSURLSession's is polled every
                # 250 ms).
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Length', str(n))
                self.end_headers()
                piece = (n + 19) // 20
                for i in range(0, n, piece):
                    self.wfile.write(body[i:i + piece])
                    self.wfile.flush()
                    time.sleep(spread / 20000)
            else:
                self.send_body(body, ctype='application/octet-stream')
        elif path == '/status':
            self.send_body('status', status=int(q.get('code', '200')))
        elif path == '/nocontent':
            self.send_response(204)
            self.end_headers()
        elif path == '/setcookies':
            self.send_response(200)
            self.send_header('Set-Cookie', 'a=1; Path=/')
            self.send_header('X-Mid', 'between')
            self.send_header('Set-Cookie', 'b=2; Path=/; HttpOnly')
            self.send_header('Content-Length', '0')
            self.end_headers()
        # Redirects: r1 sets a cookie and goes relative, r2 goes absolute.
        elif path == '/r1':
            self.redirect('r2', extra=[('Set-Cookie', 'hop=1; Path=/')])
        elif path == '/r2':
            self.redirect('http://%s/final' % self.headers['Host'])
        elif path == '/final':
            self.send_body(json.dumps(self.echo_headers()), ctype='application/json')
        elif path == '/to-localhost':  # another host name for the same (local) server
            self.redirect('http://localhost:%d/final' % self.server.server_address[1])
        elif path == '/same-host':
            self.redirect('/final')
        elif path == '/loop':
            self.redirect('/loop')
        elif path == '/ws' or path == '/ws-reject':
            if path == '/ws-reject':
                self.send_body('no', status=403)
            else:
                self.websocket()
        else:
            self.send_body('not found', status=404)

    def do_POST(self):
        path = urlsplit(self.path).path
        body = self.read_body()
        if path == '/echo':
            self.send_body(json.dumps({
                'method': self.command,
                'body': body.decode('latin-1'),
                'content-type': self.headers.get('Content-Type', ''),
                'content-length': self.headers.get('Content-Length'),
            }), ctype='application/json')
        elif path == '/post-redirect':
            self.redirect('/headers', status=302)
        elif path == '/post-307':  # the body must be sent again
            self.redirect('/echo', status=307)
        elif path == '/post-swallow':  # takes the POST, then closes without an answer
            Handler.swallowed += 1
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
        else:
            self.send_body('not found', status=404)

    do_PUT = do_POST

    # ── WebSocket echo ───────────────────────────────────────────────────
    # Text/binary messages are echoed, except these texts:
    #   "ping-me"      server pings; after the pong arrives it sends "pong ok"
    #   "fragment-me"  a text message in three frames with a ping in between
    #   "close-me"     the server closes with 4001 "bye"
    #   "headers"      the upgrade request's headers as JSON
    #   "pings"        the payloads of the client's pings so far, as JSON
    def websocket(self):
        key = self.headers.get('Sec-WebSocket-Key', '')
        if self.headers.get('Upgrade', '').lower() != 'websocket' or not key:
            self.send_body('not a websocket', status=400)
            return
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header('Upgrade', 'websocket')
        self.send_header('Connection', 'Upgrade')
        self.send_header('Sec-WebSocket-Accept', accept)
        self.end_headers()
        self.wfile.flush()
        upgrade_headers = self.echo_headers()
        self.close_connection = True
        msg, op0 = b'', 0
        pings = []
        while True:
            fr = self.ws_read()
            if fr is None:
                return
            fin, op, payload = fr
            if op == 8:  # close: echo the code back
                self.ws_send(8, payload[:2])
                return
            if op == 9:
                pings.append(payload.decode('latin-1'))
                self.ws_send(10, payload)
                continue
            if op == 10:
                if self.waiting_pong:
                    self.waiting_pong = False
                    self.ws_send(1, b'pong ok')
                continue
            if op != 0:
                msg, op0 = b'', op
            msg += payload
            if not fin:
                continue
            if op0 == 1 and msg == b'ping-me':
                self.waiting_pong = True
                self.ws_send(9, b'are you there')
            elif op0 == 1 and msg == b'fragment-me':
                self.ws_send(1, b'frag1', fin=False)
                self.ws_send(9, b'mid')
                self.ws_send(0, b'frag2', fin=False)
                self.ws_send(0, b'frag3', fin=True)
            elif op0 == 1 and msg == b'close-me':
                self.ws_send(8, struct.pack('!H', 4001) + b'bye')
                # wait for the client's answer, then hang up
                self.ws_read()
                return
            elif op0 == 1 and msg == b'pings':
                self.ws_send(1, json.dumps(pings).encode())
            elif op0 == 1 and msg == b'headers':
                self.ws_send(1, json.dumps(upgrade_headers).encode())
            else:
                self.ws_send(op0, msg)

    waiting_pong = False

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
        fin, op, masked, n = h[0] & 0x80, h[0] & 0x0f, h[1] & 0x80, h[1] & 0x7f
        if n == 126:
            n = struct.unpack('!H', exact(2))[0]
        elif n == 127:
            n = struct.unpack('!Q', exact(8))[0]
        if not masked:  # clients must mask
            raise ConnectionError('unmasked client frame')
        mask = exact(4)
        data = bytearray(exact(n) or b'')
        for i in range(len(data)):
            data[i] ^= mask[i & 3]
        return bool(fin), op, bytes(data)

    def ws_send(self, op, payload, fin=True):
        n = len(payload)
        head = bytes([(0x80 if fin else 0) | op])
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126]) + struct.pack('!H', n)
        else:
            head += bytes([127]) + struct.pack('!Q', n)
        self.wfile.write(head + payload)
        self.wfile.flush()


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # Clients hanging up mid-answer is what the cancel tests do.
        if not isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
            super().handle_error(request, client_address)


class TlsServer(Server):
    """Handshakes in the handler thread, so one stuck client can't block accept."""

    def __init__(self, addr, handler, context):
        self.context = context
        super().__init__(addr, handler)

    def finish_request(self, request, client_address):
        try:
            request = self.context.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            return  # e.g. the client refused our certificate
        self.RequestHandlerClass(request, client_address, self)


def make_cert(tmp):
    openssl = shutil.which('openssl')
    if not openssl:
        return None, 'no openssl'
    key, crt = os.path.join(tmp, 'key.pem'), os.path.join(tmp, 'cert.pem')
    r = subprocess.run([openssl, 'req', '-x509', '-newkey', 'ec', '-pkeyopt',
                        'ec_paramgen_curve:prime256v1', '-nodes', '-days', '2',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-addext', 'basicConstraints=critical,CA:TRUE',
                        '-keyout', key, '-out', crt],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        return None, 'openssl failed'
    bundle = os.path.join(tmp, 'bundle.pem')
    with open(bundle, 'wb') as out:
        out.write(open(crt, 'rb').read())
        for sys_bundle in ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt',
                           '/etc/ssl/ca-bundle.pem', '/etc/ssl/cert.pem'):
            if os.path.exists(sys_bundle):
                out.write(open(sys_bundle, 'rb').read())
                break
    return (key, crt, bundle), None


def main():
    args = sys.argv[1:]
    host = args[args.index('--host') + 1] if '--host' in args else '127.0.0.1'
    port = int(args[args.index('--port') + 1]) if '--port' in args else 0
    httpd = Server((host, port), Handler)
    print('PORT %d' % httpd.server_address[1], flush=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    tmp = None
    if '--tls' in args:
        if '--tls-cert' in args:  # a test PKI's certificate (PEOS's runner)
            at = args.index('--tls-cert')
            files, why = (args[at + 2], args[at + 1], args[at + 3]), None
        else:
            tmp = tempfile.mkdtemp(prefix='next-net-tls-')
            files, why = make_cert(tmp)
        if files:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(files[1], files[0])
            tlsd = TlsServer((host, 0), Handler, ctx)
            print('TLS %d %s' % (tlsd.server_address[1], files[2]), flush=True)
            threading.Thread(target=tlsd.serve_forever, daemon=True).start()
            ctx12 = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx12.maximum_version = ssl.TLSVersion.TLSv1_2
            ctx12.load_cert_chain(files[1], files[0])
            tls12d = TlsServer((host, 0), Handler, ctx12)
            print('TLS12 %d' % tls12d.server_address[1], flush=True)
            threading.Thread(target=tls12d.serve_forever, daemon=True).start()
        else:
            print('TLS none %s' % why, flush=True)
    print('READY', flush=True)
    try:
        if sys.stdin and not sys.stdin.isatty():
            sys.stdin.read()  # the test binary holds the other end: EOF = it is gone
        else:
            while True:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    os._exit(0)


if __name__ == '__main__':
    main()
