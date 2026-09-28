"""Exercise the real HTTP adapter/CLI against a loopback protocol fixture, not an LLM."""
import http.server
import json
import subprocess
import sys
import threading
from socketserver import TCPServer
import time


class Server(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path != '/v1/chat/completions' or request['stream'] is not False:
            self.send_error(400)
            return
        text = request['messages'][-1]['content']
        decisions = {
            'set via model': {'tool': 'ac.set_temperature', 'arguments': {'temperature': 24}},
            'bad argument': {'tool': 'ac.set_temperature', 'arguments': {'temperature': 100}},
            'unknown tool': {'tool': 'shell.exec', 'arguments': {}},
            'hello': {'response': '你好 🌳'},
        }
        if text == 'slow':
            time.sleep(2)
        payload = {'choices': [{'message': {'content': json.dumps(decisions.get(text, {'response': 'ok'}), ensure_ascii=True)}}]}
        if text == 'large':
            payload['padding'] = 'x' * (1024 * 1024 + 1)
        if text == 'malformed':
            payload = {'choices': [{'message': {'content': None}}]}
        self.send_response(503 if text == 'unavailable' else 200)
        body = json.dumps(payload).encode()
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


class LoopbackServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        # Keep the local protocol fixture independent of DNS configuration.
        TCPServer.server_bind(self)
        self.server_name = 'localhost'
        self.server_port = self.server_address[1]


server = LoopbackServer(('127.0.0.1', 0), Server)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    for text, expected in [('set via model', None), ('hello', None), ('bad argument', 'INVALID_ARGUMENTS'),
                           ('unknown tool', 'UNKNOWN_TOOL'), ('malformed', 'INVALID_RESPONSE'), ('unavailable', 'HTTP_ERROR')]:
        result = subprocess.run([sys.argv[1], 'run', '--endpoint', f'127.0.0.1:{server.server_port}', '--input', text],
                                capture_output=True, text=True, timeout=10)
        data = json.loads(result.stdout)
        if expected:
            assert result.returncode != 0 and data['error']['code'] == expected, (text, result, data)
        else:
            assert result.returncode == 0 and data['ok'], (text, result, data)
            if text == 'hello':
                assert data['output'] == '你好 🌳'
            else:
                assert data['route'] == 'model_tool' and data['output']['temperature'] == 24
    for text, code in [('slow', 'TIMEOUT'), ('large', 'HTTP_ERROR')]:
        begin = time.monotonic()
        probe = subprocess.run([sys.argv[2], f'http://127.0.0.1:{server.server_port}/v1/chat/completions', text],
                               capture_output=True, text=True, timeout=5)
        assert probe.returncode == 1 and probe.stdout.strip() == code, probe
        assert time.monotonic() - begin < 1.5, 'HTTP timeout exceeded'
    offline = subprocess.run([sys.argv[1], 'run', '--input', 'set AC to 22 degrees'], capture_output=True, text=True)
    assert offline.returncode == 0 and json.loads(offline.stdout)['route'] == 'skill'
finally:
    server.shutdown()
    server.server_close()
    thread.join()
