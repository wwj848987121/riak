#!/usr/bin/env python3
"""Check mod_php in a temporary, loopback-only Apache without changing the service."""
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.request

stage = Path(__file__).resolve().parent
stage.chmod(0o755)
scan = stage / 'ini'
scan.mkdir(mode=0o755, exist_ok=True)
(scan / '40-riak_pb.ini').write_text('extension=' + str(stage / 'riak_pb.so') + '\n')
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0))
    port = listener.getsockname()[1]
config = stage / 'httpd.conf'
config.write_text(f'''ServerRoot /etc/httpd
DefaultRuntimeDir {stage}
PidFile {stage}/httpd.pid
ErrorLog {stage}/httpd-error.log
LogLevel warn
ServerName localhost
Listen 127.0.0.1:{port}
LoadModule mpm_prefork_module modules/mod_mpm_prefork.so
LoadModule unixd_module modules/mod_unixd.so
LoadModule authz_core_module modules/mod_authz_core.so
LoadModule php_module modules/libphp.so
User daemon
Group daemon
StartServers 1
MinSpareServers 1
MaxSpareServers 1
ServerLimit 1
MaxRequestWorkers 1
PHPIniDir /etc
DocumentRoot {stage}
<Directory />
    Require all denied
</Directory>
<Directory {stage}>
    AllowOverride None
    Options None
    Require all granted
</Directory>
<Files compat-runtime.php>
    SetHandler application/x-httpd-php
</Files>
''')
env = os.environ.copy()
env['PHP_INI_SCAN_DIR'] = '/etc/php.d:' + str(scan)
command = ['/usr/sbin/httpd', '-f', str(config)]
subprocess.run(command + ['-t'], env=env, check=True, timeout=10)
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with (stage / 'httpd-process.log').open('w') as log:
    process = subprocess.Popen(command + ['-DFOREGROUND'], env=env, stdout=log, stderr=log,
                               start_new_session=True)
    try:
        deadline = time.monotonic() + 10
        while True:
            if process.poll() is not None:
                raise RuntimeError('Temporary Apache exited: ' + str(process.returncode))
            try:
                with opener.open(f'http://127.0.0.1:{port}/compat-runtime.php', timeout=1) as response:
                    result = json.load(response)
                break
            except (OSError, ValueError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        assert result['sapi'] == 'apache2handler', result
        assert result['riak_pb_loaded'] is True, result
        assert all(result['functions'].values()), result
        assert result['native_parameter_validation'] == 'PASS', result
        print(json.dumps(result, indent=2), flush=True)
        print('PASS: isolated Apache mod_php with existing PHP configuration', flush=True)
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        for name in ('httpd-process.log', 'httpd-error.log'):
            path = stage / name
            if path.exists() and path.stat().st_size:
                print(name + ':\n' + path.read_text())
        print('Temporary Apache stopped; PID=' + str(process.pid))
