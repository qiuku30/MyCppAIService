#!/usr/bin/env python3
"""Real login Session network lifecycle fixture; no database or model actions."""
import argparse
import http.cookiejar
import json
import os
import subprocess
from pathlib import Path
import time
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18120)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--container', required=True)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        raise ValueError('Explicit loopback fixture port required')
    if not args.container.startswith('mycpp-it-session-'):
        raise ValueError('Dedicated Session fixture name required')
    docker = ['docker', '--config', '/private/tmp/mycpp-integration/docker-config', '--context', 'colima-mycppaiservice']
    label = subprocess.check_output(docker + ['inspect', '--format', '{{index .Config.Labels "io.mycpp.integration"}}', args.container], text=True).strip()
    ports = json.loads(subprocess.check_output(docker + ['inspect', '--format', '{{json .NetworkSettings.Ports}}', args.container], text=True))
    if label != 'true' or not any(binding.get('HostPort') == str(args.port) and binding.get('HostIp') == '127.0.0.1' for binding in ports.get('8118/tcp', []) or []):
        raise ValueError('Refusing fixture identity/port mismatch')
    base = 'http://127.0.0.1:' + str(args.port)

    def client():
        cookies = http.cookiejar.CookieJar()
        return cookies, urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                                   urllib.request.HTTPCookieProcessor(cookies))

    def request(opener, path):
        try:
            response = opener.open(urllib.request.Request(base + path,
                headers={'Connection': 'close'}), timeout=3)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            payload = response.read()
            data = json.loads(payload) if response.headers.get_content_type() == 'application/json' else {'opaque_body_bytes': len(payload)}
            return response.status, data, response.headers

    a, open_a = client(); b, open_b = client(); c, open_c = client()
    _, observer = client()
    for _ in range(30):
        try:
            if request(observer, '/observe')[0] == 200: break
        except OSError:
            pass
        time.sleep(0.1)
    else:
        raise RuntimeError('Session network fixture unavailable')
    checks = {}
    checks['first_admitted'] = request(open_a, '/session')[0] == 200 and bool(list(a))
    checks['second_admitted'] = request(open_b, '/session')[0] == 200 and bool(list(b))
    status, _, headers = request(open_c, '/session')
    checks['capacity503_without_cookie'] = status == 503 and not headers.get('Set-Cookie') and not list(c)
    original_a = next(iter(a)).value
    status, data, headers = request(open_a, '/session')
    checks['existing_retained_at_capacity'] = status == 200 and data.get('value') == 'retained' and not headers.get('Set-Cookie')
    checks['explicit_drop'] = request(open_b, '/drop')[1].get('dropped') is True
    checks['removed_capacity_reused'] = request(open_c, '/session')[0] == 200
    observed = None
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        status, data, headers = request(open_a, '/session')
        if status != 200 or headers.get('Set-Cookie') or next(iter(a)).value != original_a:
            raise RuntimeError('Active Session unexpectedly expired during fixture keepalive')
        observed = request(observer, '/observe')[1]
        if observed['live'] == 1 and observed['expired'] >= 2:
            break
        time.sleep(0.1)
    checks['unvisited_session_automatically_reclaimed'] = observed['live'] == 1 and observed['expired'] >= 2
    checks['active_cookie_survives_sweeps'] = next(iter(a)).value == original_a
    checks['retained_before_replace'] = request(open_a, '/hold')[1].get('held') is True
    checks['manager_replacement_retained_pointer_safe'] = request(observer, '/replace')[1].get('retained_safe') is True
    checks['fresh_storage_after_replace'] = request(open_a, '/session')[0] == 200 and next(iter(a)).value != original_a
    receipt = {'stage': 'real_session_lifecycle', 'status': 'passed' if all(checks.values()) else 'failed',
               'checks': checks, 'check_count': len(checks), 'private_cookie_values_printed': False,
               'policy': {'capacity': 2, 'ttl_seconds': 2, 'cleanup_ms': 25},
               'boundary': 'real muduo/Session sources with synthetic routes, no DB or model actions; manager replacement without concurrent external manager method calls'}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.receipt, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(receipt, output, indent=2);output.write('\n')
    os.chmod(args.receipt, 0o600)
    print(json.dumps(receipt))
    return 0 if receipt['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
