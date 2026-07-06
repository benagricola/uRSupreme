"""Reference model of the assembled T-Beam Supreme board.

Not a printable part. This is the keep-out solid the enclosure is
designed around: PCB, OLED stack, M.2 core stack, button plungers,
USB-C, microSD, SMA barrel and the 18650 holder, each as a simple
prism at its measured position. See params.py for provenance of
every dimension.
"""

import cadquery as cq

from . import params as p


def pcb() -> cq.Workplane:
    """Bare PCB plate with corner radii and mounting holes."""
    board = (
        cq.Workplane("XY")
        .box(p.PCB_W, p.PCB_L, p.PCB_T, centered=False)
        .translate((0, 0, -p.PCB_T))
        .edges("|Z")
        .fillet(p.PCB_CORNER_R)
    )
    for x, y in p.MOUNT_HOLES:
        hole = (
            cq.Workplane("XY")
            .center(x, y)
            .circle(p.MOUNT_HOLE_D / 2)
            .extrude(-p.PCB_T)
        )
        board = board.cut(hole)
    return board


def _block(x0, x1, y0, y1, z0, z1) -> cq.Workplane:
    return (
        cq.Workplane("XY")
        .box(x1 - x0, y1 - y0, z1 - z0, centered=False)
        .translate((x0, y0, z0))
    )


def oled() -> cq.Workplane:
    return _block(
        p.OLED_X_MIN, p.OLED_X_MAX, p.OLED_Y_MIN, p.OLED_Y_MAX, 0, p.OLED_STACK_H
    )


def pmma() -> cq.Workplane:
    """Factory acrylic cover, plus the corner screw stacks at the
    antenna end holes."""
    plate = _block(
        p.PMMA_X_MIN, p.PMMA_X_MAX, p.PMMA_Y_MIN, p.PMMA_Y_MAX,
        p.PMMA_Z_MIN, p.PMMA_Z_MAX,
    )
    for hx, hy in p.MOUNT_HOLES:
        if hy < 50:
            plate = plate.union(
                cq.Workplane("XY")
                .center(hx, hy)
                .circle(p.SCREWHEAD_R)
                .extrude(p.SCREWHEAD_TOP)
            )
    return plate


def headers() -> cq.Workplane:
    rows = None
    for rx in p.HEADER_ROWS_X:
        row = _block(
            rx - p.HEADER_ROW_HALF_W, rx + p.HEADER_ROW_HALF_W,
            p.HEADER_Y_MIN, p.HEADER_Y_MAX, 0, p.HEADER_H,
        )
        rows = row if rows is None else rows.union(row)
    return rows


def m2_core() -> cq.Workplane:
    return _block(
        p.M2_X_MIN, p.M2_X_MAX, p.M2_Y_MIN, p.M2_Y_MAX, 0, p.M2_STACK_H
    )


def sd_slot() -> cq.Workplane:
    return _block(p.SD_X_MIN, p.SD_X_MAX, p.SD_Y_MIN, p.SD_Y_MAX, 0, p.SD_H)


def usb_c() -> cq.Workplane:
    x0 = p.USB_CENTER_X - p.USB_W / 2
    return _block(
        x0, x0 + p.USB_W, p.PCB_L - 7.0, p.PCB_L + p.USB_PROTRUSION, 0, p.USB_H
    )


def buttons() -> cq.Workplane:
    result = None
    for y in p.BUTTON_Y.values():
        body = _block(
            p.BUTTON_EDGE_X - p.BUTTON_FOOT_D,
            p.BUTTON_EDGE_X + p.BUTTON_PLUNGER_PROTRUSION,
            y - p.BUTTON_FOOT_W / 2,
            y + p.BUTTON_FOOT_W / 2,
            0,
            p.BUTTON_HEIGHT_ABOVE_PCB,
        )
        result = body if result is None else result.union(body)
    return result


def sma() -> cq.Workplane:
    # Board mounted edge launch connector: body on the PCB corner,
    # barrel protruding past the board edge through the case wall.
    barrel = (
        cq.Workplane("XZ")
        .center(p.SMA_CENTER_X, p.SMA_CENTER_Z)
        .circle(p.SMA_D / 2)
        .extrude(p.SMA_LEN)
    )
    body = _block(*p.SMA_BODY_X, *p.SMA_BODY_Y, *p.SMA_BODY_Z)
    return barrel.union(body)


def battery_holder() -> cq.Workplane:
    x0 = p.BATT_CENTER_X - p.BATT_W / 2
    return _block(
        x0,
        x0 + p.BATT_W,
        p.BATT_Y_MIN,
        p.BATT_Y_MIN + p.BATT_L,
        -p.PCB_T - p.BATT_H,
        -p.PCB_T,
    )


def assembly() -> cq.Assembly:
    asm = cq.Assembly(name="tbeam_supreme_reference")
    asm.add(pcb(), name="pcb", color=cq.Color(0.0, 0.35, 0.15))
    asm.add(oled(), name="oled", color=cq.Color(0.1, 0.1, 0.1))
    asm.add(pmma(), name="pmma", color=cq.Color(0.85, 0.9, 0.95))
    asm.add(headers(), name="headers", color=cq.Color(0.15, 0.15, 0.15))
    asm.add(m2_core(), name="m2_core", color=cq.Color(0.3, 0.3, 0.35))
    asm.add(sd_slot(), name="sd_slot", color=cq.Color(0.6, 0.6, 0.6))
    asm.add(usb_c(), name="usb_c", color=cq.Color(0.7, 0.7, 0.7))
    asm.add(buttons(), name="buttons", color=cq.Color(0.8, 0.2, 0.2))
    asm.add(sma(), name="sma", color=cq.Color(0.8, 0.7, 0.2))
    asm.add(battery_holder(), name="battery_holder", color=cq.Color(0.2, 0.2, 0.6))
    return asm


def keepout() -> cq.Workplane:
    """Single fused solid of everything, for boolean clearance checks."""
    solid = pcb()
    for part in (oled(), pmma(), headers(), m2_core(), sd_slot(), usb_c(),
                 buttons(), sma(), battery_holder()):
        solid = solid.union(part)
    return solid
