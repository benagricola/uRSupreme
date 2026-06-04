import os
import time
import hashlib
import shutil


def embed_spa(env):
    """
    Generate src/Web/SPAEmbedded.h from src/Web/spa/{index.html, styles.css,
    alpine.min.js} on every build. The source files are the source of
    truth; the .h file is checked in to keep PR diffs reviewable but is
    overwritten if any source asset is newer.

    A short hash of the CSS bytes is substituted for the
    `__STYLES_VERSION__` placeholder in the HTML so the CSS URL changes
    on every CSS edit. Combined with a long-lived immutable cache
    header on /styles.css, that keeps the browser-side CSS up to date
    across firmware ships without paying for a fetch on every reload.
    """
    project_dir = env.subst("$PROJECT_DIR")
    web_dir = os.path.join(project_dir, "src", "Web")
    src = os.path.join(web_dir, "spa", "index.html")
    css = os.path.join(web_dir, "spa", "styles.css")
    alpine = os.path.join(web_dir, "spa", "alpine.min.js")
    dst = os.path.join(web_dir, "SPAEmbedded.h")
    if not os.path.exists(src):
        return
    src_mtime = os.path.getmtime(src)
    if os.path.exists(css):
        src_mtime = max(src_mtime, os.path.getmtime(css))
    if os.path.exists(alpine):
        src_mtime = max(src_mtime, os.path.getmtime(alpine))
    if os.path.exists(dst) and os.path.getmtime(dst) >= src_mtime:
        return
    import gzip
    with open(src, "rb") as f:
        html = f.read()
    css_bytes = b""
    css_version = ""
    if os.path.exists(css):
        with open(css, "rb") as f:
            css_bytes = f.read()
        css_version = hashlib.sha1(css_bytes).hexdigest()[:10]
        html = html.replace(b"__STYLES_VERSION__", css_version.encode())
    gz = gzip.compress(html, compresslevel=9)
    body = ", ".join("0x{:02x}".format(b) for b in gz)
    header = (
        "// Auto-generated from src/Web/spa/{index.html, styles.css, alpine.min.js}\n"
        "// — do not edit by hand.\n"
        "#pragma once\n"
        "#include <pgmspace.h>\n"
        "namespace Web {\n"
        f"  static const size_t SPA_HTML_GZ_LEN = {len(gz)};\n"
        "  static const uint8_t SPA_HTML_GZ[] PROGMEM = {\n"
        f"    {body}\n"
        "  };\n"
    )
    if css_bytes:
        gz_css = gzip.compress(css_bytes, compresslevel=9)
        css_body = ", ".join("0x{:02x}".format(b) for b in gz_css)
        header += (
            f"  static const char SPA_STYLES_CSS_VERSION[] = \"{css_version}\";\n"
            f"  static const size_t SPA_STYLES_CSS_GZ_LEN = {len(gz_css)};\n"
            "  static const uint8_t SPA_STYLES_CSS_GZ[] PROGMEM = {\n"
            f"    {css_body}\n"
            "  };\n"
        )
        print(f"*** Embedded CSS:    {len(css_bytes)} bytes -> {len(gz_css)} bytes gzipped (v{css_version})")
    if os.path.exists(alpine):
        with open(alpine, "rb") as f:
            js = f.read()
        gz_js = gzip.compress(js, compresslevel=9)
        js_body = ", ".join("0x{:02x}".format(b) for b in gz_js)
        header += (
            f"  static const size_t SPA_ALPINE_JS_GZ_LEN = {len(gz_js)};\n"
            "  static const uint8_t SPA_ALPINE_JS_GZ[] PROGMEM = {\n"
            f"    {js_body}\n"
            "  };\n"
        )
        print(f"*** Embedded Alpine: {len(js)} bytes -> {len(gz_js)} bytes gzipped")
    header += "}\n"
    with open(dst, "w") as f:
        f.write(header)
    print(f"*** Embedded SPA:    {len(html)} bytes -> {len(gz)} bytes gzipped at {dst}")




#
# Custom targets
#

def target_package(target, source, env):
    print("*** Executing target_package steps...")
    print("Platform:", env.GetProjectOption("platform"))
    print("Board:", env.GetProjectOption("board"))
    print("Variant:", env.GetProjectOption("custom_variant"))
    if env.GetProjectOption("custom_variant").endswith('_local'):
        print("*** Skipping target_package for local build")
        return
    # do some actions
    platform = env.GetProjectOption("platform")
    board = env.GetProjectOption("board")
    firmware_package(env)

#
# Upload actions
#

def pre_upload(source, target, env):
    print("*** Executing pre_upload steps...")
    # do some actions

