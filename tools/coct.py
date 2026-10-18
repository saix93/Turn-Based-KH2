"""KH2 room collision (COCT, map/<room>.map entry type 6) for finding open battle spots.
Layout per OpenKh docs/kh2/file/type/coct.md: tables of mesh groups, meshes, collisions (3-4 vertex
indexes, plane, flags), vertices and planes. Coordinates here are game/world coordinates (Y down)."""
import struct

def parse(d):
    assert d[:4] == b'COCT', d[:4]
    offs = struct.unpack_from('<14I', d, 24)
    (o1, l1, o2, l2, o3, l3, o4, l4, o5, l5, o6, l6, o7, l7) = offs
    verts = [struct.unpack_from('<fff', d, o4 + i * 16) for i in range(l4 // 16)]
    planes = [struct.unpack_from('<ffff', d, o5 + i * 16) for i in range(l5 // 16)]
    flags = [struct.unpack_from('<I', d, o7 + i * 4)[0] for i in range(l7 // 4)]
    faces = []
    for i in range(l3 // 16):
        _, a, b, c, e, pl, bb, fl = struct.unpack_from('<hhhhhhhH', d, o3 + i * 16)
        idx = [a, b, c] + ([e] if e >= 0 else [])
        if max(idx) >= len(verts) or pl >= len(planes): continue
        faces.append(([verts[k] for k in idx], planes[pl], flags[fl] if fl < len(flags) else 0))
    return faces

def tris(face):
    v = face[0]
    yield (v[0], v[1], v[2])
    if len(v) == 4: yield (v[0], v[2], v[3])

def _inside(px, pz, t):
    (ax, _, az), (bx, _, bz), (cx, _, cz) = t
    d1 = (px - bx) * (az - bz) - (ax - bx) * (pz - bz)
    d2 = (px - cx) * (bz - cz) - (bx - cx) * (pz - cz)
    d3 = (px - ax) * (cz - az) - (cx - ax) * (pz - az)
    neg = d1 < 0 or d2 < 0 or d3 < 0; pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)

def _height(px, pz, t):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = t
    den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
    if abs(den) < 1e-6: return None
    w1 = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / den
    w2 = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / den
    return w1 * ay + w2 * by + (1 - w1 - w2) * cy

class Room:
    """Floors (near-horizontal faces) and walls (steep faces), bucketed on a 200-unit XZ grid."""
    CELL = 200.0
    def __init__(self, faces):
        self.floors, self.walls = {}, {}
        for f in faces:
            nx, ny, nz, _ = f[1]
            for t in tris(f):
                xs = [p[0] for p in t]; zs = [p[2] for p in t]
                target = self.floors if abs(ny) > 0.7 else self.walls
                for gx in range(int(min(xs) // self.CELL), int(max(xs) // self.CELL) + 1):
                    for gz in range(int(min(zs) // self.CELL), int(max(zs) // self.CELL) + 1):
                        target.setdefault((gx, gz), []).append(t)
    def floor(self, x, z, near_y, tol=120.0):
        """Floor height at (x, z) closest to near_y (within tol), else None."""
        best = None
        for t in self.floors.get((int(x // self.CELL), int(z // self.CELL)), []):
            if _inside(x, z, t):
                h = _height(x, z, t)
                if h is not None and abs(h - near_y) <= tol and (best is None or abs(h - near_y) < abs(best - near_y)):
                    best = h
        return best
    def wall_near(self, x, z, y, radius, height=180.0):
        """Any wall face within radius of (x, z) spanning the body height above floor y (Y down)."""
        for gx in range(int((x - radius) // self.CELL), int((x + radius) // self.CELL) + 1):
            for gz in range(int((z - radius) // self.CELL), int((z + radius) // self.CELL) + 1):
                for t in self.walls.get((gx, gz), []):
                    ys = [p[1] for p in t]
                    if max(ys) < y - height or min(ys) > y - 10: continue  # outside the body's height band
                    for i in range(3):  # distance from point to each edge (XZ)
                        (ax, _, az), (bx, _, bz) = t[i], t[(i + 1) % 3]
                        dx, dz = bx - ax, bz - az; L = dx * dx + dz * dz
                        u = 0 if L < 1e-6 else max(0, min(1, ((x - ax) * dx + (z - az) * dz) / L))
                        qx, qz = ax + u * dx - x, az + u * dz - z
                        if qx * qx + qz * qz < radius * radius: return True
        return False
