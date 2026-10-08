#!/usr/bin/env python3
"""Run MXWrite scripts with the native module and its Windows DLL dependencies."""

import argparse
import importlib.machinery
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

REPO_DIR = Path(__file__).resolve().parent
EXAMPLES_DIR = REPO_DIR / "examples"
EXAMPLES = {
    "pattern": "python_example.py",
    "opencv": "mxwrite-opencv.py",
}


def unique_paths(paths):
    result = []
    for path in paths:
        path = Path(path).expanduser().resolve()
        if path not in result:
            result.append(path)
    return result


def read_cache(directory):
    cache = directory / "CMakeCache.txt"
    if not cache.is_file():
        cache = directory.parent / "CMakeCache.txt"
    values = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line and ":" in line and not line.startswith(("#", "//")):
                key, value = line.split("=", 1)
                values[key.split(":", 1)[0]] = value
    return values


def module_directories(explicit=None):
    if explicit or os.environ.get("MXWRITE_PYTHON_MODULE_DIR"):
        return unique_paths([explicit or os.environ["MXWRITE_PYTHON_MODULE_DIR"]])
    roots = [REPO_DIR / "python_mod", REPO_DIR / "build-python"]
    roots += sorted(path for path in REPO_DIR.glob("build*") if path.is_dir())
    paths = []
    for root in unique_paths(roots):
        paths.extend([root, root / "Release", root / "RelWithDebInfo", root / "Debug"])
    return unique_paths(paths)


def has_module(directory, suffixes=None):
    if suffixes is not None:
        return any((directory / ("mxwrite_ext" + suffix)).is_file() for suffix in suffixes)
    return any(directory.glob("mxwrite_ext*.pyd")) or any(directory.glob("mxwrite_ext*.so"))


def vcpkg_bins(directories):
    bins = []
    roots = []
    if os.environ.get("VCPKG_ROOT"):
        roots.append(Path(os.environ["VCPKG_ROOT"]) / "installed")
    for directory in directories:
        cache = read_cache(directory)
        installed = cache.get("VCPKG_INSTALLED_DIR") or cache.get("_VCPKG_INSTALLED_DIR")
        if installed:
            bins.append(Path(installed) / cache.get("VCPKG_TARGET_TRIPLET", "x64-windows") / "bin")
    for parent in Path(sys.executable).resolve().parents:
        if parent.parent.name == "installed":
            bins.append(parent / "bin")
    roots.extend([REPO_DIR / "vcpkg_installed", REPO_DIR / "vcpkg" / "installed", Path("C:/vcpkg/installed")])
    triplet = os.environ.get("VCPKG_DEFAULT_TRIPLET", "x64-windows")
    bins += [root / triplet / "bin" for root in roots]
    return [path for path in unique_paths(bins) if path.is_dir()]


