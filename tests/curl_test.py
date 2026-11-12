"""Run curl fixtures without terminating processes that own the test ports."""
import contextlib
import http.server
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"myos-curl-ok\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def main():
    with tempfile.TemporaryDirectory(prefix="myos-curl-") as temporary:
        cert = Path(temporary) / "cert.pem"
        key = Path(temporary) / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-nodes", "-subj", "/CN=127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        with contextlib.ExitStack() as stack:
            # Bind both sockets before starting either server. Busy ports cause a
            # normal failure; their existing owners are never touched.
            servers = [stack.enter_context(http.server.HTTPServer(("127.0.0.1", port), Handler))
                       for port in (18765, 18766)]
            servers[1].socket = context.wrap_socket(servers[1].socket, server_side=True)
            threads = [threading.Thread(target=server.serve_forever, daemon=True)
                       for server in servers]
            for thread in threads:
                thread.start()
            try:
                return subprocess.run([str(Path(sys.argv[1]).resolve())]).returncode
            finally:
                for server in servers:
                    server.shutdown()
                for thread in threads:
                    thread.join()


if __name__ == "__main__":
    raise SystemExit(main())
