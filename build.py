import os
import subprocess
from pathlib import Path
import sys

Import("env")

def merge_binaries(source, target, env):
    # Get project directory and name
    project_dir = Path(env["PROJECT_DIR"])
    pioenv = env.get("PIOENV", "default")
    merged_bin = project_dir / f"{pioenv}_build.bin"

    # Paths to source files
    build_dir = Path(env.subst("$BUILD_DIR"))
    firmware_bin = build_dir / "firmware.bin"
    bootloader_bin = build_dir / "bootloader.bin"
    partitions_bin = build_dir / "partitions.bin"

    # Path to esptool.exe in project root
    esptool_exe = project_dir / "esptool.exe"

    # Check if esptool.exe exists
    if not esptool_exe.exists():
        print(f"[ERROR] CRITICAL ERROR: esptool.exe not found in project root")
        print(f"   Place esptool.exe here: {project_dir}")
        sys.exit(1)

    # Check if source files exist
    missing_files = []
    if not firmware_bin.exists(): missing_files.append(str(firmware_bin))
    if not bootloader_bin.exists(): missing_files.append(str(bootloader_bin))
    if not partitions_bin.exists(): missing_files.append(str(partitions_bin))

    if missing_files:
        print(f"[ERROR] Error: Missing files for merging:")
        for f in missing_files: print(f" - {f}")
        print("Ensure the build completed successfully")
        sys.exit(1)

    # Detect chip and bootloader offset from BoardConfig
    board = env.BoardConfig()
    mcu = board.get("build.mcu", "esp32")

    if mcu == "esp32c3":
        chip = "esp32c3"
        bootloader_offset = "0x0"
    else:
        chip = "esp32"
        bootloader_offset = "0x1000"

    # Command for esptool.exe
    cmd = [
        str(esptool_exe),
        "--chip", chip,
        "merge_bin",
        "-o", str(merged_bin),
        bootloader_offset, str(bootloader_bin),
        "0x8000", str(partitions_bin),
        "0x10000", str(firmware_bin)
    ]

    print(f"[BUILD] Merging binary files for {chip} ({pioenv}):")
    print(f" - Bootloader:  {bootloader_bin} @ {bootloader_offset}")
    print(f" - Partitions:  {partitions_bin} @ 0x8000")
    print(f" - Firmware:    {firmware_bin} @ 0x10000")
    print(f"[INFO] Using: {esptool_exe}")

    try:
        # Execute command
        result = subprocess.run(
            cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            shell=True
        )

        # Check output file size
        if merged_bin.exists():
            size = os.path.getsize(merged_bin) / 1024
            print(f"\n[OK] Successfully created merged file: {merged_bin}")
            print(f"Size: {size:.2f} KB")
            print(f"[FLASH] To flash, use: esptool.exe --chip {chip} write_flash 0x0 \"{merged_bin}\"")
        else:
            print("[ERROR] Error: Merged file was not created")
            if result.stderr:
                print("[INFO] esptool error:")
                print(result.stderr)
            sys.exit(1)

    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Error running esptool.exe (code {e.returncode}):")
        print(e.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"[ERROR] Unknown error: {str(e)}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

# Add script as post-action after build
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_binaries)