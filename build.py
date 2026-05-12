import os
import subprocess
import sys
from pathlib import Path

# --- CONFIGURATION ---
VCPKG_PATH = Path(r"C:\_vcpkg\vcpkg\scripts\buildsystems\vcpkg.cmake")
BUILD_DIR = Path("build")
TRIPLET = "x64-windows"
CONFIG = "Release"

def run_command(command, description):
    """Helper to run shell commands and handle errors."""
    print(f"--- {description} ---")
    try:
        # shell=True is used to mimic Batch behavior, 
        # check=True raises an exception on non-zero exit codes
        subprocess.run(command, check=True, shell=True)
    except subprocess.CalledProcessError as e:
        print(f"\nERROR: {description} failed!")
        sys.exit(e.returncode)

def main():
    print("Starting build for Project: Dupic...")

    # Create build directory if it doesn't exist
    if not BUILD_DIR.exists():
        BUILD_DIR.mkdir(parents=True)
        print(f"Created directory: {BUILD_DIR}")

    # Configure the project
    configure_cmd = [
        "cmake",
        "-S", ".",
        "-B", str(BUILD_DIR),
        f'-DCMAKE_TOOLCHAIN_FILE="{VCPKG_PATH}"',
        f"-DVCPKG_TARGET_TRIPLET={TRIPLET}"
    ]
    
    run_command(" ".join(configure_cmd), "Configuring CMake")

    # Build the project
    build_cmd = [
        "cmake",
        "--build", str(BUILD_DIR),
        "--config", CONFIG
    ]
    
    run_command(" ".join(build_cmd), "Building Project")

    print("\n" + "="*20)
    print("Build Successful!")
    print(f"Your executable and libvips DLLs are in: {BUILD_DIR / CONFIG}")
    print("="*20)

if __name__ == "__main__":
    main()