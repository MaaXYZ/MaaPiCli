"""Check the shipped runtime and launch the CLI from an isolated package copy."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


RUNTIME_LOAD_SCRIPT = """
import ctypes
import os
from pathlib import Path
import sys

directory = Path(sys.argv[1])
dll_directory = os.add_dll_directory(str(directory)) if sys.platform == "win32" else None
print("Loading packaged WebGPU Dawn", flush=True)
dawn = ctypes.CDLL(sys.argv[3])
print("Loading packaged ONNX Runtime", flush=True)
onnx = ctypes.CDLL(sys.argv[2])
onnx.OrtGetApiBase.restype = ctypes.c_void_p
if not onnx.OrtGetApiBase():
    raise RuntimeError("ONNX Runtime returned a null API base")
print("Packaged ONNX Runtime and WebGPU Dawn loaded successfully")
"""


def runtime_libraries(binary_dir):
    patterns = {
        "win32": (
            r"onnxruntime(?:_maa)?\.dll",
            r"(?:lib)?webgpu_dawn(?:_maa)?\.dll",
        ),
        "darwin": (
            r"libonnxruntime(?:\.[0-9]+)*\.dylib",
            r"libwebgpu_dawn(?:\.[0-9]+)*\.dylib",
        ),
    }.get(sys.platform, (
        r"libonnxruntime\.so(?:\.[0-9]+)*",
        r"libwebgpu_dawn\.so(?:\.[0-9]+)*",
    ))
    libraries = []
    for library, pattern in zip(("onnxruntime", "webgpu_dawn"), patterns):
        matches = sorted(
            path for path in binary_dir.iterdir()
            if path.is_file() and re.fullmatch(pattern, path.name)
        )
        if not matches:
            raise RuntimeError(f"Package is missing the {library} runtime")
        libraries.append(matches[0])
    return libraries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--arch", choices=("x86_64", "aarch64"))
    args = parser.parse_args()
    package = args.package.resolve()
    runtime_libraries(package / "bin")
    if args.check_only:
        return

    fixture = Path(__file__).resolve().parents[1] / "cli/fixtures/eof/interface.json"
    with tempfile.TemporaryDirectory(prefix="maapicli-package-") as directory:
        binary_dir = Path(directory) / "bin"
        shutil.copytree(package / "bin", binary_dir, symlinks=True)
        shutil.copyfile(fixture, binary_dir / "interface.json")

        # Do not let build/development prefixes hide missing packaged libraries.
        environment = os.environ.copy()
        for variable in (
            "LD_LIBRARY_PATH",
            "LD_PRELOAD",
            "DYLD_LIBRARY_PATH",
            "DYLD_FALLBACK_LIBRARY_PATH",
            "DYLD_INSERT_LIBRARIES",
        ):
            environment.pop(variable, None)
        if sys.platform == "win32":
            windows = Path(os.environ["SystemRoot"])
            environment["PATH"] = os.pathsep.join(
                str(path) for path in (binary_dir, windows / "System32", windows)
            )

        libraries = runtime_libraries(binary_dir)
        interpreter = [sys.executable]
        if sys.platform == "darwin" and args.arch:
            # The macOS toolcache Python is universal2. Launch its target slice
            # directly rather than going through the system Xcode launcher.
            architecture = "arm64" if args.arch == "aarch64" else "x86_64"
            subprocess.run(
                ["/usr/bin/lipo", sys.executable, "-verify_arch", architecture],
                check=True,
                timeout=30,
            )
            interpreter = ["/usr/bin/arch", f"-{architecture}", sys.executable]
        subprocess.run(
            [*interpreter, "-c", RUNTIME_LOAD_SCRIPT, str(binary_dir),
             *(str(path) for path in libraries)],
            cwd=directory,
            env=environment,
            check=True,
            timeout=90,
        )

        executable = "MaaPiCli.exe" if sys.platform == "win32" else "MaaPiCli"
        result = subprocess.run(
            [str(binary_dir / executable)],
            cwd=directory,
            env=environment,
            input="",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
        print(result.stdout)
        if result.returncode != 0 or "### Select controller ###" not in result.stdout:
            raise RuntimeError(f"Packaged CLI startup failed: exit {result.returncode}")


if __name__ == "__main__":
    main()
