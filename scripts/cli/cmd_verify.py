"""Audit native probe object files with LLVM binary utilities or invoke liburing_verify."""

import math
import re
import subprocess
from pathlib import Path

import typer

from .cmd_bench import find_binary
from .cmd_build import build_targets, ensure_configured
from .preset import resolve_preset
from .utils import capture, find_clang_tool, list_ninja_targets, read_cmake_cache_value

FORBIDDEN_SYMBOL_PATTERNS = ("__asan", "__ubsan", "__cxa_throw", "__gxx_personality_v0", "_ZTI")
FORBIDDEN_SECTION_PATTERNS = (".gcc_except_table",)


def _resolve_llvm_tool(name: str) -> str:
    tool = find_clang_tool(name)
    if not tool:
        raise ValueError(f"Required LLVM utility is unavailable: {name}")
    return tool


def discover_probe_objects(build_dir: Path) -> list[Path]:
    build_dir = Path(build_dir)
    return sorted(
        path
        for path in build_dir.rglob("*.o")
        if path.is_file() and ("probe" in path.name or any("probe" in part for part in path.parts))
    )


# These entry points promise register-only argument/return handling; larger engines may use a frame.
ABI_LEAF_PROBES = {
    "abi_cqe_echo", "abi_sqe_offset", "abi_cmd_store", "syscall_pid_probe",
    "t2_generation_delta", "t2_key_pair", "t2_completion_pair", "t2_fsm_next",
    "OpsAddressValue", "futex_ref_echo", "futex_load_probe", "futex_store_probe",
    "futex_add_probe", "proactor_token_pack_probe",
}


# Ceilings allow compiler/native-CPU variation while rejecting order-of-magnitude regressions.
MCA_CPUS = ("znver4", "znver5", "sapphirerapids")
MCA_RTHROUGHPUT_BUDGETS = {name: 12.0 for name in ABI_LEAF_PROBES}
MCA_RTHROUGHPUT_BUDGETS.update({
    "syscall_pid_probe": 64.0, "futex_wake_probe": 64.0, "futex_add_probe": 64.0,
    "proactor_cq_reap_probe": 64.0, "proactor_static_dispatch_probe": 48.0, "proactor_fsm_step_probe": 32.0,
    "t2_advance": 16.0, "t2_retire": 12.0, "t2_token_roundtrip": 12.0,
    "OpsEncodeNop": 16.0, "OpsEncodeRead": 16.0, "OpsEncodeFutex": 16.0,
    "OpsEncodeMultishot": 16.0, "OpsEncodeCommand128": 16.0,
    # Cold capability discovery and invariant failure are not register-only hot probes.
    "main": 256.0, "invariant_panic_probe": 64.0,
})


def parse_disassembly(disassembly: str) -> dict[str, list[str]]:
    functions: dict[str, list[str]] = {}
    current = None
    for line in disassembly.splitlines():
        symbol = re.match(r"^(?:[0-9a-fA-F]+ )?<([^>]+)>:$", line.strip())
        if symbol:
            current = functions.setdefault(symbol[1], [])
            continue
        relocation = re.search(r"R_X86_64_(?:PLT32|PC32)\s+(\S+)", line)
        if relocation and current and current[-1].split()[0] in {"jmp", "call", "callq"}:
            target = re.sub(r"[-+]0x[0-9a-fA-F]+$", "", relocation[1])
            current[-1] = current[-1].split()[0] + " " + target
            continue
        instruction = re.match(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)*([a-zA-Z].*)$", line)
        if instruction is None:
            instruction = re.match(r"^\s+([a-z][a-z0-9]*(?:\s+.*)?)$", line)
        if current is not None and instruction:
            current.append(instruction[1].strip())
    if not functions or not any(functions.values()):
        raise RuntimeError("No instructions found in probe disassembly")
    return functions


def _indirect_call(instruction: str) -> bool:
    return bool(re.match(r"callq?\s+(?:r[a-z0-9]+\b|.*\[)", instruction))


