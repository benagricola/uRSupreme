"""The enclosure parts: front shell, back shell, battery hatch.

Uses the board coordinate frame from params.py. The parting line sits
at Z_PART, just below the PCB underside, so every critical opening
(screen window, button flexures, USB-C, microSD, SMA) lives in the
front shell and nothing has to line up across the seam.

Closure: two hook tabs at the USB end on the back shell snap into
windows in the front shell side walls, then two M3x10 SHCS driven
from the front face at the antenna end thread into pilot holes in
the back shell bosses.

Every part prints without supports: front shell face down, back
shell hump down, hatch outer face down. All face level changes go
through 45 degree surfaces (window funnel, back hull, hatch bevels,
cap and nub chamfers, SMA teardrop, cone screw seats). Two known
micro overhangs remain by design, both under 1.5 mm and bridgeable:
the hook tab nose undersides and the hatch fixed tab arms.

Assembly order: the board goes into the front shell antenna first
(the board mounted SMA barrel enters its wall hole), then the USB end
swings down onto the locating pins. The board is clamped between the
front shell pedestals and the back shell posts; the antenna end rests
on its factory corner screws under the front shell stubs.

The battery hatch is a lift-out lid in the grip hump floor: two fixed
tabs hook over the ledge at the antenna end, snap bumps hold the
other end. The 18650 is retained by the holder's own spring clips,
so the hatch only keeps dust out and needs no strong latch.
"""

import cadquery as cq

from . import params as p

# Walls and parting line
WALL = 2.4
Z_PART = -3.0

# Cavity (interior) extents around the board
CAV_X_MIN = -0.8
CAV_X_MAX = p.PCB_W + 2.0  # button side gap, flexure nubs reach across
CAV_Y_MIN = -8.0  # pocket at the antenna end: SMA nut plus screw bosses
CAV_Y_MAX = p.PCB_L + 1.0

# Outer envelope
OUT_X_MIN = CAV_X_MIN - WALL
OUT_X_MAX = CAV_X_MAX + WALL
OUT_Y_MIN = CAV_Y_MIN - WALL
OUT_Y_MAX = CAV_Y_MAX + WALL
OUTER_R = 3.5  # vertical corner fillet
CAV_R = 2.0

# Front shell. Printed face down without supports, so the outer face
# is a single flat plane; every recessed feature reaches it through
# 45 degree surfaces. Interior is one level (FRONT_IN) clearing the
# M.2 stack; a monolithic bezel block descends to 9.0 over the screen
# (clearing the GPIO headers at 8.6 and the floating acrylic at 6.9)
# and carries the window aperture at the bottom of a 45 degree
# funnel.
FRONT_IN = 12.2
FACE = FRONT_IN + WALL
BEZEL_X = (5.4, 27.6)
BEZEL_Y = (6.8, 38.7)
BEZEL_Z = (9.0, FRONT_IN + 0.1)
APERTURE_TOP = 10.2  # vertical aperture walls from 9.0 up to here

# Back shell: printed hump down without supports. The outer surface
# is a boat hull: flat only at the hump bottom (the bed), rising at
# 45 degrees on all sides until it meets the vertical perimeter
# walls, so there is no flat pan face anywhere. The 45 degree swell
# doubles as the palm grip.
PAN_IN = -5.0  # interior floor over the solder side
HUMP_IN = -17.6
HUMP_OUT = HUMP_IN - WALL
HUMP_X_MIN, HUMP_X_MAX = 2.55, 30.35
HUMP_Y_MIN, HUMP_Y_MAX = 5.6, 94.4
HUMP_CAV_X_MIN, HUMP_CAV_X_MAX = 4.45, 28.45
HUMP_CAV_Y_MIN, HUMP_CAV_Y_MAX = 7.5, 92.5
HULL_RISE = (Z_PART + 0.1) - HUMP_OUT  # 45 degree run from hump to rim

# OLED window, cut through the valley floor with an outward chamfer.
# The active area Y position is not confirmed; the window may need to
# shift once measured on hardware.
WIN_X_MIN, WIN_X_MAX = 7.8, 25.2
WIN_Y_MIN, WIN_Y_MAX = 9.5, 36.0
WIN_CHAMFER = 1.6  # per side growth at the outer face

