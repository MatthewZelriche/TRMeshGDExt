#!/usr/bin/env python3
"""Cross-platform CMake configure/build helper for TRMeshGDExt.

Uses Ninja and exports compile_commands.json for clangd/Zed.
On Windows, loads the MSVC environment via vcvars64 (same role as the old .ps1 scripts).

Examples:
  python cmake.py configure
  python cmake.py build
  python cmake.py all
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def die(message: str, code: int = 1) -> None:
	print(f"error: {message}", file=sys.stderr)
	raise SystemExit(code)


def which_or_die(name: str) -> str | None:
	path = shutil.which(name)
	if not path:
		die(f"'{name}' not found on PATH")
	return path


def find_vcvars64() -> Path:
	program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
	vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
	if not vswhere.is_file():
		die(f"vswhere.exe not found at {vswhere}")

	result = subprocess.run(
		[
			str(vswhere),
			"-latest",
			"-products",
			"*",
			"-requires",
			"Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
			"-property",
			"installationPath",
		],
		check=False,
		capture_output=True,
		text=True,
	)
	vs_path = (result.stdout or "").strip()
	if result.returncode != 0 or not vs_path:
		die("no Visual Studio install with C++ tools found")

	vcvars = Path(vs_path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
	if not vcvars.is_file():
		die(f"vcvars64.bat not found at {vcvars}")
	return vcvars


def msvc_environ() -> dict[str, str]:
	"""Return os.environ extended with vars from vcvars64.bat."""
	vcvars = find_vcvars64()
	# Windows environment names are case-insensitive, but Python dictionaries are
	# not. Canonicalize them so stale variants such as both PATH and Path cannot
	# cause a child process to miss the paths added by vcvars64.
	base_env = {key.upper(): value for key, value in os.environ.items()}
	# `call` is required so cmd runs the .bat; `set` dumps KEY=VALUE lines.
	# shell=True avoids the broken quoting you get from list-form `"path with spaces"`.
	completed = subprocess.run(
		f'call "{vcvars}" && set',
		check=False,
		capture_output=True,
		env=base_env,
		text=True,
		shell=True,
	)
	if completed.returncode != 0:
		die(f"failed to run vcvars64.bat\n{completed.stderr}")

	env: dict[str, str] = {}
	for line in completed.stdout.splitlines():
		if "=" not in line:
			continue
		key, _, value = line.partition("=")
		# Skip cmd banner / non-env noise.
		if not key or key.startswith(" ") or "*" in key:
			continue
		env[key.upper()] = value
	if not shutil.which("cl", path=env.get("PATH", "")):
		die("vcvars64 ran but cl.exe is still not on PATH")
	return env


def command_environ() -> dict[str, str]:
	if sys.platform == "win32":
		return msvc_environ()
	return os.environ.copy()


def run_cmake(args: list[str], env: dict[str, str]) -> None:
	which_or_die("cmake")
	print("+", "cmake", *args)
	completed = subprocess.run(["cmake", *args], cwd=ROOT, env=env, check=False)
	if completed.returncode != 0:
		die(f"cmake failed with exit code {completed.returncode}", completed.returncode)


def require_ninja(env: dict[str, str]) -> None:
	if not shutil.which("ninja", path=env.get("PATH", os.environ.get("PATH", ""))):
		die("ninja not found on PATH (required for compile_commands.json / clangd)")


def configure(build_dir: Path, extra_cmake_args: list[str]) -> None:
	env = command_environ()
	require_ninja(env)

	run_cmake(
		[
			"-S",
			str(ROOT),
			"-B",
			str(build_dir),
			"-G",
			"Ninja",
			"-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
			*extra_cmake_args,
		],
		env,
	)
	db = build_dir / "compile_commands.json"
	print(f"configured. compile_commands: {db}")


def build(build_dir: Path, extra_cmake_args: list[str]) -> None:
	if not build_dir.exists():
		die(f"build dir '{build_dir}' missing; run: python cmake.py configure")
	env = command_environ()
	run_cmake(["--build", str(build_dir), *extra_cmake_args], env)


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument(
		"command",
		choices=("configure", "build", "all"),
		help="configure, build, or both",
	)
	parser.add_argument(
		"-B",
		"--build-dir",
		type=Path,
		default=ROOT / "build",
		help="build directory (default: ./build)",
	)
	parser.add_argument(
		"cmake_args",
		nargs="*",
		help="extra args after '--' are passed through to cmake",
	)

	# Split passthrough arguments explicitly so options such as -B may appear
	# between the command and `--`. Also retain positional CMake arguments.
	argv = sys.argv[1:]
	passthrough: list[str] = []
	if "--" in argv:
		idx = argv.index("--")
		passthrough = argv[idx + 1 :]
		argv = argv[:idx]

	args = parser.parse_args(argv)
	cmake_args = [*args.cmake_args, *passthrough]
	build_dir = args.build_dir if args.build_dir.is_absolute() else ROOT / args.build_dir

	if args.command in ("configure", "all"):
		# -D options belong on configure; for `all`, pass them there.
		configure(build_dir, cmake_args)
	if args.command in ("build", "all"):
		build(build_dir, cmake_args if args.command == "build" else [])


if __name__ == "__main__":
	main()