def audit_abi(functions: dict[str, list[str]], out, obj: str) -> None:
    rows = []
    for name, instructions in functions.items():
        if name not in ABI_LEAF_PROBES:
            continue
        for instruction in instructions:
            if re.search(r"\b(?:push|pop)\b|\b(?:sub|add|and|mov|lea)\s+(?:rsp|rbp)\b|\[[^\]]*\b(?:rsp|rbp)\b", instruction):
                raise RuntimeError(f"Stack use in register-only ABI probe {name} ({obj}): {instruction}")
            if instruction.startswith("call"):
                raise RuntimeError(f"Call in register-only ABI probe {name} ({obj}): {instruction}")
        rows.append([obj, name, str(len(instructions)), "register-only"])
    out.table("ABI verification", ["Object", "Leaf probe", "Instructions", "ABI"], rows)


def audit_instructions(functions: dict[str, list[str]], out, obj: str) -> None:
    total = branches = calls = syscalls = 0
    for name, instructions in functions.items():
        # Leaf entry points and static dispatch must never acquire an indirect-call dependency.
        leaf = name in ABI_LEAF_PROBES
        for instruction in instructions:
            mnemonic = instruction.split()[0]
            if mnemonic in {"div", "idiv", "divb", "divw", "divl", "divq", "idivb", "idivw", "idivl", "idivq"}:
                raise RuntimeError(f"Hardware division in {name} ({obj}): {instruction}")
            if (leaf or "static_dispatch" in name) and _indirect_call(instruction):
                raise RuntimeError(f"Indirect call in {name} ({obj}): {instruction}")
            total += 1
            branches += mnemonic.startswith("j") or mnemonic.startswith("loop")
            calls += mnemonic.startswith("call")
            syscalls += mnemonic == "syscall"
    out.table("Instruction audit", ["Object", "Instructions", "Branches", "Calls", "Syscalls"],
              [[obj, str(total), str(branches), str(calls), str(syscalls)]])


def _mca_metrics(instructions: list[str], obj: str, cpu: str) -> tuple[list[str], str]:
    assembly = [".intel_syntax noprefix"]
    for instruction in instructions:
        instruction = re.sub(r"\s*<[^>]+>", "", instruction).split("#", 1)[0].strip()
        # llvm-mca models the instruction stream, not linked addresses or control flow.
        instruction = re.sub(r"^((?:j\w+|loop\w*|callq?)\s+)(?:0x)?[0-9a-fA-F]+$",
                             r"\1.Lmca_target", instruction)
        assembly.append(instruction)
    assembly.append(".Lmca_target:")
    try:
        result = subprocess.run([_resolve_llvm_tool("llvm-mca"), f"--mcpu={cpu}"],
                                input="\n".join(assembly) + "\n", text=True, capture_output=True, check=True)
    except subprocess.CalledProcessError as error:
        if cpu != "native" and re.search(r"(?:unknown|unsupported|unrecognized).*?(?:mcpu|cpu)", error.stderr or "", re.IGNORECASE):
            return _mca_metrics(instructions, obj, "native")
        raise RuntimeError(f"llvm-mca rejected {obj} on {cpu}: {error.stderr}") from error
    if "not a recognized processor" in getattr(result, "stderr", ""):
        raise RuntimeError(f"llvm-mca does not support required CPU {cpu}")
    metrics = []
    for key in ("Instructions", "Total Cycles", "Total uOps", "Block RThroughput"):
        match = re.search(rf"^{key}:\s+([0-9.]+)\s*$", result.stdout, re.MULTILINE)
        if not match:
            raise RuntimeError(f"Missing llvm-mca metric '{key}' for {obj}")
        value = float(match[1])
        if not math.isfinite(value) or value <= 0:
            raise RuntimeError(f"Invalid llvm-mca throughput/metric '{key}' for {obj}: {value}")
        metrics.append(match[1])
    return metrics, cpu