# Button flexures: cantilever tabs in the button side wall, anchored
# at the top, nub reaching in to the switch plunger, cap outside.
# Caps and nubs carry 45 degree chamfers on their face side edge so
# they print without supports face down.
BTN_TAB_HALF = 2.5  # tab half width along Y
BTN_SLOT = 1.0  # slot around the tab
BTN_SLOT_TOP = 8.0  # slots reach up to here, tab hangs above
BTN_RECESS_TOP = 7.0  # inner thinning recess top (anchor stays thick)
BTN_TAB_T = 1.2  # thinned tab wall
BTN_NUB_TIP_X = 34.6  # nub tip, 0.2 short of the plunger face
BTN_NUB_HALF = 1.5
BTN_NUB_Z = (0.4, 3.1)
BTN_CAP_HALF = 2.0  # cap half width along Y
BTN_CAP_Z = (0.5, 3.0)
BTN_CAP_PROTRUDE = {"reset": 0.6, "power": 1.8, "boot": 1.8}

# USB-C and microSD openings in the bottom wall. They extend down to
# the parting line (open bottomed notches, closed by the back shell
# rim) so the face down print has no bridges there.
USB_CUT_X = (3.8, 14.2)
USB_CUT_Z_TOP = 4.2
USB_SCOOP_X = (2.6, 15.4)
USB_SCOOP_Z_TOP = 5.4
SD_CUT_X = (15.4, 27.4)
SD_CUT_Z_TOP = 2.6
SD_SCOOP_X = (17.0, 25.8)
SD_SCOOP_Z_TOP = 3.6
SCOOP_DEPTH = 1.2  # outer face recess depth for both scoops

# SMA: round hole in the front top wall meeting the board mounted
# connector, teardropped toward +Z (the overhang side when printed
# face down); the nut and washer clamp outside the wall
SMA_HOLE_R = 3.4

# Closure: M3x10 SHCS driven from the FRONT face at the antenna end
# (deep hex wells with 45 degree cone seats, so neither shell needs a
# flat counterbore overhang), hook tabs at the USB end. The screw
# passes a clearance bore in the front shell and thread forms into
# the back shell pilot.
BOSS_XY = [(20.0, -4.6), (31.0, -4.6)]
BOSS_R = 4.5
WELL_R = 3.2  # head well from the face down to the cone seat
CONE_Z = (0.0, 1.5)  # 45 degree seat, well radius down to shaft
SHAFT_R = 1.7
PILOT_R = 1.35  # thread forming M3 pilot in the back shell
BACK_BOSS_BOT = -9.6  # embedded in the back shell end slope
PILOT_BOT = -9.4

HOOK_Y = (91.5, 98.5)  # hook tab extent along Y
HOOK_WIN_Y = (91.3, 98.7)
HOOK_WIN_Z = (0.3, 2.0)
HOOK_RECESS_Y = (90.5, 99.5)
HOOK_TAB_T = 1.2
HOOK_NOSE = 0.75

# Board clamp: pedestals from the front, posts from the back.
# At the antenna end the stubs land on the factory corner screw
# heads (SCREWHEAD_TOP); at the USB end full towers with locating
# pins press directly on the PCB.
PED_UPPER_R = 2.75
PED_LOWER_R = 1.6  # slimmer near the board, clears USB and SD bodies
PED_LOWER_TOP = 3.6
PIN_R = 0.85
PIN_LEN = 1.4
STUB_R = 1.8
POST_R = 2.5
POST_TOP = -1.7  # 0.1 below the PCB underside at rest

# Battery hatch: opening A goes straight through the hump floor (no
# recess: a recess ledge is an unprintable flat overhang ring when
# the shell prints hump down). The hatch outer plate sits proud on
# the hump bottom with 45 degree beveled edges; the floor around A
# is the retention ledge the tabs hook over from inside. Clearance
# sized for PETG.
HATCH_A_X = (6.2, 26.7)
HATCH_A_Y = (16.5, 83.5)
HATCH_PLATE_X = (4.4, 28.5)
HATCH_PLATE_Y = (14.7, 85.3)
HATCH_PROUD = 1.2  # plate thickness outside the hump bottom
HATCH_BEVEL = 1.2  # 45 degree edge bevel on the proud plate
HATCH_CLR = 0.4
HATCH_TAB_XS = [(9.0, 13.0), (20.0, 24.0)]
HATCH_BUMP_Y = [30.0, 70.0]
# thumb notch: 45 degree V groove beyond the hatch bottom edge
THUMB_Y = (86.5, 90.5)
THUMB_APEX_Z = HUMP_OUT + 2.0
THUMB_X = (10.0, 23.0)


