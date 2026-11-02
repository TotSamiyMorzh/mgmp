"""Build a local-only stress-test skill pool from the owner's resources.gpak."""
import argparse
import json
import re
from pathlib import Path
from gpak import GPak


def classes(text):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    depth = 0
    start = 0
    name = ''
    for m in re.finditer(r'"(?:\\.|[^"\\])*"|[A-Za-z_][A-Za-z_0-9]*|[{}]', text):
        token = m.group()
        if token == '{':
            if depth == 0:
                start = m.end()
            depth += 1
        elif token == '}':
            depth -= 1
            if depth == 0:
                yield name, text[start:m.start()]
        elif depth == 0:
            name = token


def pool(body, key):
    match = re.search(r'\b' + key + r'\s*\[([^\]]*)\]', body, re.S)
    return re.findall(r'[A-Za-z_][A-Za-z_0-9]*', match[1]) if match else []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gpak')
    parser.add_argument('out')
    args = parser.parse_args()
    g = GPak(args.gpak)
    rows = []
    abilities, passives = set(), set()
    for path in ('data/classes/classes.gon', 'data/classes/advanced_classes.gon'):
        for name, body in classes(g.read(path).decode('utf-8-sig')):
            attacks = pool(body, 'attack_pool')
            active = pool(body, 'ability_pool')
            passive = pool(body, 'passive_pool')
            if attacks and active and passive:
                rows.append('C\t' + name + '\t' + attacks[0])
                abilities.update(active)
                passives.update(passive)
    # Avoid repeatedly wiping an under-equipped friendly team before skills run.
    passives.discard('Loner')
    rows += ['A\t' + name for name in sorted(abilities)]
    rows += ['P\t' + name for name in sorted(passives)]
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    print(json.dumps(dict(classes=sum(r.startswith('C\t') for r in rows),
                          abilities=len(abilities), passives=len(passives), output=str(out))))


if __name__ == '__main__':
    main()
