"""Remove one item from a local START capture for a controlled sync experiment."""
import argparse
import ctypes as C
import hashlib
from pathlib import Path
import re
import struct


def layout(name):
    header = (Path(__file__).parents[1] / 'src/pvp/lobby.h').read_text(encoding='utf-8')
    header = re.sub(r'//[^\n]*', '', header)
    bodies = dict((n, b) for b, n in re.findall(r'typedef struct\s*\{([^{}]*)\}\s*(\w+)\s*;', header))
    fields = []
    for declaration in bodies[name].split(';'):
        declaration = declaration.strip()
        if not declaration:
            continue
        match = re.fullmatch(r'(long long|int|char|void\*)\s+(\w+)((?:\[[\w]+\])*)', declaration)
        if not match:
            raise ValueError(declaration)
        kind, field, arrays = match.groups()
        value = {'long long': C.c_int64, 'int': C.c_int32, 'char': C.c_char, 'void*': C.c_uint64}[kind]
        constants = {'LOBBY_SLOTS': 5}
        for dimension in reversed(re.findall(r'\[(\w+)\]', arrays)):
            value *= int(dimension) if dimension.isdigit() else constants[dimension]
        fields.append((field, value))
    return type(name, (C.Structure,), {'_fields_': fields})


def ablate(source, destination, item):
    data = bytearray(source.read_bytes())
    offset = 1 + C.sizeof(layout('lobby_settings')) + 8 + 32
    level_size = struct.unpack_from('<I', data, offset)[0]
    offset += 4 + level_size
    count = data[offset] + data[offset + 1]
    offset += 2
    removed = []
    for fighter in range(count):
        offset += C.sizeof(layout('lobby_cat'))
        cat_size = struct.unpack_from('<I', data, offset)[0]
        offset += 4 + cat_size
        for slot in range(5):
            field = offset + slot * 48
            if bytes(data[field:field + 48]).split(b'\0')[0].decode() == item:
                data[field:field + 48] = bytes(48)
                removed.append((fighter, slot))
        offset += 5 * 48
    if offset != len(data):
        raise ValueError(f'Packet layout mismatch: parsed {offset} of {len(data)} bytes')
    if not removed:
        raise ValueError(f'No equipped {item} found')
    destination.write_bytes(data)
    print(f'Removed {item}: {removed}; size={len(data)}; SHA256={hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--drop-item', required=True)
    args = parser.parse_args()
    ablate(args.source, args.destination, args.drop_item)