def _box(x0, x1, y0, y1, z0, z1) -> cq.Workplane:
    return (
        cq.Workplane("XY")
        .box(x1 - x0, y1 - y0, z1 - z0, centered=False)
        .translate((x0, y0, z0))
    )


def _rbox(x0, x1, y0, y1, z0, z1, r) -> cq.Workplane:
    return _box(x0, x1, y0, y1, z0, z1).edges("|Z").fillet(r)


def _ycyl(x, z, r, y0, y1) -> cq.Workplane:
    """Cylinder along Y."""
    return (
        cq.Workplane("XZ")
        .center(x, z)
        .circle(r)
        .extrude(y1 - y0)
        .translate((0, y1, 0))
    )


def _zcyl(x, y, r, z0, z1) -> cq.Workplane:
    """Cylinder along Z."""
    return (
        cq.Workplane("XY")
        .center(x, y)
        .circle(r)
        .extrude(z1 - z0)
        .translate((0, 0, z0))
    )


def _xcyl(y, z, r, x0, x1) -> cq.Workplane:
    """Cylinder along X."""
    return (
        cq.Workplane("YZ", origin=(x0, 0, 0))
        .center(y, z)
        .circle(r)
        .extrude(x1 - x0)
    )


def _xz_prism(points, y0, y1) -> cq.Workplane:
    """Polygon in the XZ plane extruded from y0 to y1."""
    return (
        cq.Workplane("XZ", origin=(0, y1, 0))
        .polyline(points)
        .close()
        .extrude(y1 - y0)
    )


def _yz_prism(points, x0, x1) -> cq.Workplane:
    """Polygon in the YZ plane extruded from x0 to x1."""
    return (
        cq.Workplane("YZ", origin=(x0, 0, 0))
        .polyline(points)
        .close()
        .extrude(x1 - x0)
    )


def _rect_loft(cx, cy, w0, h0, z0, w1, h1, z1) -> cq.Workplane:
    """Loft between two centered rectangles at different heights."""
    return (
        cq.Workplane("XY", origin=(cx, cy, z0))
        .rect(w0, h0)
        .workplane(offset=z1 - z0)
        .rect(w1, h1)
        .loft()
    )


