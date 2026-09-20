"""Read-only creature attack audit; no game or mod files are changed.

Record layouts: TES5Edit/Core/wbDefinitionsTES5.pas (ATKD, MGEF DATA,
SPEL SPIT). Outputs resolved plugin-relative identities, never load-order IDs.
Usage: python tools/audit_creature_attacks.py DATA_DIR OUTPUT_DIR [EXTRA_PLUGIN ...]
"""
from pathlib import Path
import hashlib
import json
import struct
import sys
import zlib
from esm_scan import parse_subrecords, zstring


def script_forms(data, key):
    """VMAD script properties (TES5 v5, object formats 1/2); ignore fragments."""
    if not data:
        return [], []
    version, fmt, count = struct.unpack_from('<HHH', data)
    pos, forms, scripts = 6, [], []
    def string():
        nonlocal pos
        size = struct.unpack_from('<H', data, pos)[0]; pos += 2
        value = data[pos:pos+size].decode('utf-8', 'replace'); pos += size
        return value
    def value(kind):
        nonlocal pos
        if kind >= 11 and kind <= 15:
            size = struct.unpack_from('<I', data, pos)[0]; pos += 4
            for _ in range(size): value(kind-10)
        elif kind == 1:
            form = key(struct.unpack_from('<I', data, pos + (4 if fmt == 2 else 0))[0])
            if form: forms.append(form)
            pos += 8
        elif kind == 2: string()
        elif kind in (3, 4): pos += 4
        elif kind == 5: pos += 1
        else: raise ValueError(f'Unknown VMAD property type {kind}')
    for _ in range(count):
        scripts.append(string()); pos += 1
        properties = struct.unpack_from('<H', data, pos)[0]; pos += 2
        for _ in range(properties):
            string(); kind = data[pos]; pos += 2
            value(kind)
    return sorted(set(forms)), scripts


