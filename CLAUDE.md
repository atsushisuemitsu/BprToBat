# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BprToBat is a command-line tool that parses Borland C++ Builder 3.0 (BCB3) `.bpr` project files and generates parallel build `.bat` files. It enables multi-core compilation of legacy BCB3 projects by splitting source files across worker processes with CPU affinity.

## Build

Requires VS2022 Build Tools (MSVC). Single source file project.

```bat
# Build using the build script
build.bat

# Or manually
cl /EHsc /O2 /Fe:BprToBat.exe BprToBat.cpp
```

## Usage

```
BprToBat.exe <bprfile> [num_workers]
```

- `bprfile`: Path to a BCB3 `.bpr` file (required)
- `num_workers`: Parallel workers 1-16 (default: 4)
- Output: `{project}_build_parallel.bat` in the same directory as the `.bpr`

## Architecture

Everything is in a single file `BprToBat.cpp` (~1400 lines). The processing pipeline is:

1. **`parseBpr()`** (line ~299) - Reads the `.bpr` file and populates `BprProject` struct. Handles two zones:
   - Makefile zone (before `!ifdef IDEOPTIONS`): parses `VAR = VALUE` assignments with backslash continuation line joining
   - IDE options zone: parses `[Version Info]`, `[Version Info Keys]`, and `[HistoryLists\hlConditionals]` sections

2. **`readRcPreamble()`** (line ~481) - Preserves non-VERSIONINFO lines (e.g., ICON declarations) from existing `.rc` files

3. **`generateBat()`** (line ~654) - Generates the parallel build `.bat` file with these phases:
   - Phase 1: Scans source files and round-robin assigns them to workers via `FIND_AND_ASSIGN` subroutine
   - Phase 2: Launches workers with `start /affinity` for CPU pinning
   - Phase 3: Polls `_doneN.tmp` sentinel files to wait for worker completion
   - Phase 4: (If `IncludeVerInfo=1`) Parses version info from `.bpr` at runtime, generates `.rc`, compiles resources
   - Phase 5: Links using `ilink32` with a generated response file

### Key Data Structures

- **`BprProject`** (line ~29) - Holds all parsed data: Makefile variables (PROJECT, OBJFILES, CFLAG1-3, PFLAGS, RFLAGS, LFLAGS, LIBFILES, LIBRARIES, PATHCPP, ALLOBJ, ALLLIB), version info, and RC preamble lines
- **`AllobjInfo`** (line ~590) - Parsed startup objects and `$(PACKAGES)` presence from ALLOBJ
- **`AllLibInfo`** (line ~618) - System libs extracted from ALLLIB (excluding `$(LIBFILES)`/`$(LIBRARIES)` macros)

### Key Helper Functions

- **`convertFlags()`** / **`convertComponent()`** - Converts BCB3 `$(BCB)` macros to batch `%BCB%` with proper quoting for semicolon-delimited path lists
- **`extractDefines()`** - Separates `-D` flags from CFLAG2 into a dedicated CDEFINES variable
- **`buildSrcDirs()`** - Collects source directories from PATHCPP, PATHPAS, and CFLAG2's `-I` paths (excluding absolute/macro paths)
- **`detectObjDir()`** - Determines the most common obj output directory prefix

### Generated BAT Structure

The generated `.bat` uses Windows batch scripting with `setlocal enabledelayedexpansion`. Worker assignment is fully dynamic via `NUM_WORKERS` variable (can be changed post-generation without re-running BprToBat). Workers are separate `.bat` files (`_worker1.bat` .. `_workerN.bat`) launched as background processes with `start /min`.

## BPR File Format

The `.bpr` format is a BCB3-specific Makefile variant with an `!ifdef IDEOPTIONS` block containing INI-style sections. Key parsed variables: PROJECT, OBJFILES, CFLAG1-3, PFLAGS, RFLAGS, LFLAGS, LIBFILES, LIBRARIES, PATHCPP, PATHPAS, ALLOBJ, ALLLIB, RESFILES, PACKAGES. Continuation lines use trailing backslash (`\`).