def front_shell() -> cq.Workplane:
    win_cx = (WIN_X_MIN + WIN_X_MAX) / 2
    win_cy = (WIN_Y_MIN + WIN_Y_MAX) / 2
    win_w = WIN_X_MAX - WIN_X_MIN
    win_h = WIN_Y_MAX - WIN_Y_MIN

    outer = _rbox(
        OUT_X_MIN, OUT_X_MAX, OUT_Y_MIN, OUT_Y_MAX, Z_PART, FACE, OUTER_R
    )
    shell = outer.cut(
        _rbox(CAV_X_MIN, CAV_X_MAX, CAV_Y_MIN, CAV_Y_MAX,
              Z_PART - 1, FRONT_IN, CAV_R)
    )

    # bezel block descending over the screen, then the 45 degree
    # funnel and the vertical walled aperture cut through it
    shell = shell.union(_box(*BEZEL_X, *BEZEL_Y, *BEZEL_Z))
    funnel_g = 2 * (FACE + 0.1 - APERTURE_TOP)
    shell = shell.cut(
        _rect_loft(win_cx, win_cy, win_w, win_h, APERTURE_TOP,
                   win_w + funnel_g, win_h + funnel_g, FACE + 0.1)
    )
    shell = shell.cut(
        _box(WIN_X_MIN, WIN_X_MAX, WIN_Y_MIN, WIN_Y_MAX,
             BEZEL_Z[0] - 0.2, APERTURE_TOP + 0.1)
    )

    # SMA hole in the top wall, teardropped toward +Z for the face
    # down print
    shell = shell.cut(
        _ycyl(p.SMA_CENTER_X, p.SMA_CENTER_Z, SMA_HOLE_R, OUT_Y_MIN - 2, CAV_Y_MIN + 2)
    )
    # teardrop apex points to -Z: printed face down, the bore's -Z
    # side is the physical ceiling that would otherwise sag
    t = SMA_HOLE_R / 1.4142
    shell = shell.cut(
        _xz_prism(
            [(p.SMA_CENTER_X - t, p.SMA_CENTER_Z - t),
             (p.SMA_CENTER_X + t, p.SMA_CENTER_Z - t),
             (p.SMA_CENTER_X, p.SMA_CENTER_Z - SMA_HOLE_R * 1.4142)],
            OUT_Y_MIN - 2, CAV_Y_MIN + 2,
        )
    )

    # USB-C and microSD notches (open to the parting line) plus outer
    # finger scoops
    yw0, yw1 = CAV_Y_MAX, OUT_Y_MAX
    zb = Z_PART - 0.1
    shell = shell.cut(_box(*USB_CUT_X, yw0 - 0.5, yw1 + 1, zb, USB_CUT_Z_TOP))
    shell = shell.cut(
        _box(*USB_SCOOP_X, yw1 - SCOOP_DEPTH, yw1 + 1, zb, USB_SCOOP_Z_TOP)
    )
    shell = shell.cut(_box(*SD_CUT_X, yw0 - 0.5, yw1 + 1, zb, SD_CUT_Z_TOP))
    shell = shell.cut(
        _box(*SD_SCOOP_X, yw1 - SCOOP_DEPTH, yw1 + 1, zb, SD_SCOOP_Z_TOP)
    )

    # Button flexures
    for name, yc in p.BUTTON_Y.items():
        # slots around the tab, open at the parting line
        for ys in (yc - BTN_TAB_HALF - BTN_SLOT, yc + BTN_TAB_HALF):
            shell = shell.cut(
                _box(CAV_X_MAX - 0.2, OUT_X_MAX + 0.2, ys, ys + BTN_SLOT,
                     Z_PART - 0.1, BTN_SLOT_TOP)
            )
        # thin the tab from the inside, leaving a thick anchor at the top
        shell = shell.cut(
            _box(CAV_X_MAX, OUT_X_MAX - BTN_TAB_T,
                 yc - BTN_TAB_HALF, yc + BTN_TAB_HALF,
                 Z_PART - 0.1, BTN_RECESS_TOP)
        )
        # nub reaching in toward the switch plunger, 45 degree chamfer
        # on its +Z edge
        tab_face = OUT_X_MAX - BTN_TAB_T + 0.1
        rise = tab_face - BTN_NUB_TIP_X
        shell = shell.union(
            _xz_prism(
                [(tab_face, BTN_NUB_Z[0]),
                 (BTN_NUB_TIP_X, BTN_NUB_Z[0]),
                 (BTN_NUB_TIP_X, BTN_NUB_Z[1]),
                 (tab_face, BTN_NUB_Z[1] + rise)],
                yc - BTN_NUB_HALF, yc + BTN_NUB_HALF,
            )
        )
        # external cap, 45 degree chamfer on its +Z edge
        prot = BTN_CAP_PROTRUDE[name]
        shell = shell.union(
            _xz_prism(
                [(OUT_X_MAX - 0.1, BTN_CAP_Z[0]),
                 (OUT_X_MAX + prot, BTN_CAP_Z[0]),
                 (OUT_X_MAX + prot, BTN_CAP_Z[1]),
                 (OUT_X_MAX - 0.1, BTN_CAP_Z[1] + prot + 0.1)],
                yc - BTN_CAP_HALF, yc + BTN_CAP_HALF,
            )
        )

    # Hook recesses and windows in both side walls near the USB end
    for inner, sign in ((CAV_X_MIN, -1), (CAV_X_MAX, 1)):
        recess_deep = inner + sign * (HOOK_TAB_T + 0.05)
        shell = shell.cut(
            _box(min(inner - sign * 0.05, recess_deep),
                 max(inner - sign * 0.05, recess_deep),
                 *HOOK_RECESS_Y, Z_PART - 0.1, 2.3)
        )
        win_out = inner + sign * (WALL + 1)
        shell = shell.cut(
            _box(min(recess_deep, win_out), max(recess_deep, win_out),
                 *HOOK_WIN_Y, *HOOK_WIN_Z)
        )

    # Screw bosses at the antenna end: head well from the face, 45
    # degree cone seat, clearance shaft down to the parting line
    for bx, by in BOSS_XY:
        shell = shell.union(_zcyl(bx, by, BOSS_R, Z_PART, FRONT_IN + 0.1))
        shell = shell.cut(_zcyl(bx, by, WELL_R, CONE_Z[1], FACE + 0.1))
        shell = shell.cut(
            cq.Workplane("XY", origin=(bx, by, CONE_Z[0]))
            .circle(SHAFT_R)
            .workplane(offset=CONE_Z[1] - CONE_Z[0])
            .circle(WELL_R)
            .loft()
        )
        shell = shell.cut(_zcyl(bx, by, SHAFT_R, Z_PART - 0.1, CONE_Z[0] + 0.1))

    # Board pedestals: full towers with locating pins at the USB end,
    # short stubs landing on the factory corner screws at the antenna
    # end
    for hx, hy in p.MOUNT_HOLES:
        if hy > 50:
            shell = shell.union(_zcyl(hx, hy, PED_UPPER_R, PED_LOWER_TOP,
                                      FRONT_IN + 0.1))
            shell = shell.union(_zcyl(hx, hy, PED_LOWER_R, 0, PED_LOWER_TOP + 0.1))
            shell = shell.union(_zcyl(hx, hy, PIN_R, -PIN_LEN, 0.1))
        else:
            shell = shell.union(_zcyl(hx, hy, STUB_R, p.SCREWHEAD_TOP + 0.1,
                                      FRONT_IN + 0.1))

    return shell