def scan(paths):
    records, inputs = {}, []
    wanted = {b'RACE', b'NPC_', b'SPEL', b'MGEF', b'PROJ', b'ENCH', b'WEAP',
              b'SHOU', b'EXPL', b'HAZD', b'KYWD', b'PERK', b'LVLN'}
    for path in paths:
        buf = path.read_bytes()
        inputs.append({'plugin': path.name, 'sha256': hashlib.sha256(buf).hexdigest()})
        size = struct.unpack_from('<I', buf, 4)[0]
        masters = [zstring(v) for k, v in parse_subrecords(buf[24:24+size]) if k == b'MAST'] + [path.name]

        def key(value):
            index = value >> 24
            return f'{masters[index]}:{value & 0xffffff:06X}' if value and index < len(masters) else ''

        def walk(start, end):
            pos = start
            while pos + 24 <= end:
                kind, length, flags, formid = struct.unpack_from('<4sIII', buf, pos)
                if kind == b'GRUP':
                    label, group_type = struct.unpack_from('<4sI', buf, pos+8)
                    if group_type == 0 and label in wanted:
                        walk(pos+24, pos+length)
                    pos += length
                    continue
                body = buf[pos+24:pos+24+length]
                pos += 24+length
                if kind not in wanted:
                    continue
                if flags & 0x40000:
                    body = zlib.decompress(body[4:])
                subs = parse_subrecords(body)
                def first(tag):
                    return next((v for k, v in subs if k == tag), b'')
                def uint(v, offset=0):
                    return struct.unpack_from('<I', v, offset)[0]
                def ref(tag):
                    v = first(tag)
                    return key(uint(v)) if len(v) >= 4 else ''
                r = {'key': key(formid), 'plugin': path.name, 'type': kind.decode(),
                     'edid': zstring(first(b'EDID'))}
                try:
                    r['script_forms'], r['scripts'] = script_forms(first(b'VMAD'), key)
                except (ValueError, struct.error, IndexError) as error:
                    r['script_error'] = str(error)
                r['spells'] = [key(uint(v)) for k, v in subs if k == b'SPLO']
                r['keywords'] = [key(n[0]) for k, v in subs if k == b'KWDA' for n in struct.iter_unpack('<I', v)]
                r['models'] = [zstring(v) for k, v in subs if k in (b'MODL', b'ANAM') and b'.nif' in v.lower()]
                attacks = []
                for k, v in subs:
                    if k == b'ATKD':
                        attacks.append({'spell': key(uint(v, 8)), 'flags': uint(v, 12)})
                    elif k == b'ATKE' and attacks:
                        attacks[-1]['event'] = zstring(v)
                r['attacks'] = attacks
                if kind == b'NPC_':
                    r.update(race=ref(b'RNAM'), template=ref(b'TPLT'),
                             inventory=[key(uint(v)) for k, v in subs if k == b'CNTO'])
                    v = first(b'ACBS')
                    r['template_flags'] = struct.unpack_from('<H', v, 18)[0] if len(v) >= 20 else 0
                if kind == b'LVLN':
                    r['actors'] = [key(uint(v, 4)) for k, v in subs if k == b'LVLO' and len(v) >= 8]
                if kind in (b'SPEL', b'ENCH'):
                    effects = []
                    for k, v in subs:
                        if k == b'EFID':
                            effects.append({'effect': key(uint(v))})
                        elif k == b'EFIT' and effects:
                            effects[-1].update(zip(('magnitude', 'area', 'duration'), struct.unpack_from('<fII', v)))
                    r['effects'] = effects
                    v = first(b'SPIT')
                    if len(v) >= 24:
                        r.update(spell_type=uint(v, 8), casting=uint(v, 16), delivery=uint(v, 20))
                if kind == b'MGEF':
                    v = first(b'DATA')
                    if len(v) >= 92:
                        r.update(flags=uint(v), associated=key(uint(v, 8)), skill=uint(v, 12), resist=uint(v, 16),
                                 archetype=uint(v, 64), av=uint(v, 68), projectile=key(uint(v, 72)),
                                 explosion=key(uint(v, 76)), casting=uint(v, 80), delivery=uint(v, 84), av2=uint(v, 88))
                if kind == b'SHOU':
                    r['spells'] += [key(uint(v, 4)) for k, v in subs if k == b'SNAM' and len(v) >= 8]
                if kind in (b'PROJ', b'HAZD', b'EXPL', b'PERK'):
                    r['raw'] = {k.decode(): v.hex() for k, v in subs if k in (b'DATA', b'VMAD')}
                if kind in (b'WEAP', b'EXPL'):
                    r['enchantment'] = ref(b'EITM')
                v = first(b'DATA')
                if kind == b'PROJ' and len(v) >= 40:
                    r.update(projectile_flags=struct.unpack_from('<H',v)[0], projectile_type=struct.unpack_from('<H',v,2)[0], explosion=key(uint(v,36)))
                if kind == b'HAZD' and len(v) >= 28:
                    r['spells'].append(key(uint(v,24)))
                if kind == b'EXPL' and len(v) >= 32:
                    r.update(placed=key(uint(v,16)), projectile=key(uint(v,20)), damage=struct.unpack_from('<f',v,28)[0])
                records[r['key']] = r
        walk(24+size, len(buf))
    for r in records.values():
        r['keywords'] = [records.get(k, {}).get('edid', k) for k in r['keywords']]
    return records, inputs