def post_upload(source, target, env):
    print("*** Executing post_upload steps...")
    print("Platform:", env.GetProjectOption("platform"))
    print("Board:", env.GetProjectOption("board"))
    print("Variant:", env.GetProjectOption("custom_variant"))
    print("Serial port:", env.subst("$UPLOAD_PORT"))
    # do some actions
    platform = env.GetProjectOption("platform")
    board = env.GetProjectOption("board")
    if ("espressif32" in platform):
        time.sleep(10)
        # device provisioning is incomplete and only currently appropriate for 915MHz T-Beam
        device_provision(env)
        firmware_hash(source, env)
        # firmware pacakaging is incomplete due to missing console image
        #firmware_package(env)
    elif ("nordicnrf52" in platform):
        time.sleep(5)
        # device provisioning is incomplete and only currently appropriate for 915MHz RAK4631
        device_provision(env)
        time.sleep(5)
        firmware_hash(source, env)
        # firmware pacakaging is incomplete due to missing console image
        #firmware_package(env)

def pre_clean(env):
    print("*** Executing pre_clean steps...")
    print("Platform:", env.GetProjectOption("platform"))
    print("Board:", env.GetProjectOption("board"))
    print("Variant:", env.GetProjectOption("custom_variant"))
    project_dir = env.subst("$PROJECT_DIR")
    print("project_dir:", project_dir)
    env.Execute("rm -f " + project_dir + "/Release/" + env.subst("$PROGNAME") + ".zip")
    env.Execute("rm -f " + project_dir + "/Debug/" + env.subst("$PROGNAME") + ".elf")
    env.Execute("rm -f " + project_dir + "/Debug/" + env.subst("$PROGNAME") + ".map")
    env.Execute("rm -f " + project_dir + "/Release/" + env.subst("$PROGNAME") + "_debug.zip")

def full_clean(env):
    print("*** Executing full_clean steps...")
    project_dir = env.subst("$PROJECT_DIR")
    print("project_dir:", project_dir)
    env.Execute("rm -f " + project_dir + "/Release/release.json")

def device_wipe(env):
    # Device wipe
    print("--- Wiping Device ---")
    env.Execute("rnodeconf --eeprom-wipe " + env.subst("$UPLOAD_PORT"))

