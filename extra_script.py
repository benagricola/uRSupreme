import os
import time
import hashlib
import shutil


def embed_spa(env):
    """
    Generate Web/SPAEmbedded.h from Web/spa/index.html on every build.
    The HTML is the source of truth; the .h file is checked in to keep
    PR diffs reviewable but is overwritten if the .html is newer.
    """
    project_dir = env.subst("$PROJECT_DIR")
    src = os.path.join(project_dir, "Web", "spa", "index.html")
    dst = os.path.join(project_dir, "Web", "SPAEmbedded.h")
    if not os.path.exists(src):
        return
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    with open(src, "rb") as f:
        html = f.read()
    # gzip for serving via Content-Encoding: gzip (saves ~60% on the wire)
    import gzip
    gz = gzip.compress(html, compresslevel=9)
    body = ", ".join("0x{:02x}".format(b) for b in gz)
    header = (
        "// Auto-generated from Web/spa/index.html — do not edit by hand.\n"
        "#pragma once\n"
        "#include <pgmspace.h>\n"
        "namespace Web {\n"
        f"  static const size_t SPA_HTML_GZ_LEN = {len(gz)};\n"
        "  static const uint8_t SPA_HTML_GZ[] PROGMEM = {\n"
        f"    {body}\n"
        "  };\n"
        "}\n"
    )
    with open(dst, "w") as f:
        f.write(header)
    print(f"*** Embedded SPA: {len(html)} bytes -> {len(gz)} bytes gzipped at {dst}")


def patch_microreticulum_validate(env):
    """
    Patch microReticulum's Identity::validate to actually return the result of
    Ed25519PublicKey::verify().

    Upstream bug: Identity.cpp's validate() discards the bool returned by
    _sig_pub->verify() and unconditionally returns true (only catches
    exceptions, which the verify path doesn't throw). This silently turns
    signature verification into a no-op everywhere — including the library's
    own Identity::validate_announce path.

    Demonstrated by LXMF/Probe.h: alice.validate(bob_signature, msg) returns
    true with the bug present. With the fix the same probe returns false as
    expected and the probe reports PASS.

    Upstream report: TODO file an issue on attermann/microReticulum referencing
    this file and Identity.cpp:652.
    """
    project_dir = env.subst("$PROJECT_DIR")
    libdeps = os.path.join(project_dir, ".pio", "libdeps", env.subst("$PIOENV"),
                           "microReticulum", "src", "Identity.cpp")
    if not os.path.exists(libdeps):
        return
    with open(libdeps, "r") as f:
        src = f.read()
    bad = "\t\t\t_object->_sig_pub->verify(signature, message);\n\t\t\treturn true;\n"
    good = "\t\t\treturn _object->_sig_pub->verify(signature, message);\n"
    if bad in src:
        print("*** Patching microReticulum Identity::validate (upstream bug fix) at", libdeps)
        src = src.replace(bad, good)
        with open(libdeps, "w") as f:
            f.write(src)
    elif good in src:
        # already patched
        pass
    else:
        print("*** WARNING: Identity.cpp does not match either patched or known-bad shape — patch skipped")

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

print("*** Running custom script...")
patch_microreticulum_validate(env)
embed_spa(env)
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
