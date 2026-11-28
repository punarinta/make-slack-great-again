#!/usr/bin/env python3
"""A fake AI provider for the LLM tests (python3 stdlib only).

    fake_llm.py [--port N]

Prints `PORT <n>` and `READY`, then serves until stdin closes. It speaks
both wires msga uses, so a provider pointed at http://127.0.0.1:<n> (the
Anthropic wire) or http://127.0.0.1:<n>/v1 (OpenAI-compatible) gets answers.
Nothing here ever reaches a real provider.

    POST /v1/messages          Anthropic Messages: {"content":[{"type":"text",…}]}
    POST /v1/chat/completions  OpenAI chat: {"choices":[{"message":{…}}]}
    GET  /v1/models            {"data":[{"id":"fake-small"},{"id":"fake-large"}]}
    POST /v1/audio/transcriptions  speech-to-text: {"text": …} (multipart in)
    POST /_ctl/script          {"path": p, "responses": [answer, ...]} queues
                               answers for a path; an answer is a JSON object
                               sent as is, and may carry "__status" (HTTP
                               status), "__delay" (seconds) and "__raw" (a
                               body string sent instead of the object)
    POST /_ctl/reset           forget the log and the scripts
    GET  /_ctl/log             every request so far: [{method, path, headers, body}]
                               (header names lower case, body as text)
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

lock = threading.Lock()
state = {'log': [], 'scripts': {}}

DEFAULT_TEXT = 'The team agreed to **ship on Friday**.'
STT_TEXT = 'um so the deploy is uh ready'


def default_answer(method, path):
    if method == 'POST' and path == '/v1/messages':
        return {'id': 'msg_1', 'type': 'message', 'model': 'fake-anthropic',
                'stop_reason': 'end_turn',
                'content': [{'type': 'text', 'text': DEFAULT_TEXT}]}
    if method == 'POST' and path.endswith('/chat/completions'):
        return {'id': 'c1', 'model': 'fake-openai',
                'choices': [{'index': 0, 'finish_reason': 'stop',
                             'message': {'role': 'assistant', 'content': DEFAULT_TEXT}}]}
    if method == 'POST' and path.endswith('/audio/transcriptions'):
        return {'text': STT_TEXT}
    if method == 'GET' and path.endswith('/models'):
        return {'data': [{'id': 'fake-small'}, {'id': 'fake-large'}]}
    return {'__status': 404, '__raw': 'not found'}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def answer(self, status, body, ctype='application/json'):
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, method):
        length = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(length).decode('utf-8', 'replace') if length else ''
        path = self.path.split('?')[0]
        if path == '/_ctl/reset':
            with lock:
                state['log'].clear()
                state['scripts'].clear()
            return self.answer(200, '{}')
        if path == '/_ctl/script':
            req = json.loads(body)
            with lock:
                state['scripts'].setdefault(req['path'], []).extend(req['responses'])
            return self.answer(200, '{}')
        if path == '/_ctl/log':
            with lock:
                return self.answer(200, json.dumps(state['log']))
        with lock:
            state['log'].append({'method': method, 'path': path, 'body': body,
                                 'headers': {k.lower(): v for k, v in self.headers.items()}})
            queue = state['scripts'].get(path)
            reply = queue.pop(0) if queue else default_answer(method, path)
        reply = dict(reply)
        status = reply.pop('__status', 200)
        delay = reply.pop('__delay', 0)
        raw = reply.pop('__raw', None)
        if delay:
            time.sleep(delay)
        if raw is not None:
            return self.answer(status, raw, 'text/plain')
        return self.answer(status, json.dumps(reply))

    def do_GET(self):
        self.handle_any('GET')

    def do_POST(self):
        self.handle_any('POST')


def main():
    port = 0
    if '--port' in sys.argv:
        port = int(sys.argv[sys.argv.index('--port') + 1])
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f'PORT {server.server_address[1]}', flush=True)
    print('READY', flush=True)
    sys.stdin.read()  # the test closes our stdin when it is done
    server.shutdown()


if __name__ == '__main__':
    main()
