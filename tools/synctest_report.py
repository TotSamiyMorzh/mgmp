"""Compare paired turn hashes and distinguish assigned skills from executed casts."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import ctypes
import struct
from synctest_ablate import layout


def hashes(text):
    battle = ''
    result = {}
    for line in text.splitlines():
        match = re.search(r'LOCKSTEP\s+battle id now ([0-9a-f]+)', line)
        if match:
            battle = match[1]
        match = re.search(r'LOCKSTEP\s+turn (\d+) hash rng=(\w+) state=(\w+) queue=(\d+) chars=(\d+/\d+)', line)
        if match:
            result[battle, int(match[1])] = match.groups()[1:]
    return result


def report(run):
    peers = {}
    for role in ('host', 'client'):
        logs = sorted((run / role).glob(f'mgmp_{role}_*.log'))
        text = logs[-1].read_text(encoding='utf-8', errors='replace') if logs else ''
        peers[role] = hashes(text)
    common = peers['host'].keys() & peers['client'].keys()
    mismatch = [f'{battle}:{turn}' for battle, turn in sorted(common)
                if peers['host'][battle, turn] != peers['client'][battle, turn]]
    manifest = (run / 'host' / 'synctest.log').read_text(encoding='utf-8', errors='replace')
    assigned = set()
    passive = set()
    for m in re.finditer(r' skills=(\S+) passives=(\S+)', manifest):
        assigned.update(m[1].split(','))
        passive.update(m[2].split(','))
    # Replay captures already contain the fighter rows, without new CAT lines.
    if not assigned:
        row_type = layout('lobby_cat')
        for capture in sorted((run / 'host').glob('synctest_*.start')):
            data = capture.read_bytes()
            offset = 1 + ctypes.sizeof(layout('lobby_settings')) + 8 + 32
            level_size = struct.unpack_from('<I', data, offset)[0]
            offset += 4 + level_size
            count = data[offset] + data[offset + 1]
            offset += 2
            for _ in range(count):
                row = row_type.from_buffer_copy(data, offset)
                assigned.update(bytes(s).split(b'\0')[0].decode() for s in row.abil if bytes(s).split(b'\0')[0])
                passive.update(bytes(s).split(b'\0')[0].decode() for s in row.passive if bytes(s).split(b'\0')[0])
                offset += ctypes.sizeof(row_type)
                cat_size = struct.unpack_from('<I', data, offset)[0]
                offset += 4 + cat_size + 5 * 48
            if offset != len(data):
                raise ValueError(f'Unexpected START layout in {capture}')
    casts = Counter(re.findall(r'CAST .*?ability=(\S+)', manifest))
    auto = (run / 'host' / 'pvp_auto.log').read_text(encoding='utf-8', errors='replace')
    output = dict(host_hashes=len(peers['host']), client_hashes=len(peers['client']),
                  compared_hashes=len(common), mismatch=mismatch,
                  completed_fights=len(re.findall(r'fight \d+ over', auto)),
                  started_fights=len(re.findall(r'fight \d+ started', auto)),
                  scenarios=re.findall(r'FIGHT \d+ seed=\d+ scenario=(\w+)', manifest),
                  assigned_actives=sorted(assigned), assigned_passives=sorted(passive),
                  executed_assigned_actives=sorted(assigned & casts.keys()),
                  unobserved_assigned_actives=sorted(assigned - casts.keys()),
                  cast_counts=dict(casts.most_common()),
                  note='CAST is entry to Ability::trigger, not proof of a successful hit; passive execution is not instrumented.')
    (run / 'coverage.json').write_text(json.dumps(output, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(compared_hashes=len(common), mismatches=len(mismatch),
                         completed_fights=output['completed_fights'],
                         assigned_actives=len(assigned), observed_actives=len(assigned & casts.keys()),
                         assigned_passives=len(passive), scenarios=output['scenarios'])))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    report(parser.parse_args().run)
