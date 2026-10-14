# SPDX-License-Identifier: GPL-2.0-only
"""Three independent clients: shared handles, lifetime and layer invariants."""
import pathlib
import subprocess
import time
import select
from PIL import Image

root = pathlib.Path(__file__).resolve().parent
clients = []
def line(client):
    assert select.select([client.stdout], [], [], 5)[0], 'client response timeout'
    reply = client.stdout.readline().strip()
    assert reply, 'client exited'
    return reply
def start(color):
    p = subprocess.Popen([str(root / 'handle-client'), color], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True)
    clients.append(p)
    while True:
        reply = line(p)
        if reply.startswith('HANDLE '):
            return p, reply.split()[1]
def command(p, text):
    p.stdin.write(text + '\n')
    p.stdin.flush()
    assert line(p) == 'ACK'
def pixel(name, color):
    time.sleep(.2)
    target = root / 'output' / ('handles-' + name + '.png')
    subprocess.run(['grim', str(target)], check=True)
    with Image.open(target) as image:
        assert image.convert('RGB').getpixel((175, 175)) == color, name
    print('PASS', name, flush=True)
try:
    a, ha = start('FFFF0000')
    b, hb = start('FF0000FF')
    c, hc = start('FF00FF00')
    assert len({ha, hb, hc}) == 3
    pixel('initial', (0, 255, 0))
    command(a, 'above ' + hc)
    pixel('above-foreign', (255, 0, 0))
    command(a, 'below ' + hb)
    pixel('below-foreign', (0, 255, 0))
    command(b, 'above ' + hc)
    pixel('second-client-above', (0, 0, 255))
    command(c, 'below ' + ha)
    pixel('third-client-below', (0, 0, 255))
    command(a, 'above ' + hb)
    pixel('raise-again', (255, 0, 0))
    command(a, 'below ' + ha)
    pixel('self-noop', (255, 0, 0))
    b.stdin.write('quit\n'); b.stdin.flush()
    assert b.wait(timeout=5) == 0
    command(a, 'below ' + hb)
    command(a, 'below ' + '0' * 32)
    pixel('stale-and-unknown-noop', (255, 0, 0))
    command(c, 'layer 1')
    command(a, 'above ' + hc)
    pixel('cross-layer-noop', (0, 255, 0))
    command(c, 'minimize')
    pixel('minimized', (255, 0, 0))
    command(c, 'restore')
    pixel('restored-without-focus', (0, 255, 0))
    print('SHARED_HANDLE_CHECK_PASSED', flush=True)
finally:
    for p in clients:
        if p.poll() is None:
            p.stdin.write('quit\n'); p.stdin.flush()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