def main():
    data, output = map(Path, sys.argv[1:3])
    output.mkdir(parents=True, exist_ok=True)
    paths = [data / p for p in ('Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm')]
    paths += sorted(p for p in data.glob('cc*') if p.suffix.lower() in ('.esm', '.esl'))
    paths += [Path(p) for p in sys.argv[3:]]
    records, inputs = scan(paths)
    (output / 'records.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    (output / 'inputs.json').write_text(json.dumps(inputs, indent=2), encoding='utf-8')
    write_roster(records, output)
    print(f'{len(paths)} plugins, {len(records)} resolved records; output: {output}')


def write_roster(records, output):
    """Link attacks, templates, spells, scripts, projectiles and hazards.

    Script references are evidence of possible behavior, not proof that a
    condition or quest will execute it. Include unused/test actors explicitly.
    """
    resolved = {}
    fields = {'race': 1, 'spells': 8, 'inventory': 256, 'script_forms': 512, 'attacks': 2048}
    def variants(key, seen=frozenset()):
        if not key or key in seen: return []
        if key in resolved: return resolved[key]
        r = records.get(key, {})
        if r.get('type') == 'LVLN':
            result = [v for k in r['actors'] for v in variants(k, seen | {key})]
        elif r.get('type') == 'NPC_':
            flags = r.get('template_flags', 0)
            bases = variants(r.get('template'), seen | {key}) if flags & sum(fields.values()) else []
            result = [{field: base.get(field, []) if flags & flag else r.get(field, [])
                       for field, flag in fields.items()} for base in bases] if bases else [{field:r.get(field, []) for field in fields}]
        else: result = []
        # Preserve the correlation between a leveled template's race and its
        # spells. Taking separate unions invents cross-race spell loadouts.
        resolved[key] = list({json.dumps(v, sort_keys=True):v for v in result}.values())
        return resolved[key]

    def closure(roots):
        seen, pending = set(), list(roots)
        while pending:
            key = pending.pop()
            if not key or key in seen: continue
            r = records.get(key, {})
            if r.get('type') not in ('SPEL','SHOU','ENCH','MGEF','PROJ','EXPL','HAZD','WEAP'): continue
            seen.add(key)
            pending.extend(r.get('spells', []))
            pending.extend(e['effect'] for e in r.get('effects', []))
            pending.extend(r.get('script_forms', []))
            pending.extend(r.get(f, '') for f in ('associated','projectile','explosion','placed','enchantment'))
        return sorted(seen)

    roster = {}
    for r in records.values():
        if r['type'] == 'RACE' and 'ActorTypeNPC' not in r['keywords']:
            roster[r['key']] = {'name': r['edid'], 'models': r['models'], 'actors': [],
                                'attacks': list(r['attacks']), 'roots': list(r['spells']) + list(r.get('script_forms', []))}
    for r in records.values():
        if r['type'] != 'NPC_': continue
        for variant in variants(r['key']):
            race = variant.get('race')
            if not isinstance(race, str): continue
            if race not in roster: continue
            v = roster[race]; v['actors'].append(r['key'])
            for field in ('spells','inventory','script_forms'): v['roots'].extend(variant[field])
            v['attacks'].extend(variant['attacks'])
    for v in roster.values():
        v['roots'].extend(a['spell'] for a in v['attacks'])
        v['roots'] = sorted(set(v['roots']) - {''})
        v['attacks'] = list({(a.get('event'), a['spell'], a['flags']): a for a in v['attacks']}.values())
        v['linked_records'] = closure(v['roots'])
    (output / 'creature-roster.json').write_text(json.dumps(roster, indent=2), encoding='utf-8')
    lines = ['# Creature record audit', '', 'Includes unused/test actors and conditional script properties; these links are not live encounter verification.', '']
    for key, v in sorted(roster.items(), key=lambda item:item[1]['name'].lower()):
        lines += [f'## {v["name"]} ({key})', '', f'{len(set(v["actors"]))} actor bases; {len(v["attacks"])} attack variants.', '', '| Attack event | Attached spell |', '| --- | --- |']
        for a in v['attacks']: lines.append(f'| {a.get("event", "")} | {records.get(a["spell"], {}).get("edid", "")} |')
        lines += ['', '| Spell/effect | Archetype | AVs | Delivery/cast | Projectile |', '| --- | --- | --- | --- | --- |']
        for k in v['linked_records']:
            e = records[k]
            if e['type'] != 'MGEF': continue
            lines.append(f'| {e["edid"]} | {e.get("archetype")} | {e.get("av")}/{e.get("av2")} | {e.get("delivery")}/{e.get("casting")} | {records.get(e.get("projectile"),{}).get("edid", "")} |')
        lines.append('')
    (output / 'creature-roster.md').write_text('\n'.join(lines), encoding='utf-8')


if __name__ == '__main__':
    main()
