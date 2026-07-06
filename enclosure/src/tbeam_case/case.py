"""The enclosure parts: front shell, back shell, battery hatch.

Uses the board coordinate frame from params.py. The parting line sits
at Z_PART, just below the PCB underside, so every critical opening
(screen window, button flexures, USB-C, microSD, SMA) lives in the
front shell and nothing has to line up across the seam.

Closure: two hook tabs at the USB end on the back shell snap into
windows in the front shell side walls, then two M3x10 SHCS at the
antenna end thread into pilot holes in the front shell bosses.

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

# Front shell. The screen zone interior sits at 9.0 to clear the GPIO
# header rows (8.6) and the floating acrylic (6.9); the screen shows
# through a recessed valley between two side rails over the headers,
# so the window itself stays shallow (2.6 from acrylic to face).
# The M.2 zone gets the full height plateau.
FACE_STEP_Y = 40.6  # outer step position; inner step 0.6 further on
FRONT_IN_LOW = 9.0  # inner face over the screen zone
FRONT_IN_HIGH = 12.2  # inner face over the M.2 zone
FRONT_OUT_LOW = FRONT_IN_LOW + WALL
FRONT_OUT_HIGH = FRONT_IN_HIGH + WALL
VALLEY_X = (7.2, 25.8)
VALLEY_Y = (9.0, FACE_STEP_Y)
VALLEY_FLOOR_OUT = 9.5  # outer surface of the recessed valley floor
VALLEY_FLOOR_IN = 7.1  # inner surface, 0.2 above the acrylic

# Back shell: shallow pan (solder side clearance) plus grip hump over
# the 18650 holder.
PAN_IN = -5.0
PAN_OUT = PAN_IN - WALL
HUMP_IN = -17.6
HUMP_OUT = HUMP_IN - WALL
HUMP_X_MIN, HUMP_X_MAX = 2.55, 30.35
HUMP_Y_MIN, HUMP_Y_MAX = 5.6, 94.4
HUMP_CAV_X_MIN, HUMP_CAV_X_MAX = 4.45, 28.45
HUMP_CAV_Y_MIN, HUMP_CAV_Y_MAX = 7.5, 92.5

# OLED window, cut through the valley floor with an outward chamfer.
# The active area Y position is not confirmed; the window may need to
# shift once measured on hardware.
WIN_X_MIN, WIN_X_MAX = 7.8, 25.2
WIN_Y_MIN, WIN_Y_MAX = 9.5, 36.0
WIN_CHAMFER = 1.6  # per side growth at the outer face

# Button flexures: cantilever tabs in the button side wall, anchored
# at the top, nub reaching in to the switch plunger, cap outside.
BTN_TAB_HALF = 2.5  # tab half width along Y
BTN_SLOT = 1.0  # slot around the tab
BTN_SLOT_TOP = 8.0  # slots reach up to here, tab hangs above
BTN_RECESS_TOP = 7.0  # inner thinning recess top (anchor stays thick)
BTN_TAB_T = 1.2  # thinned tab wall
BTN_NUB_TIP_X = 34.6  # nub tip, 0.2 short of the plunger face
BTN_NUB_HALF = 1.5
BTN_NUB_Z = (0.4, 3.1)
BTN_CAP_R = 2.2
BTN_CAP_Z = 1.75
BTN_CAP_PROTRUDE = {"reset": 0.6, "power": 1.8, "boot": 1.8}

# USB-C and microSD openings in the bottom wall
USB_CUT_X = (3.8, 14.2)
USB_CUT_Z = (-0.9, 4.2)
USB_SCOOP_X = (2.6, 15.4)
USB_SCOOP_Z = (-2.1, 5.4)
SD_CUT_X = (15.4, 27.4)
SD_CUT_Z = (-0.5, 2.6)
SD_SCOOP_X = (17.0, 25.8)
SD_SCOOP_Z = (-1.5, 3.6)
SCOOP_DEPTH = 1.2  # outer face recess depth for both scoops

# SMA: round hole in the front top wall meeting the board mounted
# connector; the nut and washer clamp outside the wall
SMA_HOLE_R = 3.4

# Closure: M3 bosses at the antenna end, hook tabs at the USB end
BOSS_XY = [(20.0, -4.6), (31.0, -4.6)]
BOSS_R = 4.5
PILOT_R = 1.35  # thread forming M3 pilot
PILOT_TOP = 5.3  # pilot reaches just into the front face plate
SHAFT_R = 1.7
HEAD_CB_R = 3.2
HEAD_CB_TOP = -4.8  # counterbore depth tuned for M3x10 engagement

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

# Battery hatch: opening A goes through the hump floor, opening B is
# a recess for the hatch outer plate, the rim between them is the
# retention ledge.
HATCH_A_X = (6.2, 26.7)
HATCH_A_Y = (16.5, 83.5)
HATCH_B_X = (4.7, 28.2)
HATCH_B_Y = (14.0, 86.0)
HATCH_B_Z_TOP = HUMP_OUT + 1.2  # recess depth = plate thickness
HATCH_CLR = 0.3
HATCH_TAB_XS = [(9.0, 13.0), (20.0, 24.0)]
HATCH_BUMP_Y = [30.0, 70.0]
THUMB_R = 5.0
THUMB_Y = 88.5


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


def _window_cut() -> cq.Workplane:
    """Chamfered OLED window: small at the inner face, larger outside."""
    cx = (WIN_X_MIN + WIN_X_MAX) / 2
    cy = (WIN_Y_MIN + WIN_Y_MAX) / 2
    w = WIN_X_MAX - WIN_X_MIN
    h = WIN_Y_MAX - WIN_Y_MIN
    g = 2 * WIN_CHAMFER
    return (
        cq.Workplane("XY", origin=(cx, cy, VALLEY_FLOOR_IN - 0.2))
        .rect(w, h)
        .workplane(offset=VALLEY_FLOOR_OUT - VALLEY_FLOOR_IN + 0.4)
        .rect(w + g, h + g)
        .loft()
    )


def front_shell() -> cq.Workplane:
    outer = _rbox(
        OUT_X_MIN, OUT_X_MAX, OUT_Y_MIN, OUT_Y_MAX, Z_PART, FRONT_OUT_LOW, OUTER_R
    ).union(
        _rbox(
            OUT_X_MIN, OUT_X_MAX, FACE_STEP_Y, OUT_Y_MAX, Z_PART, FRONT_OUT_HIGH,
            OUTER_R,
        )
    )
    shell = (
        outer
        .cut(_rbox(CAV_X_MIN, CAV_X_MAX, CAV_Y_MIN, CAV_Y_MAX,
                   Z_PART - 1, FRONT_IN_LOW, CAV_R))
        .cut(_rbox(CAV_X_MIN, CAV_X_MAX, FACE_STEP_Y + 0.6, CAV_Y_MAX,
                   Z_PART - 1, FRONT_IN_HIGH, CAV_R))
    )

    # recessed screen valley: add the low floor plate between the side
    # rails (with 0.8 overlap shelves under the rails and band so the
    # union is solid), then carve the valley out of the rail-height
    # face
    shell = shell.union(
        _box(VALLEY_X[0] - 0.8, VALLEY_X[1] + 0.8,
             VALLEY_Y[0] - 0.8, VALLEY_Y[1] + 0.6,
             VALLEY_FLOOR_IN, VALLEY_FLOOR_OUT + 0.1)
    )
    shell = shell.cut(
        _box(*VALLEY_X, *VALLEY_Y, VALLEY_FLOOR_OUT, FRONT_OUT_LOW + 1)
    )

    shell = shell.cut(_window_cut())

    # SMA half hole in the top wall (upper half; lower half is a notch
    # in the back shell wall)
    shell = shell.cut(
        _ycyl(p.SMA_CENTER_X, p.SMA_CENTER_Z, SMA_HOLE_R, OUT_Y_MIN - 2, CAV_Y_MIN + 2)
    )

    # USB-C and microSD openings plus outer finger scoops
    yw0, yw1 = CAV_Y_MAX, OUT_Y_MAX
    shell = shell.cut(_box(*USB_CUT_X, yw0 - 0.5, yw1 + 1, *USB_CUT_Z))
    shell = shell.cut(_box(*USB_SCOOP_X, yw1 - SCOOP_DEPTH, yw1 + 1, *USB_SCOOP_Z))
    shell = shell.cut(_box(*SD_CUT_X, yw0 - 0.5, yw1 + 1, *SD_CUT_Z))
    shell = shell.cut(_box(*SD_SCOOP_X, yw1 - SCOOP_DEPTH, yw1 + 1, *SD_SCOOP_Z))

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
        # nub reaching in toward the switch plunger
        shell = shell.union(
            _box(BTN_NUB_TIP_X, OUT_X_MAX - BTN_TAB_T + 0.1,
                 yc - BTN_NUB_HALF, yc + BTN_NUB_HALF, *BTN_NUB_Z)
        )
        # external cap
        shell = shell.union(
            _xcyl(yc, BTN_CAP_Z, BTN_CAP_R,
                  OUT_X_MAX, OUT_X_MAX + BTN_CAP_PROTRUDE[name])
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

    # Screw bosses at the antenna end, pilot holes for M3 thread forming
    for bx, by in BOSS_XY:
        shell = shell.union(_zcyl(bx, by, BOSS_R, Z_PART, FRONT_IN_LOW))
        shell = shell.cut(_zcyl(bx, by, PILOT_R, Z_PART - 0.1, PILOT_TOP))

    # Board pedestals: full towers with locating pins at the USB end,
    # short stubs landing on the factory corner screws at the antenna
    # end
    for hx, hy in p.MOUNT_HOLES:
        if hy > 50:
            shell = shell.union(_zcyl(hx, hy, PED_UPPER_R, PED_LOWER_TOP,
                                      FRONT_IN_HIGH))
            shell = shell.union(_zcyl(hx, hy, PED_LOWER_R, 0, PED_LOWER_TOP + 0.1))
            shell = shell.union(_zcyl(hx, hy, PIN_R, -PIN_LEN, 0.1))
        else:
            shell = shell.union(_zcyl(hx, hy, STUB_R, p.SCREWHEAD_TOP + 0.1,
                                      FRONT_IN_LOW))

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
    pan = _rbox(OUT_X_MIN, OUT_X_MAX, OUT_Y_MIN, OUT_Y_MAX, PAN_OUT, Z_PART, OUTER_R)
    hump = _rbox(HUMP_X_MIN, HUMP_X_MAX, HUMP_Y_MIN, HUMP_Y_MAX,
                 HUMP_OUT, PAN_OUT + 0.5, 5.0)
    hump = hump.edges("<Z").fillet(3.0)
    shell = pan.union(hump)

    shell = shell.cut(
        _rbox(CAV_X_MIN, CAV_X_MAX, CAV_Y_MIN, CAV_Y_MAX, PAN_IN, Z_PART + 1, CAV_R)
    )
    shell = shell.cut(
        _rbox(HUMP_CAV_X_MIN, HUMP_CAV_X_MAX, HUMP_CAV_Y_MIN, HUMP_CAV_Y_MAX,
              HUMP_IN, PAN_IN + 1, CAV_R)
    )

    # Battery hatch opening: A through the floor, B recess outside
    shell = shell.cut(_box(*HATCH_A_X, *HATCH_A_Y, HUMP_OUT - 1, HUMP_IN + 0.1))
    shell = shell.cut(_box(*HATCH_B_X, *HATCH_B_Y, HUMP_OUT - 1, HATCH_B_Z_TOP))
    # snap bump dimples in the A opening side faces
    for by in HATCH_BUMP_Y:
        for ax in HATCH_A_X:
            shell = shell.cut(
                _xcyl(by, (HATCH_B_Z_TOP + HUMP_IN) / 2, 0.8, ax - 0.6, ax + 0.6)
            )
    # thumb scoop beyond the hatch bottom edge
    shell = shell.cut(_xcyl(THUMB_Y, HUMP_OUT, THUMB_R, 10.0, 23.0))

    # Screw bosses: through hole and head counterbore
    for bx, by in BOSS_XY:
        shell = shell.union(_zcyl(bx, by, BOSS_R, PAN_OUT, Z_PART))
        shell = shell.cut(_zcyl(bx, by, SHAFT_R, PAN_OUT - 0.1, Z_PART + 0.1))
        shell = shell.cut(_zcyl(bx, by, HEAD_CB_R, PAN_OUT - 0.1, HEAD_CB_TOP))

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
    bx0, bx1 = HATCH_B_X[0] + 0.15, HATCH_B_X[1] - 0.15
    by0, by1 = HATCH_B_Y[0] + 0.15, HATCH_B_Y[1] - 0.15

    outer_plate = _box(bx0, bx1, by0, by1, HUMP_OUT + 0.05, HATCH_B_Z_TOP - 0.1)
    inner_plate = _box(ax0, ax1, ay0, ay1, HATCH_B_Z_TOP - 0.15, HUMP_IN - 0.1)
    hatch = outer_plate.union(inner_plate)

    # fixed tabs hooking over the ledge at the antenna end
    for tx0, tx1 in HATCH_TAB_XS:
        root = _box(tx0, tx1, ay0, ay0 + 2.0, HUMP_IN - 0.1, HUMP_IN + 0.65)
        arm = _box(tx0, tx1, ay0 - 1.45, ay0 + 2.0, HUMP_IN + 0.15, HUMP_IN + 0.65)
        hatch = hatch.union(root).union(arm)

    # snap bumps on the inner plate side faces, protruding 0.45 into
    # the dimples cut in the opening walls
    zc = (HATCH_B_Z_TOP + HUMP_IN) / 2
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