def _hook_tab(inner: float, sign: int) -> cq.Workplane:
    """A hook tab rising from the back shell wall top, nose outward."""
    t0 = inner
    t1 = inner + sign * HOOK_TAB_T
    tab = _box(min(t0, t1), max(t0, t1), *HOOK_Y, -4.0, 1.9)
    # nose: flat catch face at the bottom, insertion ramp on top
    n1 = t1 + sign * HOOK_NOSE
    nose = _box(min(t1, n1), max(t1, n1), *HOOK_Y, 0.4, 1.0)
    cx = (min(t1, n1) + max(t1, n1)) / 2
    ramp = (
        cq.Workplane("XY", origin=(cx, (HOOK_Y[0] + HOOK_Y[1]) / 2, 1.0))
        .rect(HOOK_NOSE, HOOK_Y[1] - HOOK_Y[0])
        .workplane(offset=0.9)
        .center(-sign * HOOK_NOSE / 2 + sign * 0.025, 0)
        .rect(0.05, HOOK_Y[1] - HOOK_Y[0])
        .loft()
    )
    return tab.union(nose).union(ramp)


def back_shell() -> cq.Workplane:
    # boat hull: vertical walled prism intersected with a 45 degree
    # frustum flaring up from the hump footprint
    prism = _rbox(OUT_X_MIN, OUT_X_MAX, OUT_Y_MIN, OUT_Y_MAX,
                  HUMP_OUT, Z_PART, OUTER_R)
    hump_cx = (HUMP_X_MIN + HUMP_X_MAX) / 2
    hump_cy = (HUMP_Y_MIN + HUMP_Y_MAX) / 2
    hump_w = HUMP_X_MAX - HUMP_X_MIN
    hump_h = HUMP_Y_MAX - HUMP_Y_MIN
    frustum = _rect_loft(
        hump_cx, hump_cy, hump_w, hump_h, HUMP_OUT,
        hump_w + 2 * HULL_RISE, hump_h + 2 * HULL_RISE, Z_PART + 0.1,
    )
    shell = prism.intersect(frustum)

    shell = shell.cut(
        _rbox(CAV_X_MIN, CAV_X_MAX, CAV_Y_MIN, CAV_Y_MAX, PAN_IN, Z_PART + 1, CAV_R)
    )
    shell = shell.cut(
        _rbox(HUMP_CAV_X_MIN, HUMP_CAV_X_MAX, HUMP_CAV_Y_MIN, HUMP_CAV_Y_MAX,
              HUMP_IN, PAN_IN + 1, CAV_R)
    )

    # Battery hatch opening straight through the hump floor
    shell = shell.cut(_box(*HATCH_A_X, *HATCH_A_Y, HUMP_OUT - 2, HUMP_IN + 0.1))
    # snap bump dimples in the A opening side faces
    for by in HATCH_BUMP_Y:
        for ax in HATCH_A_X:
            shell = shell.cut(
                _xcyl(by, (HUMP_OUT + HUMP_IN) / 2, 0.8, ax - 0.6, ax + 0.6)
            )
    # thumb notch: 45 degree V groove beyond the hatch bottom edge
    shell = shell.cut(
        _yz_prism(
            [(THUMB_Y[0], HUMP_OUT - 0.1),
             (THUMB_Y[1], HUMP_OUT - 0.1),
             ((THUMB_Y[0] + THUMB_Y[1]) / 2, THUMB_APEX_Z)],
            *THUMB_X,
        )
    )

    # Screw bosses embedded in the end slope: pilot holes only, the
    # heads live in the front shell wells. Trimmed to the hull so
    # nothing pokes through the sloped exterior.
    for bx, by in BOSS_XY:
        boss = _zcyl(bx, by, BOSS_R, BACK_BOSS_BOT, Z_PART).intersect(frustum)
        shell = shell.union(boss)
        shell = shell.cut(_zcyl(bx, by, PILOT_R, PILOT_BOT, Z_PART + 0.1))

    # Rim tongues rising into the front shell port notches, closing
    # the gap below the connectors
    shell = shell.union(_box(4.1, 13.9, CAV_Y_MAX + 0.2, 102.2, Z_PART - 0.5, -0.5))
    shell = shell.union(_box(15.7, 27.1, CAV_Y_MAX + 0.2, 102.2, Z_PART - 0.5, -1.0))

    # Board clamp posts
    for hx, hy in p.MOUNT_HOLES:
        shell = shell.union(_zcyl(hx, hy, POST_R, PAN_IN - 0.1, POST_TOP))

    # Hook tabs at the USB end
    shell = shell.union(_hook_tab(CAV_X_MIN, -1))
    shell = shell.union(_hook_tab(CAV_X_MAX, 1))

    return shell