def interpreter_candidates(directories):
    paths = []
    for directory in directories:
        cache = read_cache(directory)
        for key in ("Python_EXECUTABLE", "_Python_EXECUTABLE"):
            if cache.get(key):
                paths.append(cache[key])
    for bin_dir in vcpkg_bins(directories):
        paths.append(bin_dir.parent / "tools" / "python3" / "python.exe")
    launcher = shutil.which("py")
    if launcher:
        result = subprocess.run([launcher, "-0p"], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            for index, character in enumerate(line):
                if character == ":" and index > 0:
                    paths.append(line[index - 1:].strip())
                    break
    return [path for path in unique_paths(paths) if path.is_file()]


def select_module(directories):
    for directory in directories:
        if has_module(directory, importlib.machinery.EXTENSION_SUFFIXES):
            return directory, None
    if os.name == "nt" and any(has_module(path) for path in directories) and not os.environ.get("MXWRITE_LAUNCHER_REEXEC"):
        for interpreter in interpreter_candidates(directories):
            if interpreter == Path(sys.executable).resolve():
                continue
            try:
                result = subprocess.run(
                    [str(interpreter), "-c", "import importlib.machinery,json; print(json.dumps(importlib.machinery.EXTENSION_SUFFIXES))"],
                    capture_output=True, text=True, timeout=10,
                )
                suffixes = json.loads(result.stdout) if result.returncode == 0 else []
            except (OSError, ValueError, subprocess.TimeoutExpired):
                continue
            for directory in directories:
                if has_module(directory, suffixes):
                    return directory, interpreter
    return None, None


def configure_runtime(module_dir, extra_dll_dirs):
    paths = [EXAMPLES_DIR]
    if module_dir:
        paths.insert(0, module_dir)
    for path in reversed(unique_paths(paths)):
        if path.is_dir():
            sys.path.insert(0, str(path))
    handles = []
    if os.name == "nt":
        dll_dirs = paths + vcpkg_bins([module_dir] if module_dir else []) + extra_dll_dirs
        if os.environ.get("MXWRITE_DLL_DIRS"):
            dll_dirs += os.environ["MXWRITE_DLL_DIRS"].split(os.pathsep)
        for key in ("CUDA_PATH",):
            if os.environ.get(key):
                root = Path(os.environ[key])
                dll_dirs += [root / "bin", root / "bin" / "x64"]
        # Python 3.8+ needs registration even when the DLL directory is on PATH.
        dll_dirs += [path for path in os.environ.get("PATH", "").split(os.pathsep) if path]
        for directory in unique_paths(dll_dirs):
            if directory.is_dir():
                handles.append(os.add_dll_directory(str(directory)))
    return handles


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(
        description="Run an MXWrite example by name, a Python script, -m module, or -c code.",
        epilog='Examples: mxpy.cmd pattern --frames 90 | mxpy.cmd --check | mxpy.cmd -c "import mxwrite_ext"',
    )
    parser.add_argument("--list", action="store_true", help="list bundled examples")
    parser.add_argument("--check", action="store_true", help="check imports and enumerate video encoders")
    parser.add_argument("--module-dir", help="directory containing mxwrite_ext (also MXWRITE_PYTHON_MODULE_DIR)")
    parser.add_argument("--dll-dir", action="append", default=[], help="extra Windows DLL directory; repeat as needed")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("-m", dest="module", nargs=argparse.REMAINDER, help="run a Python module with arguments")
    modes.add_argument("-c", dest="code", nargs=argparse.REMAINDER, help="run Python code with arguments")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="example name or script path, followed by arguments")
    args = parser.parse_args(argv)
    if args.module == [] or args.code == []:
        parser.error("-m and -c require a module name or code string")
    if args.list:
        for name, filename in EXAMPLES.items():
            print(f"{name:22} {filename}")
        return 0
    if not (args.check or args.command or args.module or args.code):
        parser.print_help()
        return 0
    script = None
    if args.command:
        target = args.command[0]
        script = EXAMPLES_DIR / EXAMPLES[target] if target in EXAMPLES else Path(target).expanduser().resolve()
        if not script.is_file():
            print(f"Script not found: {script}\nUse --list to see bundled examples.", file=sys.stderr)
            return 1
    directories = module_directories(args.module_dir)
    module_dir, interpreter = select_module(directories)
    if interpreter:
        print(f"Using {interpreter} to match {module_dir}", flush=True)
        env = os.environ.copy()
        env["MXWRITE_LAUNCHER_REEXEC"] = "1"
        env["MXWRITE_PYTHON_MODULE_DIR"] = str(module_dir)
        return subprocess.call([str(interpreter), str(Path(__file__).resolve())] + argv, env=env)
    found = [path for path in directories if has_module(path)]
    if module_dir is None and (found or args.module_dir or os.environ.get("MXWRITE_PYTHON_MODULE_DIR")):
        if not found:
            print(f"No MXWrite extension found in {directories[0]}.", file=sys.stderr)
            print("Build the module first, or select its directory with --module-dir. See README.md.", file=sys.stderr)
            return 1
        print(f"No MXWrite extension compatible with Python {sys.version.split()[0]} ({sys.executable}).", file=sys.stderr)
        for directory in found:
            print(f"  {directory}: " + ", ".join(path.name for path in directory.glob("mxwrite_ext*.*") if path.suffix in (".pyd", ".so")), file=sys.stderr)
        print("Run with the Python used to build the extension, or rebuild for your Python.\nSee README.md. Use --module-dir to select another build.", file=sys.stderr)
        return 1
    handles = configure_runtime(module_dir, args.dll_dir)
    try:
        try:
            import mxwrite_ext
        except ImportError as error:
            print(f"Could not import mxwrite_ext: {error}\nPython: {sys.executable}\nModule directory: {module_dir or 'installed Python packages'}\nSee README.md for build instructions.\nFor missing DLLs, set VCPKG_ROOT, CUDA_PATH, or pass --dll-dir PATH.", file=sys.stderr)
            return 1
        if args.check:
            print(f"Python: {sys.executable} ({sys.version.split()[0]})")
            print(f"MXWrite: {mxwrite_ext.__file__}")
            print(f"Video encoders: {len(mxwrite_ext.available_video_encoders())}")
            for package in ("numpy", "cv2"):
                try:
                    imported = __import__(package)
                    print(f"{package}: {getattr(imported, '__version__', 'available')}")
                except ImportError as error:
                    print(f"{package}: unavailable ({error})")
            return 0
        if args.module:
            sys.argv = args.module
            runpy.run_module(args.module[0], run_name="__main__", alter_sys=True)
        elif args.code:
            sys.argv = ["-c"] + args.code[1:]
            exec(args.code[0], {"__name__": "__main__", "__builtins__": __builtins__})
        elif script:
            sys.path.insert(0, str(script.parent))
            sys.argv = [str(script)] + args.command[1:]
            runpy.run_path(str(script), run_name="__main__")
    except KeyboardInterrupt:
        return 130
    finally:
        for handle in handles:
            handle.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
