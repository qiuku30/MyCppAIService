#!/usr/bin/env python3
"""Resource errors and recovery on the real, isolated image-upload HTTP route.

Generate fixtures with http_fixtures.cpp and make_fixture.py first. Credentials
and cookies exist only in memory; receipts contain assertions and HTTP statuses.
"""
import argparse
import base64
import http.cookiejar
import json
import math
import os
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18117)
    parser.add_argument('--container', default='mycpp-it-app')
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not args.container.startswith('mycpp-it-'):
        raise ValueError('Explicit isolated integration container and loopback port required')
    docker = ['docker', '--config', '/private/tmp/mycpp-integration/docker-config',
              '--context', 'colima-mycppaiservice']
    label = subprocess.check_output(docker + ['inspect', '--format',
        '{{index .Config.Labels "io.mycpp.integration"}}', args.container], text=True).strip()
    ports = json.loads(subprocess.check_output(docker + ['inspect', '--format',
        '{{json .NetworkSettings.Ports}}', args.container], text=True))
    if label != 'true' or not any(binding.get('HostPort') == str(args.port)
            and binding.get('HostIp') == '127.0.0.1'
            for binding in ports.get('8117/tcp', []) or []):
        raise ValueError('Refusing container identity or port mismatch')

    def fixture(name):
        path = args.fixtures / name
        if path.stat().st_size > 2 * 1024 * 1024:
            raise ValueError('Synthetic compressed fixture unexpectedly exceeds 2 MiB')
        return path.read_bytes()

    oversize = fixture('oversize.png')
    if oversize[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('>II', oversize[16:24]) != (5000, 5000):
        raise ValueError('Expected the safely generated 5000x5000 PNG fixture')
    images = [('unsupported_bmp', fixture('unsupported.bmp'), 400),
              ('original_pixel_budget', oversize, 413),
              ('truncated_png', fixture('normal.png')[:45], 400),
              ('baseline_jpeg_after_rejections', fixture('baseline.jpg'), 200),
              ('progressive_jpeg', fixture('progressive.jpg'), 200),
              ('png_after_rejections', fixture('normal.png'), 200)]
    cookies = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(cookies))

    def post(path, payload):
        request = urllib.request.Request('http://127.0.0.1:' + str(args.port) + path,
            data=json.dumps(payload).encode(), method='POST',
            headers={'Content-Type': 'application/json', 'Connection': 'close'})
        try:
            response = opener.open(request, timeout=60)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read())

    account = {'username': 'resource_it_' + secrets.token_hex(9),
               'password': secrets.token_urlsafe(24)}
    status, response = post('/register', account)
    if status not in (200, 201) or response.get('status') != 'success':
        raise RuntimeError('Test account registration did not succeed')
    status, response = post('/login', account)
    if status != 200 or response.get('success') is not True or not list(cookies):
        raise RuntimeError('Test account login did not establish a session')

    entries = []
    for name, data, expected in images:
        status, response = post('/upload/send', {'filename': name,
            'image': base64.b64encode(data).decode('ascii')})
        checks = {'http_status': status == expected}
        if expected == 200:
            confidence = response.get('confidence')
            checks.update(success=response.get('success') == 'ok',
                filename=response.get('filename') == name,
                label=isinstance(response.get('class_name'), str) and bool(response['class_name']),
                confidence=isinstance(confidence, (int, float)) and not isinstance(confidence, bool)
                    and math.isfinite(confidence) and 0 <= confidence <= 1)
        else:
            checks.update(error=response.get('status') == 'error',
                          message=isinstance(response.get('message'), str) and bool(response['message']))
            if expected == 413:
                checks['resource_budget'] = 'resource budget' in response.get('message', '').lower()
        entries.append({'name': name, 'expected_status': expected, 'actual_status': status,
                        'checks': checks, 'passed': all(checks.values())})
    receipt = {'stage': 'image_resource_http', 'status': 'passed' if all(x['passed'] for x in entries) else 'failed',
               'cases': entries, 'case_count': len(entries),
               'checks': sum(len(x['checks']) for x in entries),
               'failures': sum(not value for x in entries for value in x['checks'].values()),
               'boundary': 'Real upload route, OpenCV/ORT and model. Busy503 is verified separately at the actual Run barrier.'}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.receipt, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(receipt, output, indent=2); output.write('\n')
    os.chmod(args.receipt, 0o600)
    print(json.dumps(receipt))
    return int(receipt['failures'] != 0)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print(json.dumps({'stage': 'image_resource_http', 'status': 'unavailable',
                          'reason': type(error).__name__}), file=sys.stderr)
        sys.exit(2)
