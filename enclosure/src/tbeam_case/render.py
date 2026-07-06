"""Offscreen PNG renders of the case around the board.

Renders tessellated STLs with VTK (needs libosmesa6 for headless GL).
Case parts are drawn translucent so the board placement inside is
visible for design review.
"""

from pathlib import Path

# (name, color, opacity) for each board reference part
BOARD_STYLE = {
    "pcb": ((0.05, 0.45, 0.22), 1.0),
    "oled": ((0.08, 0.08, 0.10), 1.0),
    "pmma": ((0.85, 0.92, 0.98), 0.5),
    "headers": ((0.12, 0.12, 0.12), 1.0),
    "m2_core": ((0.45, 0.47, 0.52), 1.0),
    "sd_slot": ((0.72, 0.72, 0.72), 1.0),
    "usb_c": ((0.75, 0.75, 0.78), 1.0),
    "buttons": ((0.85, 0.15, 0.15), 1.0),
    "sma": ((0.85, 0.70, 0.20), 1.0),
    "battery_holder": ((0.20, 0.25, 0.65), 1.0),
}

CASE_STYLE = {
    "front_shell": ((0.62, 0.70, 0.78), 0.45),
    "back_shell": ((0.55, 0.62, 0.55), 0.5),
    "battery_hatch": ((0.90, 0.55, 0.15), 0.75),
}

# Scene center, roughly the middle of the assembled case
CENTER = (17.0, 47.0, -3.0)

# name -> (position offset from CENTER, view up, parallel scale or None)
VIEWS = {
    "iso_front": ((130, -110, 190), (0, -1, 0), None),
    "iso_back": ((120, -100, -200), (0, -1, 0), None),
    "side_buttons": ((250, -15, 30), (0, -1, 0), None),
    "bottom_ports": ((0, 245, 25), (0, 0, 1), None),
    "front_flat": ((0, 0, 270), (0, -1, 0), 62.0),
    "back_flat": ((0, 0, -270), (0, -1, 0), 62.0),
}

# Exploded view: per part Z offset
EXPLODE = {"front_shell": 42.0, "battery_hatch": -34.0}

# LilyGo's simplified board STEP uses a centered frame: SMA end at
# y = -50, buttons on +X, PCB top at z = +1. This offset maps it into
# the board frame from params.py for overlay verification.
STEP_OFFSET = (16.45, 50.0, -1.0)
STEP_STL = "lilygo_step_overlay.stl"


def render_all(stl_dir: Path, out_dir: Path, exploded: bool = True) -> list[Path]:
    import vtk

    out_dir.mkdir(parents=True, exist_ok=True)
    written = []

    def make_actor(path: Path, color, opacity, offset=(0.0, 0.0, 0.0)):
        reader = vtk.vtkSTLReader()
        reader.SetFileName(str(path))
        mapper = vtk.vtkPolyDataMapper()
        mapper.SetInputConnection(reader.GetOutputPort())
        actor = vtk.vtkActor()
        actor.SetMapper(mapper)
        actor.SetPosition(*offset)
        prop = actor.GetProperty()
        prop.SetColor(*color)
        prop.SetOpacity(opacity)
        prop.SetSpecular(0.25)
        prop.SetSpecularPower(20)
        return actor

    def render_scene(name: str, view, zoffsets: dict):
        ren = vtk.vtkRenderer()
        ren.SetBackground(0.96, 0.96, 0.97)
        # depth peeling for correct translucency
        ren.SetUseDepthPeeling(1)
        ren.SetMaximumNumberOfPeels(12)
        ren.SetOcclusionRatio(0.02)

        board_alpha = 0.35 if name == "board_vs_step" else 1.0
        for part, (color, opacity) in BOARD_STYLE.items():
            stl = stl_dir / f"board_{part}.stl"
            if stl.exists():
                ren.AddActor(make_actor(stl, color, opacity * board_alpha))
        if name == "board_vs_step":
            step_stl = stl_dir / STEP_STL
            if step_stl.exists():
                ren.AddActor(
                    make_actor(step_stl, (0.35, 0.5, 0.35), 1.0, STEP_OFFSET)
                )
        else:
            for part, (color, opacity) in CASE_STYLE.items():
                stl = stl_dir / f"{part}.stl"
                if stl.exists():
                    ren.AddActor(
                        make_actor(
                            stl, color, opacity,
                            (0.0, 0.0, zoffsets.get(part, 0.0)),
                        )
                    )

        rw = vtk.vtkRenderWindow()
        rw.SetOffScreenRendering(1)
        rw.SetAlphaBitPlanes(1)
        rw.SetMultiSamples(0)
        rw.SetSize(1100, 1500)
        rw.AddRenderer(ren)

        offset, up, pscale = view
        cam = ren.GetActiveCamera()
        cam.SetFocalPoint(*CENTER)
        cam.SetPosition(
            CENTER[0] + offset[0], CENTER[1] + offset[1], CENTER[2] + offset[2]
        )
        cam.SetViewUp(*up)
        if pscale:
            cam.ParallelProjectionOn()
            cam.SetParallelScale(pscale)
        ren.ResetCameraClippingRange()

        rw.Render()
        w2i = vtk.vtkWindowToImageFilter()
        w2i.SetInput(rw)
        w2i.Update()
        writer = vtk.vtkPNGWriter()
        png = out_dir / f"{name}.png"
        writer.SetFileName(str(png))
        writer.SetInputConnection(w2i.GetOutputPort())
        writer.Write()
        written.append(png)

    for name, view in VIEWS.items():
        render_scene(name, view, {})
    if exploded:
        render_scene("exploded", ((150, -125, 210), (0, -1, 0), None), EXPLODE)
    if (stl_dir / STEP_STL).exists():
        render_scene("board_vs_step", ((140, -120, 200), (0, -1, 0), None), {})
        render_scene(
            "board_vs_step_side", ((260, 0, 0), (0, -1, 0), 58.0), {}
        )
    return written