def device_provision(env):
    # Device provision
    print("--- Provisioning Device ---")
    platform = env.GetProjectOption("platform")
    print("Platform:", platform)
    board = env.GetProjectOption("board")
    print("Board:", board)
    variant = env.GetProjectOption("custom_variant")
    print("Variant:", variant)
    match variant:
        case "tbeam" | "tbeam_local":
            env.Execute("rnodeconf --product e0 --model e9 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "lora32v21" | "lora32v21_local":
            env.Execute("rnodeconf --product b1 --model b9 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "heltec32v4pa" | "heltec32v4pa_local":
            env.Execute("rnodeconf --product c3 --model c8 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "rak4631" | "rak4631_local":
            env.Execute("rnodeconf --product 10 --model 12 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case "heltec_t114" | "heltec_t114_local":
            env.Execute("rnodeconf --product c2 --model c7 --hwrev 1 --rom " + env.subst("$UPLOAD_PORT"))
        case _:
            print(f"Unknown board variant {variant}, can not provision device!")

def firmware_hash(source, env):
    # Firmware hash
    print("--- Updating Firmware Hash ---")
    source_file = source[0].get_abspath()
    platform = env.GetProjectOption("platform")
    print("Platform:", platform)
    if (platform == "nordicnrf52"):
        build_dir = env.subst("$BUILD_DIR")
        env.Execute("cd " + build_dir + "; unzip -o " + source_file + " " + env.subst("$PROGNAME") + ".bin")
        #source_file.replace(".zip", ".bin")
        source_file = build_dir + "/" + env.subst("$PROGNAME") + ".bin";
        print("source_file:", source_file)
        firmware_data = open(source_file, "rb").read()
        calc_hash = hashlib.sha256(firmware_data).digest()
        hex_hash = calc_hash.hex()
        print("firmware_hash:", hex_hash)
        env.Execute("rnodeconf --firmware-hash " + hex_hash + " " + env.subst("$UPLOAD_PORT"))
    else:
        print("source_file:", source_file)
        firmware_data = open(source_file, "rb").read()
        calc_hash = hashlib.sha256(firmware_data[0:-32]).digest()
        part_hash = firmware_data[-32:]
        hex_hash = calc_hash.hex()
        print("firmware_hash:", hex_hash)
        if (calc_hash == part_hash):
            env.Execute("rnodeconf --firmware-hash " + hex_hash + " " + env.subst("$UPLOAD_PORT"))
        else:
            print("Calculated hash does not match!")

def firmware_package(env):
    # Firmware package
    print("--- Packaging Firmware ---")
    platform = env.GetProjectOption("platform")
    print("Platform:", platform)
    board = env.GetProjectOption("board")
    print("Board:", board)
    variant = env.GetProjectOption("custom_variant")
    print("Variant:", variant)
    core_dir = env.subst("$CORE_DIR")
    print("core_dir:", core_dir)
    packages_dir = env.subst("$PACKAGES_DIR")
    print("packages_dir:", packages_dir)
    workspace_dir = env.subst("$WORKSPACE_DIR")
    print("workspace_dir:", workspace_dir)
    project_dir = env.subst("$PROJECT_DIR")
    print("project_dir:", project_dir)
    #build_dir = env.subst("$BUILD_DIR").get_abspath()
    build_dir = env.subst("$BUILD_DIR")
    print("build_dir:", build_dir)
    env.Execute("mkdir -p " + project_dir + "/Release")
    env.Execute("mkdir -p " + project_dir + "/Debug")
    if (platform == "espressif32"):
        #env.Execute("cp " + packages_dir + "/framework-arduinoespressif32/tools/partitions/boot_app0.bin " + build_dir + "/rnode_firmware_" + variant + ".boot_app0")
        env.Execute("cp ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin " + build_dir + "/rnode_firmware_" + variant + ".boot_app0")
        env.Execute("cp " + build_dir + "/bootloader.bin " + build_dir + "/" + env.subst("$PROGNAME") + ".bootloader")
        env.Execute("cp " + build_dir + "/partitions.bin " + build_dir + "/" + env.subst("$PROGNAME") + ".partitions")
        env.Execute("rm -f " + project_dir + "/Release/" + env.subst("$PROGNAME") + ".zip")
        zip_cmd = "zip --junk-paths "
        zip_cmd += project_dir + "/Release/rnode_firmware_" + variant + ".zip "
        zip_cmd += project_dir + "/Release/esptool/esptool.py "
        zip_cmd += project_dir + "/Release/console_image.bin "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".bin "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".boot_app0 "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".bootloader "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".partitions "
        env.Execute(zip_cmd)
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + ".elf " + project_dir + "/Debug/.")
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + ".map " + project_dir + "/Debug/.")
        zip_cmd = "zip --junk-paths "
        zip_cmd += project_dir + "/Release/rnode_firmware_" + variant + "_debug.zip "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".elf "
        zip_cmd += build_dir + "/" + env.subst("$PROGNAME") + ".map "
        env.Execute(zip_cmd)
    elif (platform == "nordicnrf52"):
        env.Execute("cp " + build_dir + "/" + env.subst("$PROGNAME") + ".zip " + project_dir + "/Release/.")
    env.Execute("python " + project_dir + "/release_hashes.py > " + project_dir + "/Release/release.json")

#
# Main script
#

Import("env")

env.Replace(PROGNAME="rnode_firmware_%s" % env.GetProjectOption("custom_variant"))
print("PROGNAME:", env.subst("$PROGNAME"))


def compute_firmware_version(env):
    """Inject a FW_VERSION_STRING preprocessor macro from `git describe`.

    Shapes:
      v0.2.0                    — exactly on a tag
      v0.2.0-3-g5f3a92e         — 3 commits past v0.2.0
      v0.2.0-3-g5f3a92e-dirty   — uncommitted changes in the tree
      5f3a92e                   — no tags yet (only the short hash)
      5f3a92e-dirty             — no tags, dirty tree
      unknown                   — git not available / not a repo

    CI must fetch tags (fetch-depth: 0 in the checkout step) for tag
    names to be visible.
    """
    import subprocess
    project_dir = env.subst("$PROJECT_DIR")
    try:
        version = subprocess.check_output(
            ["git", "-C", project_dir, "describe",
             "--tags", "--dirty", "--always"],
            stderr=subprocess.DEVNULL,
        ).decode("ascii", errors="replace").strip()
    except Exception:
        version = "unknown"
    print("Firmware version:", version)
    # CPPDEFINES tuple form with manually-escaped embedded quotes — the
    # compiler ends up seeing `-DFW_VERSION_STRING="v0.2.0"` and the
    # source sees a literal const-char* string.
    env.Append(CPPDEFINES=[("FW_VERSION_STRING", '\\"%s\\"' % version)])


def regenerate_sdkconfig_defaults(env):
    """For arduino-as-component builds (framework = arduino, espidf):
    regenerate sdkconfig.defaults = arduino-esp32's reference sdkconfig +
    this project's overrides (sdkconfig.overrides). pioarduino's espidf
    builder only honours `custom_sdkconfig` for pure `framework = arduino`
    builds, so for arduino-as-component we do the merge ourselves. Without
    the arduino reference as the base, defaults like FREERTOS_HZ=1000,
    AUTOSTART_ARDUINO, BT_*, flash/partition settings fall back to IDF
    natives that don't match arduino-esp32 v3.x. Append wins (later
    CONFIG_X overrides earlier). No-op for non-arduino-as-component envs.
    """
    frameworks = env.subst("$PIOFRAMEWORK").split()
    if "arduino" not in frameworks or "espidf" not in frameworks:
        return
    project_dir = env.subst("$PROJECT_DIR")
    packages_dir = env.subst("$PROJECT_PACKAGES_DIR")
    if not packages_dir or not os.path.isdir(packages_dir):
        packages_dir = os.path.expanduser("~/.platformio/packages")
    mcu = env.subst("$BOARD_MCU")  # e.g. "esp32s3"
    arduino_ref = os.path.join(
        packages_dir, "framework-arduinoespressif32-libs", mcu, "sdkconfig")
    overrides = os.path.join(project_dir, "sdkconfig.overrides")
    target = os.path.join(project_dir, "sdkconfig.defaults")
    if not os.path.exists(arduino_ref):
        print(f"*** sdkconfig.defaults: arduino reference not found at "
              f"{arduino_ref}; leaving sdkconfig.defaults untouched")
        return
    with open(arduino_ref, "rb") as f:
        ref_bytes = f.read()
    override_bytes = b""
    if os.path.exists(overrides):
        with open(overrides, "rb") as f:
            override_bytes = f.read()
    new = (
        b"# AUTO-GENERATED by extra_script.py:regenerate_sdkconfig_defaults.\n"
        b"# Source: " + arduino_ref.encode() + b"\n"
        b"# Overrides: sdkconfig.overrides\n"
        b"# DO NOT EDIT THIS FILE BY HAND. Edit sdkconfig.overrides instead.\n"
        b"# ---------------------------------------------------------------\n"
        + ref_bytes
        + b"\n# --- Project overrides (sdkconfig.overrides) -----------------\n"
        + override_bytes
    )
    if os.path.exists(target):
        with open(target, "rb") as f:
            if f.read() == new:
                return
    with open(target, "wb") as f:
        f.write(new)
    # IDF generates the effective sdkconfig (sdkconfig.<env>) from these
    # defaults ONCE, then preserves it across builds — changing a default for
    # an already-set symbol does NOT flip it. So whenever the defaults change
    # (an edit to sdkconfig.overrides or a new arduino reference), drop the
    # stale effective sdkconfig to force a clean regeneration; otherwise the
    # override edit silently fails to take effect.
    effective = os.path.join(project_dir, "sdkconfig.%s" % env.subst("$PIOENV"))
    if os.path.exists(effective):
        os.remove(effective)
        print(f"*** Removed stale {os.path.basename(effective)} "
              f"so the new sdkconfig.overrides re-apply")
    print(f"*** Regenerated sdkconfig.defaults "
          f"({len(ref_bytes)} B arduino ref + {len(override_bytes)} B overrides)")


compute_firmware_version(env)

print("*** Running custom script...")
# microReticulum patches used to be applied here. They're now committed
# directly to the local microReticulum repo at ../microReticulum, on the
# ur-patches branch (one commit per fix). platformio.ini's lib_deps
# uses symlink:// to that repo so the build picks them up natively.
embed_spa(env)
# arduino-as-component (supreme) envs: rebuild sdkconfig.defaults from the
# arduino reference + sdkconfig.overrides before the espidf builder reads it.
# No-op on the official-platform envs.
regenerate_sdkconfig_defaults(env)
platform = env.GetProjectOption("platform")
print("Platform:", platform)
targets = env.GetProjectOption("targets", [])
print("Targets:", targets)

# Clean
if env.IsCleanTarget():
    pre_clean(env)
    if "cleanall" in targets or "fullclean" in targets:
        full_clean(env)

# Add custom targets
if (platform == "espressif32"):
    env.AddCustomTarget(
        name="package",
        dependencies="$BUILD_DIR/${PROGNAME}.bin",
        actions=[
            target_package
        ],
        title="Package",
        description="Package esp32 firmware for delivery"
    )
elif (platform == "nordicnrf52"):
    # remove --specs=nano.specs to allow exceptions to work
    if '--specs=nano.specs' in env['LINKFLAGS']:
        env['LINKFLAGS'].remove('--specs=nano.specs')
    env.AddCustomTarget(
        name="package",
        dependencies="$BUILD_DIR/${PROGNAME}.zip",
        actions=[
            target_package
        ],
        title="Package",
        description="Package nrf52 firmware for delivery"
    )

# Register actions
env.AddPreAction("upload", pre_upload)
env.AddPostAction("upload", post_upload)
