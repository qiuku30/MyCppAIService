#!/usr/bin/env python3
"""Inspect actual logs from the dedicated loopback Linux fixture, never print secrets."""
import argparse
import base64
from datetime import datetime, timedelta, timezone
import http.cookiejar
import ipaddress
import json
import os
from pathlib import Path
import secrets
import subprocess
import urllib.error
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', default='http://127.0.0.1:18117')
    parser.add_argument('--docker-config', default='/private/tmp/mycpp-integration/docker-config')
    parser.add_argument('--docker-context', default='colima-mycppaiservice')
    parser.add_argument('--container', default='mycpp-it-app')
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    url = urllib.parse.urlsplit(args.base_url)
    if (url.scheme != 'http' or not url.port or url.username or url.password
            or url.path not in ('', '/') or url.query or url.fragment
            or not (url.hostname == 'localhost' or ipaddress.ip_address(url.hostname).is_loopback)):
        raise ValueError('Explicit loopback HTTP fixture address required')
    if not args.container.startswith('mycpp-it-'):
        raise ValueError('Dedicated integration container required')
    docker = ['docker', '--config', args.docker_config, '--context', args.docker_context]
    label = subprocess.check_output(docker + ['inspect', '--format',
        '{{index .Config.Labels "io.mycpp.integration"}}', args.container], text=True).strip()
    if label != 'true':
        raise ValueError('Refusing unlabelled container')
    cookies = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                        urllib.request.HTTPCookieProcessor(cookies))
    nonce = secrets.token_hex(12)
    account = {'username': 'log_it_' + nonce, 'password': secrets.token_urlsafe(24)}
    marker = 'PRIVATE_BODY_' + nonce
    path_marker = 'PRIVATE_PATH_' + nonce
    query_marker = 'PRIVATE_QUERY_' + nonce
    auth_marker = 'PRIVATE_AUTH_' + nonce
    since = (datetime.now(timezone.utc) - timedelta(seconds=1)).isoformat()

    def request(path, payload=None, headers=None):
        data = None if payload is None else json.dumps(payload).encode()
        request_headers = {'Connection': 'close', **(headers or {})}
        if data is not None:
            request_headers['Content-Type'] = 'application/json'
        req = urllib.request.Request(args.base_url.rstrip('/') + path, data=data,
                                     headers=request_headers)
        try:
            response = opener.open(req, timeout=30)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.read()

    status, body = request('/register', account)
    if status not in (200, 201) or json.loads(body).get('status') != 'success':
        raise RuntimeError('Fixture registration failed')
    status, body = request('/login', account)
    if status != 200 or json.loads(body).get('success') is not True or not list(cookies):
        raise RuntimeError('Fixture login failed')
    status, body = request('/upload/send', {'filename': marker + '.png',
        'image': base64.b64encode(args.fixture.read_bytes()).decode('ascii')},
        {'Authorization': 'Bearer ' + auth_marker})
    prediction = json.loads(body)
    if status != 200 or prediction.get('filename') != marker + '.png':
        raise RuntimeError('Actual model upload response did not complete')
    unknown_status, _ = request('/' + path_marker + '?token=' + query_marker)
    if unknown_status != 404:
        raise RuntimeError('Fixture unmatched route did not return404')
    # muduo's stdout sink can buffer. Fixed non-sensitive404 traffic forces the
    # preceding requests through that buffer without changing server log policy.
    for _ in range(96):
        status, _ = request('/__logging_flush__')
        if status != 404:
            raise RuntimeError('Fixture log flush request failed')
    result = subprocess.run(docker + ['logs', '--since', since, args.container],
                            capture_output=True, text=True, check=True)
    log = result.stdout + result.stderr
    checks = {
        'cookie_absent': not any(cookie.value in log for cookie in cookies),
        'password_absent': account['password'] not in log,
        'authorization_absent': auth_marker not in log,
        'body_absent': marker not in log,
        'raw_path_absent': path_marker not in log,
        'query_absent': query_marker not in log,
        'metadata_present': log.count('http_request request_id=') >= 3,
        'trusted_image_route_present': 'route="/upload/send"' in log,
    }
    receipt = {'stage': 'live_log_privacy',
               'status': ('unavailable' if not log.strip() else ('passed' if all(checks.values()) else 'failed')),
               'checks': checks, 'http': {'login': 200, 'image': 200, 'unmatched': 404},
               'private_values_printed': False,
               'scope': 'real Linux muduo logs, real login cookie and actual OpenCV/ORT image response'}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.receipt, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(receipt, output, indent=2)
        output.write('\n')
    os.chmod(args.receipt, 0o600)
    # Raw runtime logs may contain actual credentials on RED. Keep them private.
    log_path = args.receipt.with_suffix('.private.log')
    fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as output:
        output.write(log)
    os.chmod(log_path, 0o600)
    print(json.dumps(receipt))
    return 0 if receipt['status'] == 'passed' else (2 if receipt['status'] == 'unavailable' else 1)


if __name__ == '__main__':
    raise SystemExit(main())
