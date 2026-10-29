#!/usr/bin/env python3
"""Real INFO-level muduo logging contract, for a labelled synthetic fixture only."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18118)
    parser.add_argument('--container', required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or not args.container.startswith('mycpp-it-log-'):
        raise ValueError('Dedicated loopback logging fixture required')
    docker = ['docker', '--config', '/private/tmp/mycpp-integration/docker-config',
              '--context', 'colima-mycppaiservice']
    label = subprocess.check_output(docker + ['inspect', '--format',
        '{{index .Config.Labels "io.mycpp.integration"}}', args.container], text=True).strip()
    ports = json.loads(subprocess.check_output(docker + ['inspect', '--format', '{{json .NetworkSettings.Ports}}', args.container], text=True))
    if label != 'true' or not any(binding.get('HostPort') == str(args.port) and binding.get('HostIp') == '127.0.0.1' for binding in ports.get('8118/tcp', []) or []):
        raise ValueError('Refusing fixture identity/port mismatch')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, payload=None):
        req = urllib.request.Request('http://127.0.0.1:' + str(args.port) + path,
            data=None if payload is None else json.dumps(payload).encode(), headers={
                'Connection': 'close', 'Cookie': 'sessionId=FAKE_REQUEST_COOKIE_PRIVATE',
                'Authorization': 'Bearer FAKE_AUTH_REQUEST_PRIVATE'})
        try:
            response = opener.open(req, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            return response.status, response.read(), response.headers

    for attempt in range(30):
        try:
            status, _, _ = request('/ready')
            if status == 404:
                break
        except OSError:
            pass
        time.sleep(0.1)
    else:
        raise RuntimeError('Logging network fixture unavailable')
    status, body, headers = request('/private', {'question': 'FAKE_QUESTION_PRIVATE'})
    assert status == 200 and body == b'FAKE_RESPONSE_BODY_PRIVATE'
    assert headers.get('Set-Cookie', '').startswith('sessionId=FAKE_COOKIE_PRIVATE')
    assert headers.get('Authorization') == 'Bearer FAKE_AUTH_RESPONSE_PRIVATE'
    status, body, _ = request('/failure')
    assert status == 500 and body == b'FAKE_EXCEPTION_PRIVATE'
    status, _, _ = request('/FAKE_RAW_PATH_PRIVATE?token=FAKE_QUERY_PRIVATE')
    assert status == 404
    status, body, _ = request('/stream')
    assert status == 200 and body == b'data: FAKE_STREAM_BODY_PRIVATE\n\ndata: [DONE]\n\n'
    for _ in range(96):
        assert request('/flush')[0] == 404
    logs = subprocess.run(docker + ['logs', args.container], text=True,
                          capture_output=True, check=True)
    text = logs.stdout + logs.stderr
    if not ('Sending response:' in text or 'http_request request_id=' in text):
        raise RuntimeError('No request logs observed; cannot classify privacy')
    markers = ['FAKE_COOKIE_PRIVATE', 'FAKE_REQUEST_COOKIE_PRIVATE', 'FAKE_AUTH_RESPONSE_PRIVATE',
               'FAKE_AUTH_REQUEST_PRIVATE', 'FAKE_RESPONSE_BODY_PRIVATE', 'FAKE_QUESTION_PRIVATE',
               'FAKE_EXCEPTION_PRIVATE', 'FAKE_RAW_PATH_PRIVATE', 'FAKE_QUERY_PRIVATE', 'FAKE_STREAM_BODY_PRIVATE']
    checks = {'synthetic_markers_absent': all(marker not in text for marker in markers),
              'response_metadata_present': 'event=response' in text and 'response_bytes=' in text,
              'stream_open_metadata_present': 'event=stream_open' in text,
              'trusted_route_present': 'route="/private"' in text,
              'actual_wire_contracts_passed': True}
    result = {'stage': 'real_muduo_info_log', 'status': 'passed' if all(checks.values()) else 'failed',
              'checks': checks, 'leaked_marker_count': sum(marker in text for marker in markers),
              'request_log_count': text.count('http_request request_id='), 'fixture_level': 'INFO',
              'boundary': 'real muduo network and production HTTP source; synthetic callbacks, no DB/model/provider calls'}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    for path, value in [(args.receipt, json.dumps(result, indent=2) + '\n'),
                        (args.receipt.with_suffix('.private.log'), text)]:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as output:
            output.write(value)
        os.chmod(path, 0o600)
    print(json.dumps(result))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
