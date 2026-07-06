"""Dimensional parameters for the T-Beam Supreme enclosure.

Coordinate convention (matches the LilyGo board DXF):
  origin = board corner at the SMA / OLED end, on the long edge
  opposite the buttons.
  X runs across the board width (0 .. 32.893), the button edge is
  at X = PCB_W.
  Y runs along the board length (0 .. 100.127), Y = 0 is the SMA /
  OLED end (antenna end, "up" in the holding orientation), Y = PCB_L
  is the USB-C / microSD end ("down").
  Z = 0 is the PCB top surface. Top-side components (OLED, M.2 core)
  grow +Z, the 18650 holder on the back grows -Z.

Provenance tags:
  OFFICIAL   measured from LilyGo's published DXF / STEP / schematic
             (Xinyuan-LilyGO/LilyGo-LoRa-Series, dimensions/ and
             schematic/T-Beam-S3-Supreme/)
  ESTIMATED  from photos, generic part datasheets, or community
             case notes. Verify against hardware before trusting
             for a tight fit.
"""

# PCB outline (OFFICIAL, DXF dimension entities)
PCB_W = 32.893
PCB_L = 100.127
PCB_CORNER_R = 2.0  # corner cutback is 2.03 x 2.03, modelled as R2
PCB_T = 1.6  # ESTIMATED, DXF does not state thickness

# Corner mounting holes (OFFICIAL): diameter 2.0 mm.
# Community cases use 2.2 mm self tapping screws in these.
MOUNT_HOLE_D = 2.0
MOUNT_HOLES = [
    (2.59, 2.37),
    (30.30, 2.37),
    (2.61, 97.67),
    (30.29, 97.72),
]

# Side actuated tactile buttons on the X = PCB_W long edge (OFFICIAL
# positions from DXF footprints, order confirmed by silkscreen).
# Plungers protrude past the board edge in +X.
BUTTON_EDGE_X = PCB_W
BUTTON_Y = {
    "reset": 50.7,
    "power": 59.0,
    "boot": 74.6,
}
BUTTON_FOOT_W = 4.75  # footprint extent along Y (OFFICIAL)
BUTTON_FOOT_D = 2.45  # footprint extent along X (OFFICIAL)
BUTTON_PLUNGER_PROTRUSION = 1.5  # past board edge, ESTIMATED (TS-019B type)
BUTTON_PLUNGER_W = 3.0  # ESTIMATED
BUTTON_HEIGHT_ABOVE_PCB = 3.0  # switch body top, ESTIMATED
BUTTON_TRAVEL = 0.25  # typical side tactile switch travel, ESTIMATED

# USB-C on the Y = PCB_L short edge (OFFICIAL footprint x 4.30..13.74)
USB_CENTER_X = 9.02
USB_W = 9.44  # footprint width along X
USB_H = 3.3  # receptacle height, generic part, ESTIMATED
USB_PROTRUSION = 0.9  # past board edge (OFFICIAL, from footprint)

# microSD (TF) slot, card inserts from the Y = PCB_L edge (OFFICIAL bbox)
SD_X_MIN, SD_X_MAX = 14.30, 28.43
SD_Y_MIN, SD_Y_MAX = 83.53, 99.56
SD_H = 2.0  # ESTIMATED

# OLED: 1.3 inch SH1106 128x64 at the Y = 0 end. The glass sits
# between the two GPIO header rows; the factory acrylic (PMMA) cover
# floats above it (position OFFICIAL, measured from the PMMA solid in
# LilyGo's assembly STEP: 23.0 x 34.5 x 1.55 at z 5.3..6.85).
# The active area position within the glass is ESTIMATED; measure on
# hardware before tightening the case window.
OLED_Y_MIN, OLED_Y_MAX = 0.5, 37.0
OLED_X_MIN, OLED_X_MAX = 4.9, 28.0
OLED_STACK_H = 5.3  # glass top above PCB
PMMA_X_MIN, PMMA_X_MAX = 4.95, 27.95
PMMA_Y_MIN, PMMA_Y_MAX = 5.5, 40.0
PMMA_Z_MIN, PMMA_Z_MAX = 5.3, 6.9
OLED_ACTIVE_LEN = 29.42  # along Y (portrait orientation)
OLED_ACTIVE_W = 14.70  # across X

# GPIO breakout header rows flanking the OLED (positions OFFICIAL
# from DXF; socket height ESTIMATED worst case)
HEADER_ROWS_X = [3.8, 29.2]  # row centerlines
HEADER_ROW_HALF_W = 1.3
HEADER_Y_MIN, HEADER_Y_MAX = 6.77, 38.15
HEADER_H = 8.6

# Factory corner screws at the antenna end holes (hold the PMMA
# standoffs). Height above PCB ESTIMATED; the case stubs land on
# these, verify on hardware.
SCREWHEAD_R = 2.2
SCREWHEAD_TOP = 6.3

# M.2 core module region on the top side (OFFICIAL bbox from DXF).
# Socket stack height 8.5 mm is OFFICIAL ("M.2 B-KEY H8.5" on the
# schematic); the card and shields on top of it are ESTIMATED.
M2_X_MIN, M2_X_MAX = 0.0, 25.4
M2_Y_MIN, M2_Y_MAX = 43.4, 90.2
M2_STACK_H = 11.5

# SMA connector: board mounted, edge launch at the Y = 0 corner
# (confirmed by product photos and the SMA solid in LilyGo's assembly
# STEP at x 2.1..11.3, y 0..8.5 on board, z -1.5..6.5 in this frame).
# Its position is fixed by the board; the case wall hole must meet it.
# The body keepout starts at x 4.6 to leave room for the factory
# corner screw beside it.
SMA_CENTER_X = 6.7
SMA_D = 6.5
SMA_LEN = 12.0  # barrel beyond the board edge
SMA_CENTER_Z = 2.5
SMA_BODY_X = (4.6, 11.3)
SMA_BODY_Y = (0.0, 8.5)
SMA_BODY_Z = (-1.5, 6.6)

# 18650 holder on the back side (presence OFFICIAL, envelope ESTIMATED
# from generic drop-in frame holders; cell inserts radially from -Z).
BATT_W = 21.0  # across X
BATT_L = 78.0  # along Y
BATT_H = 15.0  # below the PCB underside
BATT_CENTER_X = PCB_W / 2
BATT_Y_MIN = 11.0

# Known good outer envelope: LilyGo's own injection molded shell
# measures 43.2 x 109.2 x 34.3 mm (OFFICIAL, measured from their STLs).
LILYGO_SHELL_W = 43.2
LILYGO_SHELL_L = 109.2
LILYGO_SHELL_T = 34.3

# Fit note: LilyGo changed a mold thickness by 0.8 mm in 2024 and broke
# community case fit at the SMA end. Leave extra tolerance there.
SMA_END_EXTRA_CLEARANCE = 1.0
