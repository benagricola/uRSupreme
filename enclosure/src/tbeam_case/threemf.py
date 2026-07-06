"""Minimal 3MF writer.

CadQuery's built in exporter writes one mesh object per file and does
not weld vertices across faces, which slicers report as thousands of
non manifold edges (and a multi part plate becomes one object with
floating regions). This writer welds vertices, emits one 3MF object
per part with its name, and verifies each mesh is watertight (every
edge shared by exactly two triangles).
"""

import zipfile
from pathlib import Path

CONTENT_TYPES = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/'
    'content-types">'
    '<Default Extension="rels" ContentType="application/vnd.'
    'openxmlformats-package.relationships+xml"/>'
    '<Default Extension="model" ContentType="application/vnd.'
    'ms-package.3dmanufacturing-3dmodel+xml"/>'
    "</Types>"
)

RELS = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/'
    '2006/relationships">'
    '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
    'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/'
    '3dmodel"/>'
    "</Relationships>"
)


def _welded_mesh(shape, tolerance: float):
    """Tessellate and weld: returns (points, triangles)."""
    verts, tris = shape.val().tessellate(tolerance)
    index = {}
    points = []
    remap = []
    for v in verts:
        key = (round(v.x, 4), round(v.y, 4), round(v.z, 4))
        if key not in index:
            index[key] = len(points)
            points.append(key)
        remap.append(index[key])
    triangles = []
    for a, b, c in tris:
        ra, rb, rc = remap[a], remap[b], remap[c]
        if ra != rb and rb != rc and ra != rc:
            triangles.append((ra, rb, rc))
    return points, triangles


def _edge_defects(triangles) -> int:
    """Count edges not shared by exactly two triangles."""
    edges = {}
    for a, b, c in triangles:
        for e in ((a, b), (b, c), (c, a)):
            key = (min(e), max(e))
            edges[key] = edges.get(key, 0) + 1
    return sum(1 for n in edges.values() if n != 2)


def write_3mf(path: Path, objects, tolerance: float = 0.08) -> None:
    """Write named shapes as separate objects in one 3MF file.

    objects: iterable of (name, cadquery Workplane).
    """
    resources = []
    items = []
    for i, (name, shape) in enumerate(objects, start=1):
        points, triangles = _welded_mesh(shape, tolerance)
        defects = _edge_defects(triangles)
        status = "watertight" if defects == 0 else f"{defects} edge defects"
        print(f"  {name}: {len(triangles)} triangles, {status}")
        vs = "".join(
            f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in points
        )
        ts = "".join(
            f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in triangles
        )
        resources.append(
            f'<object id="{i}" name="{name}" type="model">'
            f"<mesh><vertices>{vs}</vertices>"
            f"<triangles>{ts}</triangles></mesh></object>"
        )
        items.append(f'<item objectid="{i}"/>')

    model = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<model unit="millimeter" xml:lang="en-US" '
        'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
        f"<resources>{''.join(resources)}</resources>"
        f"<build>{''.join(items)}</build></model>"
    )

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", RELS)
        z.writestr("3D/3dmodel.model", model)
