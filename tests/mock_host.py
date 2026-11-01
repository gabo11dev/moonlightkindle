#!/usr/bin/env python3
"""Exercise HTTP/TLS control against a fixture, not a real Sunshine server.
Usage: mock_host.py [emulator and loader options ...] /path/to/protocol-test
"""
import http.server, ssl, subprocess, sys, tempfile, threading
from pathlib import Path
class Host(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        if self.path.startswith('/serverinfo'):
            body=f'''<root status_code="200"><currentgame>0</currentgame><PairStatus>1</PairStatus>
<appversion>7.1.431.0</appversion><state>SUNSHINE_SERVER_FREE</state><ServerCodecModeSupport>1</ServerCodecModeSupport>
<gputype>Fixture</gputype><GsVersion>1</GsVersion><GfeVersion>3.26.0</GfeVersion><HttpsPort>{tls.server_port}</HttpsPort>
<DisplayMode><Height>360</Height><Width>640</Width><RefreshRate>10</RefreshRate></DisplayMode></root>'''
        elif self.path.startswith('/applist'):
            body='<root status_code="200"><App><ID>123</ID><AppTitle>Desktop</AppTitle></App></root>'
        elif self.path.startswith('/launch'):
            body='<root status_code="503" status_message="Fixture launch rejected"><gamesession>0</gamesession></root>'
        else:
            self.send_error(404); return
        data=body.encode(); self.send_response(200); self.send_header('Content-Length',str(len(data)))
        self.end_headers(); self.wfile.write(data)
with tempfile.TemporaryDirectory(prefix='moonlight-protocol-') as tmp:
    tmp=Path(tmp); cert=tmp/'server.pem'; key=tmp/'server.key'; keys=tmp/'keys'; keys.mkdir(mode=0o700)
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-days','1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    http_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Host)
    tls=http.server.ThreadingHTTPServer(('127.0.0.1',0),Host)
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert,key)
    tls.socket=ctx.wrap_socket(tls.socket,server_side=True)
    for s in [http_server,tls]: threading.Thread(target=s.serve_forever,daemon=True).start()
    try:
        result=subprocess.run(sys.argv[1:]+[str(http_server.server_port),str(keys)],timeout=60)
        assert (keys/'uniqueid.dat').read_text()!='0123456789ABCDEF'
    finally:
        http_server.shutdown(); tls.shutdown(); http_server.server_close(); tls.server_close()
    sys.exit(result.returncode)