def battery_hatch() -> cq.Workplane:
    ax0, ax1 = HATCH_A_X[0] + HATCH_CLR, HATCH_A_X[1] - HATCH_CLR
    ay0, ay1 = HATCH_A_Y[0] + HATCH_CLR, HATCH_A_Y[1] - HATCH_CLR
    p_cx = (HATCH_PLATE_X[0] + HATCH_PLATE_X[1]) / 2
    p_cy = (HATCH_PLATE_Y[0] + HATCH_PLATE_Y[1]) / 2
    p_w = HATCH_PLATE_X[1] - HATCH_PLATE_X[0]
    p_h = HATCH_PLATE_Y[1] - HATCH_PLATE_Y[0]

    # proud outer plate, 45 degree bevels growing from the bed face
    outer_plate = _rect_loft(
        p_cx, p_cy,
        p_w - 2 * HATCH_BEVEL, p_h - 2 * HATCH_BEVEL,
        HUMP_OUT - HATCH_PROUD,
        p_w, p_h,
        HUMP_OUT - 0.05,
    )
    inner_plate = _box(ax0, ax1, ay0, ay1, HUMP_OUT - 0.1, HUMP_IN - 0.1)
    hatch = outer_plate.union(inner_plate)

    # fixed tabs hooking over the floor ledge at the antenna end
    for tx0, tx1 in HATCH_TAB_XS:
        root = _box(tx0, tx1, ay0, ay0 + 2.0, HUMP_IN - 0.1, HUMP_IN + 0.65)
        arm = _box(tx0, tx1, ay0 - 1.45, ay0 + 2.0, HUMP_IN + 0.15, HUMP_IN + 0.65)
        hatch = hatch.union(root).union(arm)

    # snap bumps on the inner plate side faces, protruding 0.45 into
    # the dimples cut in the opening walls
    zc = (HUMP_OUT + HUMP_IN) / 2
    for by in HATCH_BUMP_Y:
        hatch = hatch.union(_xcyl(by, zc, 0.7, ax0 - 0.45, ax0 + 0.6))
        hatch = hatch.union(_xcyl(by, zc, 0.7, ax1 - 0.6, ax1 + 0.45))

    return hatch


def parts() -> dict:
    return {
        "front_shell": front_shell(),
        "back_shell": back_shell(),
        "battery_hatch": battery_hatch(),
    }
