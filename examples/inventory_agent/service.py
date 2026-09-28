#!/usr/bin/env python3
"""Loopback-only inventory example. SQLite atomically stores stock and receipts.
No authentication: use only as a local integration example, not a public service.
"""
import argparse
import json
import re
import sqlite3
from socketserver import TCPServer
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit


def initialize(path):
    with sqlite3.connect(path) as db:
        db.execute('PRAGMA journal_mode=WAL')
        db.execute('CREATE TABLE IF NOT EXISTS stock(sku TEXT PRIMARY KEY, quantity INTEGER NOT NULL)')
        db.execute('CREATE TABLE IF NOT EXISTS receipts(key TEXT PRIMARY KEY, input TEXT NOT NULL, result TEXT NOT NULL, status INTEGER NOT NULL)')
        db.execute("INSERT OR IGNORE INTO stock VALUES('widget',10)")


class LoopbackServer(HTTPServer):
    def server_bind(self):
        # HTTPServer normally does a reverse-DNS lookup for server_name. This
        # loopback-only service does not need DNS and must start offline too.
        TCPServer.server_bind(self)
        self.server_name = 'localhost'
        self.server_port = self.server_address[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--database', required=True)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--ready-file')
    parser.add_argument('--drop-first-response', action='store_true', help='Fault injection: commit then close socket without a response')
    args = parser.parse_args()
    initialize(args.database)

    class Handler(BaseHTTPRequestHandler):
        dropped = False

        def log_message(self, *_):
            pass

        def reply(self, status, result):
            body = json.dumps(result).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            with sqlite3.connect(args.database) as db:
                if path == '/stock/widget':
                    return self.reply(200, {'quantity': db.execute("SELECT quantity FROM stock WHERE sku='widget'").fetchone()[0]})
                if path.startswith('/receipts/'):
                    receipt = db.execute('SELECT result FROM receipts WHERE key=?', (path.removeprefix('/receipts/'),)).fetchone()
                    if receipt:
                        return self.reply(200, json.loads(receipt[0]))
            self.reply(404, {'error': 'not found'})

        def do_POST(self):
            if self.path != '/reservations':
                return self.reply(404, {'error': 'not found'})
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 4096:
                    raise ValueError('Invalid body length')
                key = self.headers.get('Idempotency-Key', '')
                if not re.fullmatch(r'[0-9a-f]{2,512}\.[0-9a-f]{2,512}', key):
                    raise ValueError('Invalid idempotency key')
                request = json.loads(self.rfile.read(length))
                if (not isinstance(request, dict) or set(request) != {'sku', 'quantity'} or
                        not isinstance(request['sku'], str) or type(request['quantity']) is not int or
                        not 1 <= request['quantity'] <= 300000):
                    raise ValueError('Invalid reservation')
            except (ValueError, TypeError):
                return self.reply(400, {'error': 'invalid request'})
            encoded = json.dumps(request, sort_keys=True)
            with sqlite3.connect(args.database) as db:
                db.execute('PRAGMA synchronous=FULL')
                db.execute('BEGIN IMMEDIATE')
                prior = db.execute('SELECT input,result,status FROM receipts WHERE key=?', (key,)).fetchone()
                if prior:
                    if prior[0] != encoded:
                        return self.reply(409, {'key': key, 'outcome': 'conflict', 'error': 'key reused with different input'})
                    return self.reply(prior[2], json.loads(prior[1]))
                stock = db.execute('SELECT quantity FROM stock WHERE sku=?', (request['sku'],)).fetchone()
                if not stock or stock[0] < request['quantity']:
                    result = {'key': key, 'outcome': 'failed', 'error': 'Insufficient stock or unknown SKU'}
                    status = 409
                else:
                    remaining = stock[0] - request['quantity']
                    db.execute('UPDATE stock SET quantity=? WHERE sku=?', (remaining, request['sku']))
                    result = {'key': key, 'outcome': 'committed', 'output': {**request, 'remaining': remaining}}
                    status = 200
                db.execute('INSERT INTO receipts VALUES(?,?,?,?)', (key, encoded, json.dumps(result), status))
            # The independent service transaction has committed before this fault.
            if args.drop_first_response and not Handler.dropped:
                Handler.dropped = True
                self.close_connection = True
                return
            self.reply(status, result)

    server = LoopbackServer(('127.0.0.1', args.port), Handler)
    if args.ready_file:
        Path(args.ready_file).write_text(str(server.server_port))
    print(f'Inventory service listening on http://127.0.0.1:{server.server_port}', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
