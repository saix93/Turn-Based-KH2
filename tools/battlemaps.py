"""Finds battle maps for every world: rooms whose own battle program spawns that world's enemies, and a spot
in each with room for the party line, the enemy line facing it and the camera behind the party, checked
against the room's collision (floor under every slot at one height, no wall near any of them).
Writes sheets/battleMaps.json.   usage: python tools/battlemaps.py"""
import sys, os, glob, json, math, struct, re
sys.path.insert(0, os.path.dirname(__file__))
import egs, ard, coct

KH = r'E:/SteamLibrary/steamapps/common/KINGDOM HEARTS -HD 1.5+2.5 ReMIX-/Image/dt'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORLDS = {'tt': 2, 'hb': 4, 'bb': 5, 'he': 6, 'al': 7, 'mu': 8, 'lk': 10, 'dc': 12, 'wi': 13, 'nm': 14, 'ca': 16, 'tr': 17, 'eh': 18}
GAP, ESPACE, PSPACE, NENEMY, NPARTY = 360.0, 190.0, 170.0, 4, 3
CAM_BACK, CAM_SIDE = 430.0, 150.0
SLOT_CLEAR, FLAT = 110.0, 45.0

pk = [egs.Pkg(h) for h in glob.glob(KH + '/kh2_*.hed')]
def read(n):
    for p in pk:
        if p.has(n): return p.read(n)
objs = ard.objentry(next(p for p in pk if p.has('00objentry.bin')))
names = {}
for line in open(os.path.join(ROOT, '_ref/OpenKh/docs/kh2/worlds.md'), encoding='utf-8'):
    c = [x.strip() for x in line.split('|')]
    if len(c) > 5 and len(c[1]) == 4 and c[1][:2].isalpha() and c[1][2:].isdigit(): names[c[1]] = c[5]

def groups_of(ents, spawns):
    """Enemy groups (type 2 ambush or type 1 normal) a battle program's spawn sets bring in."""
    out = []
    for s in spawns:
        d = ents.get(s)
        if not d or len(d) < 8: continue
        typ, cnt = struct.unpack_from('<ii', d, 0); o = 8
        for _ in range(cnt):
            gt, fl, gid, ne, nev, nwp, rp, nsig = struct.unpack_from('<BBHHHHHi', d, o)
            o2 = o + 0x2C; mobs = []
            for k in range(ne):
                mid = struct.unpack_from('<I', d, o2)[0]; x, y, z = struct.unpack_from('<fff', d, o2 + 4); o2 += 0x40
                ty, nm = objs.get(mid, (None, '?'))
                if ty == 4 and not nm.startswith(('F_', 'N_')): mobs.append((mid, nm, x, y, z))
            o = o2 + nev * 0x40
            for _ in range(nwp):
                c = struct.unpack_from('<H', d, o + 2)[0]; o += 8 + 12 * c
            o += rp * 0x10 + nsig * 8
            if len(mobs) >= 2 and gt in (1, 2): out.append((s, gid, gt, mobs))
    return out

def story_program(ents, spawns):
    """True when a battle program spawns a player or party actor (a scripted story fight with its own copies of
    Sora, Donald and Goofy) or a boss: never a place for a random encounter."""
    for s in spawns:
        d = ents.get(s)
        if not d or len(d) < 8: continue
        typ, cnt = struct.unpack_from('<ii', d, 0); o = 8
        for _ in range(cnt):
            gt, fl, gid, ne, nev, nwp, rp, nsig = struct.unpack_from('<BBHHHHHi', d, o)
            o2 = o + 0x2C
            for k in range(ne):
                mid = struct.unpack_from('<I', d, o2)[0]; o2 += 0x40
                if objs.get(mid, (None, '?'))[0] in (0, 1, 3): return True
            o = o2 + nev * 0x40
            for _ in range(nwp):
                c = struct.unpack_from('<H', d, o + 2)[0]; o += 8 + 12 * c
            o += rp * 0x10 + nsig * 8
    return False

def fits(room, cx, cy, cz, dx, dz):
    """Floor height of the spot if both lines and the camera fit, else None. Returns (y, clearance)."""
    rx, rz = dz, -dx
    pts = []
    for i in range(NPARTY): o = (i - (NPARTY - 1) / 2) * PSPACE; pts.append((cx - dx * GAP / 2 + rx * o, cz - dz * GAP / 2 + rz * o))
    for i in range(NENEMY): o = (i - (NENEMY - 1) / 2) * ESPACE; pts.append((cx + dx * GAP / 2 + rx * o, cz + dz * GAP / 2 + rz * o))
    for k in range(-2, 3): pts.append((cx + dx * GAP * k / 4, cz + dz * GAP * k / 4))  # the ground between the lines
    y0 = room.floor(cx, cz, cy, 300)
    if y0 is None: return None
    for (x, z) in pts:
        y = room.floor(x, z, y0, FLAT)
        if y is None or room.wall_near(x, z, y, SLOT_CLEAR): return None
    # The camera behind the party: open air there (no wall at head height around it).
    camx, camz = cx - dx * (GAP / 2 + CAM_BACK) + rx * CAM_SIDE, cz - dz * (GAP / 2 + CAM_BACK) + rz * CAM_SIDE
    if room.wall_near(camx, camz, y0, 60, height=400): return None
    clear = 0
    for extra in (60, 120, 180):  # bonus: how much more room there is around the slots
        if all(not room.wall_near(x, z, y0, SLOT_CLEAR + extra) for (x, z) in pts): clear = extra
    return y0, clear

