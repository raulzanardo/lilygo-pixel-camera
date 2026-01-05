Import("env")
import shutil
import os

def copy_firmware_to_build(source, target, env):
    build_dir = os.path.join(env.get("PROJECT_DIR"), "build")
    
    if not os.path.exists(build_dir):
        os.makedirs(build_dir)
    
    firmware_source = str(target[0])
    firmware_dest = os.path.join(build_dir, "firmware_latest.bin")
    
    try:
        shutil.copy2(firmware_source, firmware_dest)
        print(f"Firmware copied to: {firmware_dest}")
    except Exception as e:
        print(f"Error copying firmware: {e}")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware_to_build)
