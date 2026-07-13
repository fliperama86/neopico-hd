#!/usr/bin/env python3
"""Static audit helper for NeoPico-HD RP2350 firmware ELFs.

The goal is to catch binary/layout hazards before a flash-and-watch test:

* critical HSTX/video functions accidentally placed in XIP flash
* scratch SRAM overflows
* critical SRAM functions branching/calling back into XIP flash
* Core 1 background/audio functions that are flash-resident in a timing build
* section and symbol-address drift against a known-good baseline

This is intentionally conservative. It can reject obviously risky binaries, but
it cannot prove that a firmware is stable on a sink.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


FLASH_BASE = 0x10000000
FLASH_END = 0x14000000
SRAM_BASE = 0x20000000
SRAM_END = 0x20082000
SCRATCH_X_BASE = 0x20080000
SCRATCH_X_END = 0x20081000
SCRATCH_Y_BASE = 0x20081000
SCRATCH_Y_END = 0x20082000
SCRATCH_SIZE = 4096

DEFAULT_CRITICAL_SYMBOLS = (
    "dma_irq_handler",
    "build_line_with_di",
    "hstx_di_queue_get_audio_packet",
)

DEFAULT_BACKGROUND_SYMBOLS = (
    "audio_subsystem_background_task",
    "audio_background_task",
    "audio_output_callback",
    "audio_pipeline_process",
    "menu_diag_experiment_tick_background",
)

SECTIONS_TO_SHOW = (
    ".text",
    ".rodata",
    ".data",
    ".bss",
    ".scratch_x",
    ".scratch_y",
)

FLAGS_TO_SHOW = (
    "NEOPICO_CAPTURE_TARGET",
    "NEOPICO_AUDIO_MODE",
    "NEOPICO_ENABLE_OSD",
    "NEOPICO_OSD_FAKE_BLEND",
    "NEOPICO_OSD_CONTROLLER_INPUTS",
    "NEOPICO_VIDEO_240P",
    "NEOPICO_VIDEO_720P",
    "NEOPICO_USE_NONRT_HDMI",
    "NEOPICO_COPY_TO_RAM",
    "NEOPICO_VIDEO_DVI_ONLY",
    "NEOPICO_RESOLUTION_MENU",
    "NEOPICO_RESOLUTION_MENU_720P",
    "NEOPICO_FIRST_BOOT_REBOOT",
    "NEOPICO_SETTINGS_FLASH",
    "NEOPICO_EXP_PRECOMPOSED_HDMI",
    "NEOPICO_EXP_GENLOCK_DYNAMIC",
    "NEOPICO_DIAG_COUNTERS",
    "NEOPICO_DIAG_AUDIO_OSD",
)


@dataclass(frozen=True)
class Section:
    name: str
    size: int
    addr: int


@dataclass(frozen=True)
class Symbol:
    name: str
    addr: int
    size: int | None
    kind: str


@dataclass
class Finding:
    severity: str
    text: str


@dataclass
class Report:
    elf: Path
    sections: dict[str, Section]
    symbols: dict[str, Symbol]
    disasm: dict[str, list[str]]
    flags: dict[str, str]
    findings: list[Finding]


def die(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def run(cmd: list[str]) -> str:
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT)
    except FileNotFoundError:
        die(f"required tool not found: {cmd[0]}")
    except subprocess.CalledProcessError as exc:
        die(f"command failed: {' '.join(cmd)}\n{exc.output}")


def tool(prefix: str, name: str) -> str:
    candidate = f"{prefix}{name}"
    found = shutil.which(candidate)
    if not found:
        die(f"required tool not found in PATH: {candidate}")
    return found


def resolve_elf(path_text: str) -> Path:
    path = Path(path_text).expanduser().resolve()
    if path.is_file():
        return path
    if not path.is_dir():
        die(f"path does not exist: {path}")

    candidates = [
        path / "src" / "neopico_hd.elf",
        path / "neopico_hd.elf",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    all_elves = sorted(path.rglob("*.elf"))
    if len(all_elves) == 1:
        return all_elves[0]
    if not all_elves:
        die(f"no ELF found under {path}")
    die(f"multiple ELFs found under {path}; pass the exact ELF path")


def build_root_for(elf: Path) -> Path | None:
    for parent in (elf.parent, *elf.parents):
        if (parent / "CMakeCache.txt").is_file():
            return parent
    return None


def parse_flags(elf: Path) -> dict[str, str]:
    root = build_root_for(elf)
    if root is None:
        return {}
    cache = root / "CMakeCache.txt"
    flags: dict[str, str] = {}
    for line in cache.read_text(errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        left, value = line.split("=", 1)
        key = left.split(":", 1)[0]
        if key.startswith("NEOPICO_") or key.startswith("PICO_HDMI_"):
            flags[key] = value
    return flags


def parse_sections(size_text: str) -> dict[str, Section]:
    sections: dict[str, Section] = {}
    for line in size_text.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        name = parts[0]
        try:
            size = int(parts[1], 0)
            addr = int(parts[2], 0)
        except ValueError:
            continue
        sections[name] = Section(name=name, size=size, addr=addr)
    return sections


def is_hex(text: str) -> bool:
    try:
        int(text, 16)
        return True
    except ValueError:
        return False


def parse_symbols(nm_text: str) -> dict[str, Symbol]:
    symbols: dict[str, Symbol] = {}
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) < 3 or not is_hex(parts[0]):
            continue
        addr = int(parts[0], 16)
        size: int | None = None
        kind_index = 1
        if len(parts) >= 4 and is_hex(parts[1]) and len(parts[2]) == 1:
            size = int(parts[1], 16)
            kind_index = 2
        if len(parts[kind_index]) != 1:
            continue
        kind = parts[kind_index]
        name = parts[kind_index + 1]
        symbols.setdefault(name, Symbol(name=name, addr=addr, size=size, kind=kind))
    return symbols


def parse_disasm(objdump_text: str) -> dict[str, list[str]]:
    functions: dict[str, list[str]] = {}
    current: str | None = None
    header_re = re.compile(r"^([0-9a-fA-F]+) <(.+)>:$")
    for line in objdump_text.splitlines():
        match = header_re.match(line.strip())
        if match:
            current = match.group(2)
            functions[current] = []
            continue
        if current is not None:
            functions[current].append(line)
    return functions


def region(addr: int) -> str:
    if SCRATCH_X_BASE <= addr < SCRATCH_X_END:
        return "scratch_x"
    if SCRATCH_Y_BASE <= addr < SCRATCH_Y_END:
        return "scratch_y"
    if SRAM_BASE <= addr < SRAM_END:
        return "sram"
    if FLASH_BASE <= addr < FLASH_END:
        return "flash"
    return "other"


def in_sram(addr: int) -> bool:
    return SRAM_BASE <= addr < SRAM_END


def in_flash(addr: int) -> bool:
    return FLASH_BASE <= addr < FLASH_END


def symbol_line(symbol: Symbol) -> str:
    size = "?" if symbol.size is None else str(symbol.size)
    return f"{symbol.name}: 0x{symbol.addr:08x} {region(symbol.addr)} size={size} type={symbol.kind}"


def branch_target(line: str) -> tuple[str, int, str] | None:
    if "\t" in line:
        fields = line.split("\t")
        if len(fields) < 3:
            return None
        instruction = fields[2].strip()
    else:
        instruction = line.strip()
    if not instruction:
        return None

    parts = instruction.split(None, 1)
    if not parts:
        return None
    mnemonic = parts[0]
    operands = parts[1] if len(parts) > 1 else ""

    target_match = re.search(r"\b([0-9a-fA-F]{8})\s+<([^>]+)>", operands)
    if not target_match:
        return None
    return mnemonic, int(target_match.group(1), 16), target_match.group(2)


def is_branch_or_call(mnemonic: str) -> bool:
    return mnemonic == "bl" or mnemonic == "blx" or mnemonic.startswith("b")


def literal_flash_refs(lines: Iterable[str]) -> list[str]:
    refs: list[str] = []
    for line in lines:
        if "\t" in line:
            fields = line.split("\t", 2)
            text = fields[2] if len(fields) > 2 else ""
        else:
            text = line.split(":", 1)[-1]
        if re.search(r"\b0x1[0-3][0-9a-fA-F]{6}\b", text) or re.search(r"\b1[0-3][0-9a-fA-F]{6}\b", text):
            refs.append(line.strip())
    return refs


def audit_report(
    elf: Path,
    prefix: str,
    critical_symbols: tuple[str, ...],
    background_symbols: tuple[str, ...],
) -> Report:
    size_tool = tool(prefix, "size")
    nm_tool = tool(prefix, "nm")
    objdump_tool = tool(prefix, "objdump")

    sections = parse_sections(run([size_tool, "-A", str(elf)]))
    symbols = parse_symbols(run([nm_tool, "-n", "-S", str(elf)]))
    disasm = parse_disasm(run([objdump_tool, "-d", str(elf)]))
    flags = parse_flags(elf)
    findings: list[Finding] = []

    for section_name in (".scratch_x", ".scratch_y"):
        section = sections.get(section_name)
        if section and section.size > SCRATCH_SIZE:
            findings.append(Finding("FAIL", f"{section_name} is {section.size} bytes, exceeds {SCRATCH_SIZE} bytes"))

    for name in critical_symbols:
        symbol = symbols.get(name)
        if symbol is None:
            findings.append(Finding("WARN", f"critical symbol missing: {name}"))
            continue
        if not in_sram(symbol.addr):
            findings.append(Finding("FAIL", f"critical symbol is not in SRAM: {symbol_line(symbol)}"))
            continue

        lines = disasm.get(name, [])
        if not lines:
            findings.append(Finding("WARN", f"no disassembly found for critical symbol: {name}"))
            continue

        for line in lines:
            target = branch_target(line)
            if target is None:
                continue
            mnemonic, addr, target_name = target
            if is_branch_or_call(mnemonic) and in_flash(addr):
                findings.append(
                    Finding(
                        "FAIL",
                        f"{name} has {mnemonic} to flash target 0x{addr:08x} <{target_name}>",
                    )
                )
            elif mnemonic == "blx" and "<" not in line:
                findings.append(Finding("WARN", f"{name} has indirect call/branch: {line.strip()}"))

        refs = literal_flash_refs(lines)
        for ref in refs[:4]:
            findings.append(Finding("WARN", f"{name} contains possible flash literal/reference: {ref}"))
        if len(refs) > 4:
            findings.append(Finding("WARN", f"{name} has {len(refs) - 4} more possible flash references"))

    for name in background_symbols:
        symbol = symbols.get(name)
        if symbol is None:
            continue
        if in_flash(symbol.addr):
            findings.append(Finding("WARN", f"background/Core1 symbol is flash-resident: {symbol_line(symbol)}"))

    return Report(elf=elf, sections=sections, symbols=symbols, disasm=disasm, flags=flags, findings=findings)


def print_sections(report: Report, baseline: Report | None) -> None:
    print("Sections:")
    for name in SECTIONS_TO_SHOW:
        section = report.sections.get(name)
        if not section:
            continue
        delta = ""
        if baseline and name in baseline.sections:
            d_size = section.size - baseline.sections[name].size
            if d_size:
                delta = f" ({d_size:+d})"
        print(f"  {name:<12} {section.size:>8}{delta:>10} @ 0x{section.addr:08x}")


def print_flags(report: Report) -> None:
    shown = [(key, report.flags[key]) for key in FLAGS_TO_SHOW if key in report.flags]
    if not shown:
        return
    print("Build Flags:")
    for key, value in shown:
        print(f"  {key}={value}")


def print_symbols(title: str, report: Report, names: Iterable[str], baseline: Report | None) -> None:
    print(f"{title}:")
    any_symbol = False
    for name in names:
        symbol = report.symbols.get(name)
        if symbol is None:
            continue
        any_symbol = True
        delta = ""
        if baseline and name in baseline.symbols:
            d_addr = symbol.addr - baseline.symbols[name].addr
            if d_addr:
                delta = f" ({d_addr:+#x})"
        print(f"  {symbol_line(symbol)}{delta}")
    if not any_symbol:
        print("  none found")


def print_findings(report: Report) -> None:
    print("Findings:")
    if not report.findings:
        print("  OK no findings")
        return
    order = {"FAIL": 0, "WARN": 1, "INFO": 2}
    for finding in sorted(report.findings, key=lambda f: (order.get(f.severity, 9), f.text)):
        print(f"  {finding.severity}: {finding.text}")


def parse_symbol_arg(values: list[str] | None, defaults: tuple[str, ...]) -> tuple[str, ...]:
    if not values:
        return defaults
    result: list[str] = list(defaults)
    for value in values:
        for item in value.split(","):
            item = item.strip()
            if item and item not in result:
                result.append(item)
    return tuple(result)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Audit a NeoPico-HD RP2350 firmware ELF for timing-sensitive layout hazards."
    )
    parser.add_argument("elf_or_build_dir", help="ELF path or build directory containing src/neopico_hd.elf")
    parser.add_argument("--baseline", help="Optional baseline ELF/build directory to compare section and symbol drift")
    parser.add_argument("--tool-prefix", default="arm-none-eabi-", help="Tool prefix, default: arm-none-eabi-")
    parser.add_argument(
        "--critical",
        action="append",
        help="Additional comma-separated critical symbol names expected in SRAM",
    )
    parser.add_argument(
        "--background",
        action="append",
        help="Additional comma-separated Core 1/background symbol names to report if flash-resident",
    )
    parser.add_argument("--no-fail", action="store_true", help="Always exit 0, even when FAIL findings are present")
    args = parser.parse_args(argv)

    elf = resolve_elf(args.elf_or_build_dir)
    baseline_elf = resolve_elf(args.baseline) if args.baseline else None
    critical_list = list(parse_symbol_arg(args.critical, DEFAULT_CRITICAL_SYMBOLS))
    flags = parse_flags(elf)
    if flags.get("NEOPICO_RESOLUTION_MENU_720P") == "ON":
        for symbol in ("rt_build_line_with_di", "build_line_with_di_backporch"):
            if symbol not in critical_list:
                critical_list.append(symbol)
    if flags.get("NEOPICO_EXP_PRECOMPOSED_HDMI") != "ON":
        scanline_symbol = (
            "video_pipeline_scanline_callback_reboot_modes"
            if flags.get("NEOPICO_RESOLUTION_MENU") == "ON"
            and flags.get("NEOPICO_RESOLUTION_MENU_720P") == "ON"
            else "video_pipeline_scanline_callback"
        )
        if scanline_symbol not in critical_list:
            critical_list.append(scanline_symbol)
    critical = tuple(critical_list)
    background = parse_symbol_arg(args.background, DEFAULT_BACKGROUND_SYMBOLS)

    report = audit_report(elf, args.tool_prefix, critical, background)
    baseline = audit_report(baseline_elf, args.tool_prefix, critical, background) if baseline_elf else None

    print(f"Firmware Audit: {report.elf}")
    if baseline:
        print(f"Baseline:       {baseline.elf}")
    print()
    print_flags(report)
    if report.flags:
        print()
    print_sections(report, baseline)
    print()
    print_symbols("Critical Symbols", report, critical, baseline)
    print()
    print_symbols("Background Symbols", report, background, baseline)
    print()
    print_findings(report)

    fail_count = sum(1 for finding in report.findings if finding.severity == "FAIL")
    if fail_count and not args.no_fail:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
