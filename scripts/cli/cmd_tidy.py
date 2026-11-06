"""Bounded parallel clang-tidy checks against the selected compile database."""

import asyncio
import json
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

import typer

from .cmd_build import ensure_configured
from .config import PROJECT_ROOT
from .preset import resolve_preset
from .utils import CPP_EXTENSIONS, cpp_files, find_clang_tool, get_concurrency_budget

DIAGNOSTIC_CHECK_RE = re.compile(r"(?:warning|error):\s+.*\[([^\],\s]+)(?:,[^\]]*)?\]\s*$")


def _verify_tidy_config(tool: str) -> None:
    result = subprocess.run(
        [tool, "--verify-config"],
        cwd=PROJECT_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"clang-tidy configuration check failed:\n{result.stdout}{result.stderr}")


def _supports_p2996_reflection(tool: str) -> bool:
    with tempfile.TemporaryDirectory(prefix="tidy-probe-") as temporary:
        probe = Path(temporary) / "probe.cc"
        probe.write_text("consteval auto kProbe = ^^int;\n")
        result = subprocess.run(
            [tool, str(probe), "--", "-std=c++26", "-freflection"],
            cwd=temporary,
            text=True,
            capture_output=True,
            check=False,
        )
        return result.returncode == 0


def _extract_non_clang_diagnostics(output: str) -> list[str]:
    findings: list[str] = []
    for line in output.splitlines():
        match = DIAGNOSTIC_CHECK_RE.search(line)
        if match:
            check_name = match.group(1)
            if not check_name.startswith("clang-diagnostic-"):
                findings.append(line)
    return findings


async def _check_all(commands, jobs, out, *, p2996_capable: bool = True):
    limit = asyncio.Semaphore(jobs)

    async def check(command):
        async with limit:
            process = await asyncio.create_subprocess_exec(
                *command, cwd=PROJECT_ROOT, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE
            )
            stdout, stderr = await process.communicate()
            combined = stdout.decode(errors="replace") + stderr.decode(errors="replace")
            if p2996_capable:
                failed = process.returncode != 0 or "fatal error:" in combined
                display = combined
            else:
                findings = _extract_non_clang_diagnostics(combined)
                findings.extend(line for line in combined.splitlines() if "fatal error:" in line and line not in findings)
                failed = (process.returncode is not None and process.returncode < 0) or bool(findings)
                display = "\n".join(findings) if findings else ""
            out.emit(
                "command",
                shlex.join(command),
                exit_code=1 if failed else 0,
                stdout=display if not p2996_capable else stdout.decode(errors="replace"),
                stderr="" if not p2996_capable else stderr.decode(errors="replace"),
            )
            if out.fmt == "text" and display:
                out.console.print(display, markup=False)
            return 1 if failed else 0

    codes = await asyncio.gather(*(check(command) for command in commands))
    if any(codes):
        raise RuntimeError("clang-tidy checks failed")


def tidy_cmd(
    ctx: typer.Context,
    paths: list[str] | None = typer.Argument(None, help="Source files or directories to process"),
    debug: bool = typer.Option(False, "-d", "--debug", help="Use the debug preset"),
    dev: bool = typer.Option(
        False, "-dev", "--dev", "--development", help="Use a development build with sanitizers"
    ),
    compiler: str = typer.Option(
        "clang",
        "-c",
        "--compiler",
        help="Compiler toolchain (clang, gcc, musl-clang, musl-gcc, clang-glibc, gcc-glibc, clang-musl, gcc-musl)",
    ),
    sanitizer: str = typer.Option(
        "asan-ubsan", "-s", "--sanitizer", help="Sanitizer for --dev builds (asan-ubsan)"
    ),
    preset: str | None = typer.Option(
        None,
        "--preset",
        help="Explicit CMake preset override (e.g. clang-glibc-release, gcc-glibc-release, clang-musl-release, gcc-musl-release)",
    ),
    jobs: int | None = typer.Option(
        None, "-j", "--jobs", min=1, help="Maximum parallel build or analysis jobs"
    ),
    verbose: bool = typer.Option(False, "-v", "--verbose", help="Show verbose command output"),
):
    """Run clang-tidy in parallel; reflection flags are adapted in a temporary database."""
    tool = find_clang_tool("clang-tidy")
    if not tool:
        raise ValueError("clang-tidy is unavailable; install a reflection-capable version or add it to PATH")
    _verify_tidy_config(tool)
    p2996_capable = _supports_p2996_reflection(tool)
    name = resolve_preset(debug, dev, compiler, sanitizer, preset=preset)
    directory = Path(ensure_configured(name, ctx.obj)).resolve()
    project_root = PROJECT_ROOT.resolve()
    database = json.loads((directory / "compile_commands.json").read_text())
    selected = set(cpp_files(paths)) if paths else None
    explicit_negative = any("compile_fail" in Path(path).resolve().parts for path in (paths or []))
    files = set()
    stripped_flags = {"-freflection-latest"} if p2996_capable else {"-freflection-latest", "-freflection"}
    for entry in database:
        source = (Path(entry["directory"]) / entry["file"]).resolve()
        if "compile_fail" in source.parts and not explicit_negative:
            continue
        if selected is None:
            if (
                not source.is_relative_to(project_root)
                or source.is_relative_to(directory)
                or "_deps" in source.parts
                or source.suffix not in CPP_EXTENSIONS
            ):
                continue
            files.add(source)
        elif source in selected:
            files.add(source)
        arguments = entry.get("arguments") or shlex.split(entry.pop("command"))
        entry["arguments"] = [arg for arg in arguments if arg not in stripped_flags]
    if not files:
        raise ValueError("No requested translation units appear in compile_commands.json")
    extra_args = (
        ["--extra-arg=-freflection", "--warnings-as-errors=*"]
        if p2996_capable
        else ["--extra-arg=-D__is_consteval_only(...)=false"]
    )
    with tempfile.TemporaryDirectory(prefix="tidy-", dir=directory) as temporary:
        (Path(temporary) / "compile_commands.json").write_text(json.dumps(database))
        commands = [[tool, str(file), "-p", temporary, *extra_args] for file in sorted(files)]
        asyncio.run(
            _check_all(
                commands,
                jobs or get_concurrency_budget(),
                ctx.obj,
                p2996_capable=p2996_capable,
            )
        )