def best_spot(room, g):
    mobs = g[3]
    gx = sum(m[2] for m in mobs) / len(mobs); gz = sum(m[4] for m in mobs) / len(mobs)
    gy = sorted(m[3] for m in mobs)[len(mobs) // 2]
    best = None
    for ox in range(-400, 401, 100):
        for oz in range(-400, 401, 100):
            for a in range(16):
                dx, dz = math.sin(a * math.pi / 8), math.cos(a * math.pi / 8)
                r = fits(room, gx + ox, gy, gz + oz, dx, dz)
                if not r: continue
                y, clear = r
                score = clear * 3 - math.hypot(ox, oz) * 0.5
                if not best or score > best[0]: best = (score, gx + ox, y, gz + oz, dx, dz)
    return best, (gx, gy, gz)

rows = []
for w, wid in WORLDS.items():
    found = []
    for r in range(0, 60):
        rn = '%s%02d' % (w, r)
        a = read('ard/us/%s.ard' % rn)
        m = read('map/%s.map' % rn)
        if not a or not m: continue
        ents = {n: e for t, n, e in ard.bar(a) if n.startswith('b_')}
        cc = [e for t, n, e in ard.bar(m) if t == 6]
        if not cc: continue
        room = coct.Room(coct.parse(cc[0]))
        progs = {}
        for t, n, e in ard.bar(a):
            if n == 'btl':
                for pid, ops in ard.scripts(e).items():
                    progs[pid] = [p[:4].rstrip(b'\0').decode('latin1') for op, p in ops if op == 0 and len(p) >= 4]
        seen = set()
        for pid in sorted(progs, key=lambda p: (p > 9, p)):  # the room's ordinary programs first
            if story_program(ents, progs[pid]): continue  # a scripted fight: brings its own party copies or a boss
            for g in groups_of(ents, progs[pid]):
                key = (g[0], g[1])
                if key in seen: continue
                seen.add(key)
                spot, trig = best_spot(room, g)
                if spot:
                    found.append(dict(score=spot[0], room=r, btl=pid, group=g[1], trig=trig, spot=spot, foes=sorted({m[0] for m in g[3]}), models=sorted({m[1] for m in g[3]})))
        print(w, rn, 'checked', file=sys.stderr)
    # The best spot of every room with enemies: encounters are fought in the room the party is in.
    found.sort(key=lambda f: -f['score'])
    used = set(); n = 0
    for f in found:
        if f['room'] in used: continue
        if any(m.startswith('B_') for m in f['models']) or abs(f['spot'][2]) > 3000: continue
        used.add(f['room']); n += 1
        rn = '%s%02d' % (w, f['room'])
        sx, sy, sz, dx, dz = f['spot'][1:]
        rows.append({"key": "%s_g%d" % (rn, f['group']), "world": wid, "room": f['room'], "roomName": re.sub(r"\{[^}]*\}", "", names.get(rn, rn)).replace("´", "'").strip(), "btlProgram": f['btl'], "door": 0,
                     "triggerX": round(f['trig'][0]), "triggerY": round(f['trig'][1]), "triggerZ": round(f['trig'][2]),
                     "spotX": round(sx), "spotY": round(sy), "spotZ": round(sz), "dirX": round(dx, 3), "dirZ": round(dz, 3),
                     "foeIds": f['foes'], "foeModels": f['models'], "verified": False, "waves": 1})
    print(w, 'spots', n, file=sys.stderr)
json.dump({"sheet": "battleMaps",
           "about": "Where random encounters are fought: one open spot per room that has enemies (FFX-style battle maps). An encounter in a room is fought at that room's spot; test mode, starting in a room without enemies, travels to the nearest room of the same world that has one. Each row is a room and battle program whose own spawn group brings in that world's enemies, and a spot (spotX/Y/Z = the middle between the lines, dir = from the party toward the enemies) where the party line, the enemy line and the camera behind the party fit: found by tools/battlemaps.py against the room's collision (floor under every slot at one height, no wall within 110 units of any slot). trigger is the spawn group's centre: the player stands there for a moment so an ambush group appears. verified: seen in the game by a test run.",
           "columns": ["key", "world", "room", "roomName", "btlProgram", "door", "triggerX", "triggerY", "triggerZ", "spotX", "spotY", "spotZ", "dirX", "dirZ", "waves", "foeIds", "foeModels", "verified"],
           "rows": rows}, open(os.path.join(ROOT, 'sheets/battleMaps.json'), 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print(len(rows), 'battle maps')