def audit_mca(functions: dict[str, list[str]], out, obj: str, *, gcc: bool = False) -> None:
    rows = []
    for name, instructions in functions.items():
        if not instructions:
            raise RuntimeError(f"Empty probe symbol {name} in {obj}")
        # Resolve local tail-call trampolines so the budget covers their implementation as well.
        modeled = list(instructions)
        seen = {name}
        target = instructions[-1].split() if len(instructions) == 1 else []
        while len(target) == 2 and target[0] == "jmp" and target[1] in functions and target[1] not in seen:
            seen.add(target[1])
            body = functions[target[1]]
            modeled.extend(body)
            target = body[-1].split() if len(body) == 1 else []
        budget = MCA_RTHROUGHPUT_BUDGETS.get(
            name, 384.0 if name.startswith("_ZN8liburing4caps10KernelCaps5Probe") else
            128.0 if name.startswith("_ZN8liburing4caps19VerifyKernelVersion") else
            32.0 if name.startswith("_ZN8liburing4caps18ParseKernelVersion") else
            64.0 if "CqCursor9ReapBatch" in name or "syscall" in name or "atomic" in name or name.startswith("_ZN8liburing3sys5Panic") else 24.0)
        # Clang stays <=16 on these CPUs; GCC emits 23.8 on Zen4/5 and 25.5 on Sapphire Rapids.
        if gcc and name == "t2_advance":
            budget = 28.0
        for cpu in MCA_CPUS:
            metrics, modeled_cpu = _mca_metrics(modeled, f"{obj}:{name}", cpu)
            if float(metrics[3]) > budget:
                raise RuntimeError(f"MCA budget exceeded for {name} ({obj}) on {cpu}: {metrics[3]} > {budget}")
            rows.append([obj, name, modeled_cpu, *metrics, str(budget)])
    out.table("llvm-mca throughput (per symbol and target CPU, linear stream)",
              ["Object", "Symbol", "CPU", "Instructions", "Total Cycles", "Total uOps", "Block RThroughput", "Budget"], rows)


def audit_disassembly(functions: dict[str, list[str]], out, obj: str, gate: str, *, gcc: bool = False) -> None:
    if gate in {"all", "abi-check"}:
        audit_abi(functions, out, obj)
    if gate in {"all", "asm-check"}:
        audit_instructions(functions, out, obj)
    if gate in {"all", "mca-gate"}:
        audit_mca(functions, out, obj, gcc=gcc)


