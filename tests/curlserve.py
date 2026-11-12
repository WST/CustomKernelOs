# Локальный HTTP или HTTPS для make curltest. Не гостевой httpd.
import http.server
import ssl
import sys

port = int(sys.argv[1])
body = b"myos-curl-ok\n"

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass

server = http.server.HTTPServer(("127.0.0.1", port), Handler)
if len(sys.argv) > 2:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
server.serve_forever()
