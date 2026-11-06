"""Resolve supported toolchains against the actual configure presets."""

import json
import re
from pathlib import Path

from .config import COMPILER_ALIASES, PROJECT_ROOT


def _strip_json_comments(text: str) -> str:
    pattern = r'("(?:[^"\\]|\\.)*")|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda m: m.group(1) if m.group(1) is not None else " ", text)


def _load_presets_data() -> dict:
    return json.loads(_strip_json_comments((PROJECT_ROOT / "CMakePresets.json").read_text()))


def load_available_presets() -> set[str]:
    return {p["name"] for p in _load_presets_data()["configurePresets"] if not p.get("hidden", False)}


def get_preset_cache_variable(name: str, key: str):
    presets = {p["name"]: p for p in _load_presets_data()["configurePresets"]}

    def walk(current, seen):
        if current in seen:
            raise ValueError("Cyclic preset inheritance")
        p = presets[current]
        if key in p.get("cacheVariables", {}):
            v = p["cacheVariables"][key]
            return v.get("value") if isinstance(v, dict) else v
        parents = p.get("inherits", [])
        for parent in [parents] if isinstance(parents, str) else parents:
            value = walk(parent, seen | {current})
            if value is not None:
                return value
        return None

    return walk(name, set())


def resolve_preset(debug=False, dev=False, compiler="clang", sanitizer="asan-ubsan", *, preset=None) -> str:
    if debug and dev:
        raise ValueError("--debug and --dev are mutually exclusive")
    if compiler not in COMPILER_ALIASES:
        raise ValueError(f"Unknown compiler {compiler!r}; choose from {', '.join(COMPILER_ALIASES)}")
    if preset is None:
        toolchain = COMPILER_ALIASES[compiler]
        if sanitizer != "asan-ubsan":
            raise ValueError("Only asan-ubsan is supported by the liburing presets")
        preset = f"{toolchain}-{'asan-ubsan' if dev else 'debug' if debug else 'release'}"
    available = load_available_presets()
    if preset not in available:
        raise ValueError(f"Unknown preset {preset!r}; available: {', '.join(sorted(available))}")
    return preset


def get_preset_compiler(name: str):
    compiler = get_preset_cache_variable(name, "CMAKE_CXX_COMPILER")
    if compiler:
        return str(compiler).replace("${sourceDir}", str(PROJECT_ROOT))
    presets = {p["name"]: p for p in _load_presets_data()["configurePresets"]}

    def toolchain(current, seen):
        if current in seen:
            raise ValueError("Cyclic preset inheritance")
        entry = presets[current]
        if entry.get("toolchainFile"):
            return entry["toolchainFile"]
        parents = entry.get("inherits", [])
        for parent in [parents] if isinstance(parents, str) else parents:
            found = toolchain(parent, seen | {current})
            if found:
                return found
        return None

    filename = toolchain(name, set()) or get_preset_cache_variable(name, "CMAKE_TOOLCHAIN_FILE")
    if not filename:
        return None
    path = Path(str(filename).replace("${sourceDir}", str(PROJECT_ROOT)))
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    match = re.search(r'(?m)^\s*set\(\s*CMAKE_CXX_COMPILER\s+(?:"([^"\n]+)"|([^\s)]+))', path.read_text())
    if not match:
        return None
    return (match.group(1) or match.group(2)).replace("${CMAKE_CURRENT_LIST_DIR}", str(path.parent))