def audit_probe_objects(build_dir: Path, out, *, gate: str = "all") -> list[dict[str, str]]:
    objects = discover_probe_objects(build_dir)
    if not objects:
        raise ValueError(f"No probe object files found under {build_dir}")
    llvm_nm = _resolve_llvm_tool("llvm-nm")
    llvm_readobj = _resolve_llvm_tool("llvm-readobj")
    llvm_objdump = _resolve_llvm_tool("llvm-objdump")
    llvm_size = _resolve_llvm_tool("llvm-size")

    rows: list[list[str]] = []
    summaries: list[dict[str, str]] = []
    found_abi = set()
    compiler = read_cmake_cache_value(build_dir / "CMakeCache.txt", "CMAKE_CXX_COMPILER") or ""
    gcc = Path(compiler).name.endswith("g++")
    for obj in objects:
        header = obj.read_bytes()[:4]
        if header != b"\x7fELF":
            raise RuntimeError(f"Probe object is not native ELF: {obj}")

        nm_out = capture([llvm_nm, "-S", "--format=posix", obj])
        readobj_out = capture([llvm_readobj, "--sections", "--symbols", obj])
        objdump_out = capture([llvm_objdump, "-dr", "-M", "intel", "--no-show-raw-insn", "--no-leading-addr", obj])
        size_out = capture([llvm_size, obj])

        combined = "\n".join((nm_out, readobj_out, objdump_out))
        for forbidden in FORBIDDEN_SYMBOL_PATTERNS:
            if forbidden in combined:
                raise RuntimeError(f"Forbidden symbol '{forbidden}' detected in {obj}")
        for section in FORBIDDEN_SECTION_PATTERNS:
            if section in readobj_out:
                raise RuntimeError(f"Forbidden exception section '{section}' detected in {obj}")

        defined_symbols = []
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1] in {"T", "t", "W", "w"}:
                defined_symbols.append(parts[0])
        if not defined_symbols:
            raise RuntimeError(f"No defined text symbols in probe object: {obj}")

        size_lines = [line.strip() for line in size_out.splitlines() if line.strip()]
        size_row = size_lines[-1].split() if len(size_lines) >= 2 else ["0", "0", "0", "0"]
        text_bytes, data_bytes, bss_bytes, total_bytes = (size_row + ["0", "0", "0", "0"])[:4]
        rel_obj = str(obj.relative_to(build_dir)) if obj.is_relative_to(build_dir) else str(obj)
        if gate in {"all", "abi-check", "asm-check", "mca-gate"}:
            functions = parse_disassembly(objdump_out)
            found_abi.update(name for name in defined_symbols if functions.get(name))
            audit_disassembly(functions, out, rel_obj, gate, gcc=gcc)
        rows.append([rel_obj, str(len(defined_symbols)), text_bytes, data_bytes, bss_bytes, total_bytes])
        summaries.append(
            {
                "object": rel_obj,
                "symbols": str(len(defined_symbols)),
                "text": text_bytes,
                "data": data_bytes,
                "bss": bss_bytes,
                "total": total_bytes,
            }
        )

    if gate in {"all", "abi-check"}:
        missing = ABI_LEAF_PROBES - found_abi
        if missing:
            raise RuntimeError(f"Missing contracted ABI probes: {', '.join(sorted(missing))}")
    out.table(
        f"Native probe verification ({gate})",
        ["Object", "Symbols", "Text (B)", "Data (B)", "BSS (B)", "Total (B)"],
        rows,
    )
    out.info(f"Verified {len(objects)} native probe object(s) with zero sanitizer/exception/RTTI symbols")
    return summaries


def verify_cmd(
    ctx: typer.Context,
    gate: str = typer.Argument(
        "all",
        help="Gate: all, lint, abi-check, asm-check, mca-gate, size-report, pmu-gate, gem5-gate; aliases: abi, asm, mca, symbols, size, pmu, tma, gem5, o3",
    ),
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
    verbose: bool = typer.Option(False, "-v", "--verbose", help="Show verbose command output"),
):
    """Run a native verification gate using its canonical name or short alias."""
    gates = {"all", "lint", "abi-check", "asm-check", "mca-gate", "size-report", "pmu-gate", "gem5-gate"}
    aliases = {
        "abi": "abi-check",
        "asm": "asm-check",
        "mca": "mca-gate",
        "symbols": "size-report",
        "size": "size-report",
        "pmu": "pmu-gate",
        "tma": "pmu-gate",
        "gem5": "gem5-gate",
        "o3": "gem5-gate",
    }
    gate = aliases.get(gate, gate)
    if gate not in gates:
        raise ValueError(f"Unknown verification gate; choose {', '.join(sorted(gates | aliases.keys()))}")
    name = resolve_preset(debug, dev, compiler, sanitizer, preset=preset)
    directory = ensure_configured(name, ctx.obj, components={"tests": True})
    targets = dict(list_ninja_targets(directory))
    if "liburing_verify" in targets:
        runner_targets = ["liburing_verify"]
        if "liburing_probes" in targets:
            runner_targets.append("liburing_probes")
        build_targets(directory, ctx.obj, runner_targets, verbose=verbose)
        ctx.obj.run_command([find_binary(directory, "liburing_verify"), gate])
        return
    if gate in {"pmu-gate", "gem5-gate"}:
        raise ValueError(f"Tier 2 gate '{gate}' requires hardware PMU / gem5 harness and is not implemented in static probe mode")
    probe_targets = [target for target in targets if "probe" in target]
    build_targets(directory, ctx.obj, probe_targets or None, verbose=verbose)
    audit_probe_objects(directory, ctx.obj, gate=gate)
