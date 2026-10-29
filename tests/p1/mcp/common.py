from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
DEPS = Path(os.environ.get('MYCPP_TEST_DEPS', '/private/tmp/mycpp-p1-deps'))


def function(source, signature):
    """Extract one unmodified C++ function, ignoring braces in literals/comments."""
    begin = source.index(signature)
    opening = source.index('{', begin)
    level = 0
    state = 'code'
    i = opening
    while i < len(source):
        c = source[i]
        following = source[i:i + 2]
        if state in ('"', "'"):
            if c == '\\':
                i += 2
                continue
            if c == state:
                state = 'code'
        elif state == 'line':
            if c == '\n':
                state = 'code'
        elif state == 'comment':
            if following == '*/':
                state = 'code'
                i += 2
                continue
        elif following == '//':
            state = 'line'
            i += 2
            continue
        elif following == '/*':
            state = 'comment'
            i += 2
            continue
        elif c in ('"', "'"):
            state = c
        elif c == '{':
            level += 1
        elif c == '}':
            level -= 1
            if level == 0:
                return source[begin:i + 1]
        i += 1
    raise RuntimeError('Unterminated production function: ' + signature)


def run_fixture(fixture, replacements):
    source = (Path(__file__).parent / fixture).read_text()
    for marker, body in replacements.items():
        if source.count(marker) != 1:
            raise RuntimeError('Fixture marker must occur once: ' + marker)
        source = source.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix='mycpp-mcp-') as directory:
        output = Path(directory)
        unit = output / fixture
        unit.write_text(source)
        compiler = os.environ.get('CXX', 'clang++')
        subprocess.run([compiler, '-std=c++17', '-O0', '-g', '-pthread',
                        '-I' + str(ROOT), '-I' + str(DEPS),
                        *shlex.split(os.environ.get('MCP_TEST_CXXFLAGS', '')),
                        str(unit), '-o', str(output / 'test')], check=True)
        result = subprocess.run([str(output / 'test')])
        if result.returncode:
            raise SystemExit(result.returncode)
