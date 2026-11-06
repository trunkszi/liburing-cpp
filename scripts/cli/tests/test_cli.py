"""Exercise the launcher and preset selection through their public interfaces."""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))
for venv_site in (ROOT / "scripts/cli/.venv/lib").glob("python*/site-packages"):
    if str(venv_site) not in sys.path:
        sys.path.insert(0, str(venv_site))


class CliContracts(unittest.TestCase):
    def test_launcher_help(self):
        result = subprocess.run([ROOT / "uring", "--help"], check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in ("build", "test", "format", "tidy", "verify", "bench"):
            self.assertIn(command, result.stdout)

    def test_all_twelve_presets(self):
        from cli.preset import resolve_preset

        for compiler in ("clang-glibc", "gcc-glibc", "clang-musl", "gcc-musl"):
            self.assertEqual(resolve_preset(compiler=compiler), compiler + "-release")
            self.assertEqual(resolve_preset(compiler=compiler, debug=True), compiler + "-debug")
            self.assertEqual(resolve_preset(compiler=compiler, dev=True), compiler + "-asan-ubsan")

    def test_json_invalid_preset(self):
        result = subprocess.run(
            [ROOT / "uring", "--format", "json", "build", "--preset", "absent-preset"],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout)["success"])

    def test_fragment_discovery(self):
        from cli.preset import get_preset_compiler

        with tempfile.TemporaryDirectory(prefix="liburing-fragments-") as temporary:
            root = Path(temporary)
            shutil.copy(ROOT / "CMakeLists.txt", root)
            directories = ("cmake", "tests", "benches")
            for optional in ("src", "examples"):
                if (ROOT / optional).is_dir():
                    directories += (optional,)
            for directory in directories:
                shutil.copytree(
                    ROOT / directory, root / directory, ignore=shutil.ignore_patterns("__pycache__")
                )
            (root / "cmake/T90.cmake").write_text("add_custom_target(fragment_library)\n")
            (root / "tests/T90.cmake").write_text(
                'add_test(NAME fragment_case COMMAND "${CMAKE_COMMAND}" -E true)\n'
            )
            (root / "benches/T90.cmake").write_text("add_custom_target(fragment_bench)\n")
            build = root / "build"
            command = [
                "cmake",
                "-S",
                root,
                "-B",
                build,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                "-DLIBURING_BUILD_TESTS=ON",
                "-DLIBURING_BUILD_BENCHES=ON",
                f"-DCMAKE_CXX_COMPILER={get_preset_compiler('clang-glibc-release')}",
            ]
            result = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            (root / "cmake/T91.cmake").write_text("add_custom_target(fragment_late)\n")
            result = subprocess.run(
                ["cmake", "--build", build, "--target", "fragment_library"],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            targets = subprocess.check_output(["ninja", "-C", build, "-t", "targets", "all"], text=True)
            self.assertIn("fragment_late:", targets)
            self.assertIn("fragment_bench:", targets)
            cases = json.loads(
                subprocess.check_output(
                    ["ctest", "--test-dir", build, "--show-only=json-v1"],
                    text=True,
                )
            )
            self.assertIn("fragment_case", [case["name"] for case in cases["tests"]])

    def test_find_clang_tool_override_and_versioned_fallback(self):
        from unittest import mock

        from cli import utils

        with tempfile.TemporaryDirectory(prefix="liburing-tool-") as temporary:
            custom_tool = Path(temporary) / "custom-clang-tidy"
            custom_tool.write_text("#!/bin/sh\nexit 0\n")
            custom_tool.chmod(0o755)
            with mock.patch.dict("os.environ", {"LIBURING_CLANG_TIDY_BIN": str(custom_tool)}, clear=False):
                self.assertEqual(utils.find_clang_tool("clang-tidy"), str(custom_tool))

        def fake_which(candidate):
            if candidate == "clang-tidy-23":
                return "/usr/bin/clang-tidy-23"
            if candidate == "clang-tidy":
                return "/usr/bin/clang-tidy"
            return None

        with (
            mock.patch.dict("os.environ", {}, clear=True),
            mock.patch.object(utils.os, "access", return_value=False),
            mock.patch.object(utils.shutil, "which", side_effect=fake_which),
        ):
            self.assertEqual(utils.find_clang_tool("clang-tidy"), "/usr/bin/clang-tidy-23")

    def test_verify_audit_probe_objects_detects_clean_and_forbidden_symbols(self):
        from types import SimpleNamespace
        from unittest import mock

        from cli import cmd_verify

        with tempfile.TemporaryDirectory(prefix="liburing-verify-") as temporary:
            build = Path(temporary)
            probe_dir = build / "tests/CMakeFiles/native_smoke_probe.dir/unit"
            probe_dir.mkdir(parents=True)
            probe_obj = probe_dir / "smoke_test.cc.o"
            probe_obj.write_bytes(b"\x7fELF\x02\x01\x01\x00")

            tables = []
            infos = []
            out = SimpleNamespace(
                table=lambda title, cols, rows: tables.append((title, cols, rows)),
                info=lambda msg: infos.append(msg),
            )

            def clean_capture(cmd, cwd=None):
                tool = Path(cmd[0]).name
                if tool == "llvm-nm":
                    return "abi_cqe_echo T 0 10\n"
                if tool == "llvm-readobj":
                    return "Section {\n  Name: .text\n}\n"
                if tool == "llvm-objdump":
                    return "0000000000000000 <abi_cqe_echo>:\n  0: xor eax, eax\n  2: ret\n"
                if tool == "llvm-size":
                    return "   text    data     bss     dec     hex filename\n     16       0       0      16      10 smoke_test.cc.o\n"
                raise AssertionError(f"Unexpected tool: {tool}")

            with (
                mock.patch.object(cmd_verify, "_resolve_llvm_tool", side_effect=lambda name: f"/bin/{name}"),
                mock.patch.object(cmd_verify, "capture", side_effect=clean_capture),
            ):
                summaries = cmd_verify.audit_probe_objects(build, out, gate="lint")
                self.assertEqual(len(summaries), 1)
                self.assertEqual(summaries[0]["text"], "16")
                self.assertTrue(tables)
                self.assertTrue(infos)

            def tainted_capture(cmd, cwd=None):
                tool = Path(cmd[0]).name
                if tool == "llvm-nm":
                    return "__asan_report_load8 U\nmain T 0 10\n"
                return clean_capture(cmd, cwd=cwd)

            with (
                mock.patch.object(cmd_verify, "_resolve_llvm_tool", side_effect=lambda name: f"/bin/{name}"),
                mock.patch.object(cmd_verify, "capture", side_effect=tainted_capture),
            ):
                with self.assertRaisesRegex(RuntimeError, "Forbidden symbol '__asan'"):
                    cmd_verify.audit_probe_objects(build, out, gate="lint")

    def test_verify_requires_complete_abi_contract(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify
        with tempfile.TemporaryDirectory() as temporary:
            obj = Path(temporary) / "probe.o"
            obj.write_bytes(b"\x7fELF")
            def output(command):
                tool = Path(command[0]).name
                if tool == "llvm-nm":
                    return "abi_cqe_echo T 0 1\n"
                if tool == "llvm-objdump":
                    return "<abi_cqe_echo>:\n  ret\n"
                if tool == "llvm-size":
                    return "text data bss dec\n1 0 0 1\n"
                return ""
            with (mock.patch.object(cmd_verify, "discover_probe_objects", return_value=[obj]),
                  mock.patch.object(cmd_verify, "_resolve_llvm_tool", side_effect=lambda name: name),
                  mock.patch.object(cmd_verify, "capture", side_effect=output)):
                with self.assertRaisesRegex(RuntimeError, "Missing contracted ABI probes"):
                    cmd_verify.audit_probe_objects(Path(temporary), SimpleNamespace(table=mock.Mock(), info=mock.Mock()), gate="abi-check")

    def test_verify_mca_rejects_symbol_budget_regression(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify
        metrics = "Instructions: 100\nTotal Cycles: 100\nTotal uOps: 100\nBlock RThroughput: 1000.0\n"
        with (mock.patch.object(cmd_verify, "_resolve_llvm_tool", return_value="llvm-mca"),
              mock.patch.object(cmd_verify.subprocess, "run", return_value=SimpleNamespace(stdout=metrics))):
            with self.assertRaisesRegex(RuntimeError, "MCA budget exceeded.*abi_cqe_echo"):
                cmd_verify.audit_mca({"abi_cqe_echo": ["ret"]}, SimpleNamespace(table=mock.Mock()), "probe.o")

    def test_verify_gate_specific_instruction_checks(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify

        out = SimpleNamespace(table=mock.Mock(), info=mock.Mock())
        clean = {"abi_cqe_echo": ["mov rax, rdi", "ret"]}
        cmd_verify.audit_abi(clean, out, "probe.o")
        cmd_verify.audit_instructions(clean, out, "probe.o")
        for instructions in (["push rbp", "ret"], ["sub rsp, 16", "ret"],
                             ["mov qword ptr [rsp - 8], rax", "ret"]):
            with self.assertRaisesRegex(RuntimeError, "Stack"):
                cmd_verify.audit_abi({"abi_cqe_echo": instructions}, out, "probe.o")
        for name, instructions, diagnostic in (
            ("arbitrary_probe", ["idiv rcx"], "division"),
            ("abi_cqe_echo", ["call rax"], "Indirect"),
            ("proactor_static_dispatch_probe", ["call qword ptr [rax]"], "Indirect"),
        ):
            with self.assertRaisesRegex(RuntimeError, diagnostic):
                cmd_verify.audit_instructions({name: instructions}, out, "probe.o")
        with mock.patch.object(cmd_verify, "audit_mca") as mca:
            for gate in ("abi-check", "asm-check", "mca-gate", "all"):
                out.table.reset_mock()
                cmd_verify.audit_disassembly(clean, out, "probe.o", gate)
                titles = [call.args[0] for call in out.table.call_args_list]
                self.assertEqual(any("ABI" in title for title in titles), gate in ("abi-check", "all"))
                self.assertEqual(any("Instruction" in title for title in titles), gate in ("asm-check", "all"))
            self.assertEqual(mca.call_count, 2)

    def test_verify_mca_executes_and_validates_metrics(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify

        functions = {"abi_cqe_echo": ["mov rax, rdi", "jne 0x20 <abi_cqe_echo+0x20>", "ret"]}
        metrics = "Instructions: 300\nTotal Cycles: 105\nTotal uOps: 300\nBlock RThroughput: 1.0\n"
        out = SimpleNamespace(table=mock.Mock())
        with (mock.patch.object(cmd_verify, "_resolve_llvm_tool", return_value="llvm-mca"),
              mock.patch.object(cmd_verify.subprocess, "run", return_value=SimpleNamespace(stdout=metrics)) as run):
            cmd_verify.audit_mca(functions, out, "probe.o")
            self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                             [f"--mcpu={cpu}" for cpu in ("znver4", "znver5", "sapphirerapids")])
            self.assertIn(".intel_syntax noprefix", run.call_args.kwargs["input"])
            self.assertIn("jne .Lmca_target", run.call_args.kwargs["input"])
            self.assertTrue(run.call_args.kwargs["check"])
            run.return_value.stdout = metrics.replace("1.0", "0.0")
            with self.assertRaisesRegex(RuntimeError, "throughput"):
                cmd_verify.audit_mca(functions, out, "probe.o")
            run.return_value.stdout = "Instructions: 300\n"
            with self.assertRaisesRegex(RuntimeError, "metric"):
                cmd_verify.audit_mca(functions, out, "probe.o")

    def test_verify_mca_checks_every_cpu_budget(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify
        metrics = "Instructions: 100\nTotal Cycles: 100\nTotal uOps: 100\nBlock RThroughput: 1.0\n"
        def result(command, **kwargs):
            return SimpleNamespace(stdout=metrics.replace("1.0", "1000.0") if command[-1] == "--mcpu=znver5" else metrics, stderr="")
        with (mock.patch.object(cmd_verify, "_resolve_llvm_tool", return_value="llvm-mca"),
              mock.patch.object(cmd_verify.subprocess, "run", side_effect=result)):
            with self.assertRaisesRegex(RuntimeError, "budget exceeded.*znver5"):
                cmd_verify.audit_mca({"abi_cqe_echo": ["ret"]}, SimpleNamespace(table=mock.Mock()), "probe.o")

    def test_verify_mca_gcc_advance_baseline(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify
        metrics = "Instructions: 100\nTotal Cycles: 100\nTotal uOps: 100\nBlock RThroughput: 25.5\n"
        with (mock.patch.object(cmd_verify, "_resolve_llvm_tool", return_value="llvm-mca"),
              mock.patch.object(cmd_verify.subprocess, "run", return_value=SimpleNamespace(stdout=metrics, stderr=""))):
            out = SimpleNamespace(table=mock.Mock())
            with self.assertRaisesRegex(RuntimeError, "budget exceeded.*t2_advance"):
                cmd_verify.audit_mca({"t2_advance": ["ret"]}, out, "probe.o")
            cmd_verify.audit_mca({"t2_advance": ["ret"]}, out, "probe.o", gcc=True)

    def test_verify_mca_stub_cpu_fallback(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify
        metrics = "Instructions: 100\nTotal Cycles: 100\nTotal uOps: 100\nBlock RThroughput: 1.0\n"
        def result(command, **kwargs):
            if command[-1] != "--mcpu=native":
                raise subprocess.CalledProcessError(1, command, stderr="unknown mcpu target in synthetic stub")
            return SimpleNamespace(stdout=metrics, stderr="")
        with (mock.patch.object(cmd_verify, "_resolve_llvm_tool", return_value="llvm-mca"),
              mock.patch.object(cmd_verify.subprocess, "run", side_effect=result)):
            cmd_verify.audit_mca({"abi_cqe_echo": ["ret"]}, SimpleNamespace(table=mock.Mock()), "probe.o")

    def test_verify_tier2_cli_exits_nonzero(self):
        for gate in ("pmu", "gem5"):
            with self.subTest(gate=gate):
                result = subprocess.run([ROOT / "uring", "verify", gate, "--preset", "clang-glibc-release"],
                                        check=False, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("not implemented in static probe mode", " ".join((result.stdout + result.stderr).split()))

    def test_verify_disassembly_extraction(self):
        from cli import cmd_verify
        raw = "0000000000000000 <abi_cqe_echo>:\n  0: 48 89 f8 mov rax, rdi\n  3: c3 ret\n"
        plain = "<abi_cqe_echo>:\n  mov rax, rdi\n  ret\n"
        expected = {"abi_cqe_echo": ["mov rax, rdi", "ret"]}
        self.assertEqual(cmd_verify.parse_disassembly(raw), expected)
        self.assertEqual(cmd_verify.parse_disassembly(plain), expected)
        trampoline = "<wrapper>:\n  jmp 0x5 <wrapper+0x5>\n\tR_X86_64_PLT32 target-0x4\n"
        self.assertEqual(cmd_verify.parse_disassembly(trampoline), {"wrapper": ["jmp target"]})
        with self.assertRaisesRegex(RuntimeError, "No instructions"):
            cmd_verify.parse_disassembly("garbage")

    def test_verify_tier2_requires_runner(self):
        from types import SimpleNamespace
        from unittest import mock
        from cli import cmd_verify

        with (mock.patch.object(cmd_verify, "ensure_configured", return_value=Path("/tmp/build")),
              mock.patch.object(cmd_verify, "list_ninja_targets", return_value={}),
              mock.patch.object(cmd_verify, "build_targets") as build):
            for gate in ("pmu", "gem5"):
                with self.assertRaisesRegex(ValueError, "Tier 2 gate.*not implemented in static probe mode"):
                    cmd_verify.verify_cmd(SimpleNamespace(obj=None), gate=gate, debug=False, dev=False,
                                          compiler="clang", sanitizer="asan-ubsan",
                                          preset="clang-glibc-release", verbose=False)
            build.assert_not_called()

    def test_tidy_filters_project_cpp_and_flags_non_clang_diagnostics(self):
        from types import SimpleNamespace
        from unittest import mock

        from cli import cmd_tidy

        with tempfile.TemporaryDirectory(prefix="liburing-tidy-") as temporary:
            repo = Path(temporary) / "repo"
            external = Path(temporary) / "external"
            build = repo / "build/clang-glibc-release"
            (repo / "tests/unit").mkdir(parents=True)
            (repo / "tests/compile_fail").mkdir(parents=True)
            external.mkdir(parents=True)
            build.mkdir(parents=True)

            good_cpp = repo / "tests/unit/smoke_test.cc"
            good_cpp.write_text("int main() { return 0; }\n")
            compile_fail_cpp = repo / "tests/compile_fail/bad.cc"
            compile_fail_cpp.write_text("int main() { return 0; }\n")
            external_cpp = external / "cpuid.cc"
            external_cpp.write_text("int main() { return 0; }\n")
            build_c = build / "mixed_consumer.c"
            build_c.write_text("int main(void) { return 0; }\n")

            db = [
                {"directory": str(build), "file": str(p), "arguments": ["clang++", "-freflection-latest", "-c", str(p)]}
                for p in (good_cpp, compile_fail_cpp, external_cpp, build_c)
            ]
            (build / "compile_commands.json").write_text(json.dumps(db))

            captured_commands = []

            async def fake_check_all(commands, jobs, out, *, p2996_capable=True):
                captured_commands.extend(commands)

            out = SimpleNamespace(fmt="json", emit=lambda *a, **k: None, console=SimpleNamespace(print=lambda *a, **k: None))
            with (
                mock.patch.object(cmd_tidy, "PROJECT_ROOT", repo),
                mock.patch.object(cmd_tidy, "find_clang_tool", return_value="/usr/bin/clang-tidy-23"),
                mock.patch.object(cmd_tidy, "_verify_tidy_config", return_value=None),
                mock.patch.object(cmd_tidy, "_supports_p2996_reflection", return_value=False),
                mock.patch.object(cmd_tidy, "ensure_configured", return_value=build),
                mock.patch.object(cmd_tidy, "_check_all", side_effect=fake_check_all),
            ):
                cmd_tidy.tidy_cmd(
                    SimpleNamespace(obj=out),
                    paths=None,
                    debug=False,
                    dev=False,
                    compiler="clang",
                    sanitizer="asan-ubsan",
                    preset="clang-glibc-release",
                    jobs=1,
                    verbose=False,
                )

            self.assertEqual(len(captured_commands), 1)
            self.assertEqual(captured_commands[0][1], str(good_cpp.resolve()))
            self.assertIn("--extra-arg=-D__is_consteval_only(...)=false", captured_commands[0])

            sample_output = (
                "/root/liburing-cpp/tests/unit/smoke_test.cc:3:10: error: 'meta' file not found [clang-diagnostic-error]\n"
                "/root/liburing-cpp/tests/unit/smoke_test.cc:12:8: warning: invalid case style for struct 'bad_name' [readability-identifier-naming]\n"
            )
            findings = cmd_tidy._extract_non_clang_diagnostics(sample_output)
            self.assertEqual(len(findings), 1)
            self.assertIn("[readability-identifier-naming]", findings[0])

    def test_tidy_fatal_errors_always_fail(self):
        import asyncio
        from types import SimpleNamespace
        from cli import cmd_tidy

        diagnostic = "fixture.cc:1:10: fatal error: 'missing.h' file not found [clang-diagnostic-error]\n"
        for capable in (False, True):
            for status in (0, 1):
                with self.subTest(capable=capable, status=status):
                    events = []
                    out = SimpleNamespace(fmt="json", emit=lambda *a, **k: events.append((a, k)))
                    command = [sys.executable, "-c", f"import sys; sys.stderr.write({diagnostic!r}); sys.exit({status})"]
                    with self.assertRaisesRegex(RuntimeError, "clang-tidy checks failed"):
                        asyncio.run(cmd_tidy._check_all([command], 1, out, p2996_capable=capable))
                    self.assertEqual(events[0][1]["exit_code"], 1)
                    emitted = events[0][1]["stdout"] + events[0][1]["stderr"]
                    self.assertIn("fatal error:", emitted)


if __name__ == "__main__":
    unittest.main()
