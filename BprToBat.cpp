// BprToBat.cpp - Parses BCB3 .bpr project files and generates parallel build .bat files.
//
// Compiler: MSVC (VS2022)
//   cl /EHsc /O2 /Fe:BprToBat.exe BprToBat.cpp
//
// Usage:
//   BprToBat.exe <bprfile> [num_workers]
//
// bprfile:     path to .bpr file (required)
// num_workers: 1-16, default 4
// Output:      {projectBaseName}_build_parallel.bat in the same directory as .bpr

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct BprProject {
    std::string projectName;       // e.g. "Gats2120.exe"
    std::string projectBaseName;   // e.g. "Gats2120"
    std::vector<std::string> objFiles; // individual OBJ file entries (with dir prefix)
    std::string resFiles, libFiles, libraries, packages;
    std::string pathCpp, pathPas, pathAsm;
    std::string cflag1, cflag2, cflag3, pflags, rflags, lflags, aflags;
    std::string allobj, alllib;
    std::string debugLibPath, releaseLibPath;

    // Version Info
    bool includeVerInfo = false;
    int majorVer = 0, minorVer = 0, release = 0, build = 0;
    int debug = 0, preRelease = 0;
    int locale = 0, codePage = 0;
    std::map<std::string, std::string> versionKeys;

    // hlConditionals Item0 (for CDEFINES hint)
    std::string hlConditionalsItem0;

    // RC preamble: non-VERSIONINFO lines from existing .rc file (e.g. ICON)
    std::vector<std::string> rcPreambleLines;
};

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

static std::string trimWhitespace(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Split a string by a single delimiter character
static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> result;
    std::string token;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim) {
            result.push_back(token);
            token.clear();
        } else {
            token += s[i];
        }
    }
    result.push_back(token);
    return result;
}

// Split a string by whitespace (collapses multiple spaces), respecting double-quoted substrings
static std::vector<std::string> splitBySpace(const std::string& s) {
    std::vector<std::string> result;
    std::string token;
    bool inQuotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') {
            inQuotes = !inQuotes;
            token += s[i];
        } else if (!inQuotes && (s[i] == ' ' || s[i] == '\t')) {
            if (!token.empty()) {
                result.push_back(token);
                token.clear();
            }
        } else {
            token += s[i];
        }
    }
    if (!token.empty()) result.push_back(token);
    return result;
}

// Replace all occurrences of 'from' with 'to' in str
static std::string replaceAll(const std::string& str,
                              const std::string& from,
                              const std::string& to) {
    if (from.empty()) return str;
    std::string result;
    size_t pos = 0;
    while (true) {
        size_t found = str.find(from, pos);
        if (found == std::string::npos) {
            result += str.substr(pos);
            break;
        }
        result += str.substr(pos, found - pos);
        result += to;
        pos = found + from.size();
    }
    return result;
}

// Check if a path starts with a drive letter (absolute path)
static bool isAbsolutePath(const std::string& s) {
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':')
        return true;
    if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\')
        return true;
    return false;
}

// Check if a path component contains a BCB/macro reference
static bool containsMacro(const std::string& s) {
    return s.find("$(") != std::string::npos;
}

// Remove all double-quote characters from a string
static std::string stripQuotes(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c != '"') r += c;
    }
    return r;
}

// Extract directory part from a path (everything before the last backslash)
static std::string getDirPart(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

// Extract filename without extension from a path
static std::string getBaseName(const std::string& path) {
    size_t slashPos = path.find_last_of("\\/");
    std::string name = (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);
    size_t dotPos = name.find_last_of('.');
    if (dotPos != std::string::npos)
        name = name.substr(0, dotPos);
    return name;
}

// Extract filename (with extension) from a path
static std::string getFileName(const std::string& path) {
    size_t slashPos = path.find_last_of("\\/");
    return (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);
}

// ---------------------------------------------------------------------------
// Flag conversion: $(BCB) -> %BCB%, $(RELEASELIBPATH) -> %RELEASELIBPATH%
// with quoting for components that contain macro substitutions.
//
// The conversion handles semicolon-delimited path lists within flag tokens.
// For example:
//   -Ifoo;bar;$(BCB)\include -> -Ifoo;bar;"%BCB%\include"
// ---------------------------------------------------------------------------

// Convert a single semicolon-delimited component.
// If it contains $(BCB) or $(RELEASELIBPATH), replace the macro with
// the corresponding %ENV% variable.  Do NOT wrap in quotes -- bcc32
// does not handle quoted paths inside semicolon-delimited -I/-L lists.
static std::string convertComponent(const std::string& comp) {
    if (comp.find("$(") == std::string::npos) {
        return comp;
    }
    std::string result = comp;
    result = replaceAll(result, "$(BCB)", "%BCB%");
    result = replaceAll(result, "$(RELEASELIBPATH)", "%RELEASELIBPATH%");
    result = replaceAll(result, "$(DEBUGLIBPATH)", "%DEBUGLIBPATH%");
    return result;
}

// Convert a flag value string. The string may contain multiple space-separated
// tokens, some of which are semicolon-delimited path lists (e.g., -Ifoo;bar;$(BCB)\inc).
static std::string convertFlags(const std::string& flags) {
    if (flags.empty()) return flags;

    // We need to be careful: tokens like -$Y are valid flags, not macros.
    // We split by spaces, then for each token check if it contains semicolons.

    std::vector<std::string> tokens = splitBySpace(flags);
    std::string result;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) result += ' ';

        const std::string& token = tokens[i];

        // Check if this token contains semicolons (it's a path list)
        if (token.find(';') != std::string::npos) {
            // Split by semicolons
            std::vector<std::string> components = split(token, ';');
            std::string converted;
            for (size_t j = 0; j < components.size(); ++j) {
                if (j > 0) converted += ';';
                converted += convertComponent(components[j]);
            }
            result += converted;
        } else if (token.find("$(") != std::string::npos) {
            // Single token containing a macro but no semicolons
            result += convertComponent(token);
        } else {
            result += token;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Extract -D flags from CFLAG2 and return them as CDEFINES.
// The remaining CFLAG2 content (without -D flags) is stored back.
// ---------------------------------------------------------------------------

static std::string extractDefines(std::string& cflag2) {
    std::vector<std::string> tokens = splitBySpace(cflag2);
    std::string defines;
    std::string remaining;

    for (size_t i = 0; i < tokens.size(); ++i) {
        // A -D flag starts with -D and may contain semicolons for multiple defines
        if (tokens[i].size() >= 2 && tokens[i][0] == '-' && tokens[i][1] == 'D') {
            if (!defines.empty()) defines += ' ';
            defines += tokens[i];
        } else {
            if (!remaining.empty()) remaining += ' ';
            remaining += tokens[i];
        }
    }

    cflag2 = remaining;
    return defines;
}

// ---------------------------------------------------------------------------
// Parse the BPR file
// ---------------------------------------------------------------------------

// Read the entire file in binary mode (to preserve Shift-JIS bytes)
static std::string readFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Cannot open file: " << path << std::endl;
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
}

// Split raw file content into lines (handle \r\n and \n)
static std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            lines.push_back(line);
            line.clear();
            if (i + 1 < content.size() && content[i + 1] == '\n')
                ++i;
        } else if (content[i] == '\n') {
            lines.push_back(line);
            line.clear();
        } else {
            line += content[i];
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Check if a line ends with backslash continuation (space + backslash at end)
static bool hasContinuation(const std::string& line) {
    std::string trimmed = trimWhitespace(line);
    if (trimmed.size() >= 2 && trimmed[trimmed.size() - 1] == '\\' &&
        trimmed[trimmed.size() - 2] == ' ') {
        return true;
    }
    // Also handle case where only a backslash (no space before it but common pattern)
    if (!trimmed.empty() && trimmed.back() == '\\') {
        return true;
    }
    return false;
}

// Strip the trailing continuation marker from a line
static std::string stripContinuation(const std::string& line) {
    std::string trimmed = trimWhitespace(line);
    if (!trimmed.empty() && trimmed.back() == '\\') {
        trimmed.pop_back();
    }
    return trimWhitespace(trimmed);
}

static bool parseBpr(const std::string& bprPath, BprProject& proj) {
    std::string content = readFile(bprPath);
    if (content.empty()) return false;

    std::vector<std::string> lines = splitLines(content);

    enum ParseZone {
        ZONE_MAKEFILE,    // Before !ifdef IDEOPTIONS
        ZONE_VERINFO,     // [Version Info] section
        ZONE_VERKEYS,     // [Version Info Keys] section
        ZONE_HISTORY,     // [HistoryLists\...] sections
        ZONE_OTHER_IDE    // Other IDE sections
    };

    ParseZone zone = ZONE_MAKEFILE;
    std::string currentHistorySection;

    // Build a processed line list:
    // - For the makefile zone (before !ifdef IDEOPTIONS): join continuation lines
    // - For the IDE zone (inside !ifdef IDEOPTIONS): keep lines as-is
    std::vector<std::string> joinedLines;
    {
        bool reachedIdeoptions = false;
        std::string accum;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& rawLine = lines[i];

            if (rawLine.find("!ifdef IDEOPTIONS") != std::string::npos) {
                // Flush any accumulated makefile line
                if (!accum.empty()) {
                    joinedLines.push_back(accum);
                    accum.clear();
                }
                reachedIdeoptions = true;
                joinedLines.push_back(rawLine);
                continue;
            }

            if (!reachedIdeoptions) {
                // Makefile zone: handle backslash continuation
                if (hasContinuation(rawLine)) {
                    accum += stripContinuation(rawLine) + " ";
                } else {
                    accum += trimWhitespace(rawLine);
                    joinedLines.push_back(accum);
                    accum.clear();
                }
            } else {
                // IDE zone: pass lines through unchanged
                joinedLines.push_back(rawLine);
            }
        }
        if (!accum.empty()) {
            joinedLines.push_back(accum);
        }
    }

    // Parse the processed lines
    bool inIdeoptions = false;
    for (size_t i = 0; i < joinedLines.size(); ++i) {
        const std::string& line = joinedLines[i];
        std::string trimmed = trimWhitespace(line);

        if (trimmed.empty()) continue;

        // Detect zone transitions
        if (trimmed.find("!ifdef IDEOPTIONS") != std::string::npos) {
            inIdeoptions = true;
            zone = ZONE_OTHER_IDE;
            continue;
        }
        if (trimmed.find("!endif") != std::string::npos) {
            inIdeoptions = false;
            zone = ZONE_MAKEFILE;
            continue;
        }

        // Section headers (within IDEOPTIONS)
        if (inIdeoptions && !trimmed.empty() && trimmed[0] == '[') {
            size_t close = trimmed.find(']');
            if (close != std::string::npos) {
                std::string section = trimmed.substr(1, close - 1);
                if (section == "Version Info") {
                    zone = ZONE_VERINFO;
                } else if (section == "Version Info Keys") {
                    zone = ZONE_VERKEYS;
                } else if (section.find("HistoryLists\\") == 0) {
                    zone = ZONE_HISTORY;
                    currentHistorySection = section;
                } else {
                    zone = ZONE_OTHER_IDE;
                }
                continue;
            }
        }

        // Parse based on zone
        if (!inIdeoptions) {
            // Makefile variable zone: parse VAR = VALUE lines
            // Ignore comments (lines starting with #) and directives
            if (trimmed[0] == '#' || trimmed[0] == '!' || trimmed[0] == '.') continue;

            size_t eqPos = trimmed.find('=');
            if (eqPos == std::string::npos) continue;

            std::string varName = trimWhitespace(trimmed.substr(0, eqPos));
            std::string varValue = trimWhitespace(trimmed.substr(eqPos + 1));

            if (varName == "PROJECT") proj.projectName = varValue;
            else if (varName == "OBJFILES") {
                // Parse individual obj file entries (space-separated)
                std::vector<std::string> objs = splitBySpace(varValue);
                for (const auto& obj : objs) {
                    proj.objFiles.push_back(obj);
                }
            }
            else if (varName == "RESFILES") proj.resFiles = varValue;
            else if (varName == "LIBFILES") proj.libFiles = varValue;
            else if (varName == "LIBRARIES") proj.libraries = varValue;
            else if (varName == "PACKAGES") proj.packages = varValue;
            else if (varName == "PATHCPP") proj.pathCpp = varValue;
            else if (varName == "PATHPAS") proj.pathPas = varValue;
            else if (varName == "PATHASM") proj.pathAsm = varValue;
            else if (varName == "AFLAGS") proj.aflags = varValue;
            else if (varName == "CFLAG1") proj.cflag1 = varValue;
            else if (varName == "CFLAG2") proj.cflag2 = varValue;
            else if (varName == "CFLAG3") proj.cflag3 = varValue;
            else if (varName == "PFLAGS") proj.pflags = varValue;
            else if (varName == "RFLAGS") proj.rflags = varValue;
            else if (varName == "LFLAGS") proj.lflags = varValue;
            else if (varName == "ALLOBJ") proj.allobj = varValue;
            else if (varName == "ALLLIB") proj.alllib = varValue;
            else if (varName == "DEBUGLIBPATH") proj.debugLibPath = varValue;
            else if (varName == "RELEASELIBPATH") proj.releaseLibPath = varValue;
        }
        else if (zone == ZONE_VERINFO) {
            size_t eqPos = trimmed.find('=');
            if (eqPos == std::string::npos) continue;
            std::string key = trimWhitespace(trimmed.substr(0, eqPos));
            std::string val = trimWhitespace(trimmed.substr(eqPos + 1));

            if (key == "IncludeVerInfo") proj.includeVerInfo = (val == "1");
            else if (key == "MajorVer") proj.majorVer = std::atoi(val.c_str());
            else if (key == "MinorVer") proj.minorVer = std::atoi(val.c_str());
            else if (key == "Release") proj.release = std::atoi(val.c_str());
            else if (key == "Build") proj.build = std::atoi(val.c_str());
            else if (key == "Debug") proj.debug = std::atoi(val.c_str());
            else if (key == "PreRelease") proj.preRelease = std::atoi(val.c_str());
            else if (key == "Locale") proj.locale = std::atoi(val.c_str());
            else if (key == "CodePage") proj.codePage = std::atoi(val.c_str());
        }
        else if (zone == ZONE_VERKEYS) {
            size_t eqPos = trimmed.find('=');
            if (eqPos == std::string::npos) continue;
            std::string key = trimWhitespace(trimmed.substr(0, eqPos));
            std::string val = trimWhitespace(trimmed.substr(eqPos + 1));
            proj.versionKeys[key] = val;
        }
        else if (zone == ZONE_HISTORY) {
            // Parse [HistoryLists\hlConditionals] Item0
            if (currentHistorySection == "HistoryLists\\hlConditionals") {
                size_t eqPos = trimmed.find('=');
                if (eqPos == std::string::npos) continue;
                std::string key = trimWhitespace(trimmed.substr(0, eqPos));
                std::string val = trimWhitespace(trimmed.substr(eqPos + 1));
                if (key == "Item0") {
                    proj.hlConditionalsItem0 = val;
                }
            }
        }
    }

    // Derive projectBaseName from projectName
    if (!proj.projectName.empty()) {
        proj.projectBaseName = getBaseName(proj.projectName);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Read existing .rc file preamble (lines before VERSIONINFO block)
// ---------------------------------------------------------------------------

static void readRcPreamble(const std::string& bprDir, BprProject& proj) {
    std::string rcPath = bprDir + "\\" + proj.projectBaseName + ".rc";
    // Silently skip if .rc file doesn't exist
    {
        std::ifstream test(rcPath);
        if (!test.is_open()) return;
    }
    std::string content = readFile(rcPath);
    if (content.empty()) return;

    std::vector<std::string> lines = splitLines(content);
    for (const auto& line : lines) {
        std::string trimmed = trimWhitespace(line);
        // Stop when we hit VERSIONINFO block
        if (trimmed.find("VERSIONINFO") != std::string::npos) break;
        // Skip empty lines just before VERSIONINFO
        // But keep ICON lines, STRINGTABLE, etc.
        if (!trimmed.empty()) {
            proj.rcPreambleLines.push_back(line);
        }
    }
}

// ---------------------------------------------------------------------------
// Determine the obj output directory
// ---------------------------------------------------------------------------

static std::string detectObjDir(const std::vector<std::string>& objFiles) {
    if (objFiles.empty()) return ".";

    // Count how many obj files have a directory prefix
    std::map<std::string, int> dirCounts;
    for (const auto& obj : objFiles) {
        std::string dir = getDirPart(obj);
        if (dir.empty()) dir = ".";
        dirCounts[dir]++;
    }

    // Find the most common directory prefix
    std::string bestDir = ".";
    int bestCount = 0;
    for (const auto& kv : dirCounts) {
        if (kv.second > bestCount) {
            bestCount = kv.second;
            bestDir = kv.first;
        }
    }

    return bestDir;
}

// ---------------------------------------------------------------------------
// Build SRCDIRS list
// ---------------------------------------------------------------------------

static std::vector<std::string> buildSrcDirs(const BprProject& proj) {
    // Collect directories from:
    // 1. PATHCPP (semicolon-separated)
    // 2. PATHPAS (semicolon-separated)
    // 3. CFLAG2's -I paths (local directories only)

    std::vector<std::string> dirs;
    std::set<std::string> seen; // case-insensitive dedup

    auto addDir = [&](const std::string& d) {
        std::string trimmed = trimWhitespace(d);
        if (trimmed.empty()) return;
        std::string key = toLower(trimmed);
        if (seen.find(key) == seen.end()) {
            seen.insert(key);
            dirs.push_back(trimmed);
        }
    };

    // PATHCPP
    for (const auto& d : split(proj.pathCpp, ';')) {
        addDir(d);
    }

    // PATHPAS
    for (const auto& d : split(proj.pathPas, ';')) {
        addDir(d);
    }

    // PATHASM
    for (const auto& d : split(proj.pathAsm, ';')) {
        addDir(d);
    }

    // Extract -I paths from CFLAG2
    std::vector<std::string> tokens = splitBySpace(proj.cflag2);
    for (const auto& token : tokens) {
        // Look for -I flag followed by semicolon-separated paths
        if (token.size() > 2 && token[0] == '-' && (token[1] == 'I' || token[1] == 'i')) {
            std::string pathList = token.substr(2);
            for (const auto& p : split(pathList, ';')) {
                std::string trimP = trimWhitespace(p);
                if (trimP.empty()) continue;
                // Strip quotes for reliable path checks
                std::string unquoted = stripQuotes(trimP);
                if (unquoted.empty()) continue;
                // Skip paths containing macros or absolute paths
                if (containsMacro(unquoted)) continue;
                if (isAbsolutePath(unquoted)) continue;
                if (unquoted.find("..") == 0) continue; // skip parent-relative paths
                addDir(unquoted);
            }
        }
    }

    return dirs;
}

// ---------------------------------------------------------------------------
// Parse ALLOBJ: extract startup objects and detect $(PACKAGES) position
// ---------------------------------------------------------------------------

struct AllobjInfo {
    std::vector<std::string> startupObjs;  // e.g. c0w32.obj, sysinit.obj
    bool hasPackages = false;              // $(PACKAGES) present in ALLOBJ
};

static AllobjInfo parseAllobj(const std::string& allobj) {
    AllobjInfo info;
    std::vector<std::string> tokens = splitBySpace(allobj);

    for (const auto& tok : tokens) {
        if (tok == "$(OBJFILES)") {
            // Everything before this is startup/packages
            continue;
        }
        if (tok == "$(PACKAGES)") {
            info.hasPackages = true;
            continue;
        }
        info.startupObjs.push_back(tok);
    }

    return info;
}

// ---------------------------------------------------------------------------
// Parse ALLLIB: extract system libs (tokens that aren't $(LIBFILES), $(LIBRARIES))
// ---------------------------------------------------------------------------

struct AllLibInfo {
    std::vector<std::string> systemLibs; // e.g. import32.lib cp32mt.lib
};

static AllLibInfo parseAlllib(const std::string& alllib) {
    AllLibInfo info;
    std::vector<std::string> tokens = splitBySpace(alllib);

    for (const auto& tok : tokens) {
        if (tok == "$(LIBFILES)" || tok == "$(LIBRARIES)" || tok == "$(PACKAGES)") {
            continue;
        }
        info.systemLibs.push_back(tok);
    }

    return info;
}

// ---------------------------------------------------------------------------
// Generate the .bat file
// ---------------------------------------------------------------------------

// Helper: write a line with \r\n
static void writeLine(std::ofstream& ofs, const std::string& line = "") {
    ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
    ofs.write("\r\n", 2);
}

// Convert an integer to hex string (uppercase, at least 1 digit)
static std::string toHex(int value) {
    if (value == 0) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%X", static_cast<unsigned int>(value));
    return buf;
}

static bool generateBat(const std::string& bprPath,
                         const BprProject& proj,
                         int numWorkers) {
    // Determine output path
    std::string bprDir = getDirPart(bprPath);
    if (bprDir.empty()) bprDir = ".";
    std::string bprFileName = getFileName(bprPath);
    std::string batPath = bprDir + "\\" + proj.projectBaseName + "_build_parallel.bat";

    // Detect obj directory
    std::string objDir = detectObjDir(proj.objFiles);

    // Build SRCDIRS
    std::vector<std::string> srcDirs = buildSrcDirs(proj);

    // Convert flags
    std::string cflag1_conv = convertFlags(proj.cflag1);
    std::string cflag2_conv = convertFlags(proj.cflag2);
    std::string cflag3_conv = proj.cflag3; // typically no macros
    std::string pflags_conv = convertFlags(proj.pflags);
    std::string rflags_conv = convertFlags(proj.rflags);
    std::string lflags_conv = convertFlags(proj.lflags);
    std::string aflags_conv = convertFlags(proj.aflags);

    // Extract -D defines from CFLAG2
    std::string cdefines = extractDefines(cflag2_conv);

    // Parse ALLOBJ and ALLLIB
    AllobjInfo allobjInfo = parseAllobj(proj.allobj);
    AllLibInfo allLibInfo = parseAlllib(proj.alllib);

    // Build LIBFILES and LIBRARIES as space-separated strings (they may have
    // backslash continuations joined, but we already joined them)
    std::string libFilesStr = trimWhitespace(proj.libFiles);
    std::string librariesStr = trimWhitespace(proj.libraries);

    // RESFILES
    std::string resFilesStr = trimWhitespace(proj.resFiles);

    // Build the SRCDIRS string
    std::string srcDirsStr;
    for (size_t i = 0; i < srcDirs.size(); ++i) {
        if (i > 0) srcDirsStr += ' ';
        srcDirsStr += srcDirs[i];
    }

    // Project base name in lower case for response file name
    std::string projBaseNameLower = toLower(proj.projectBaseName);

    // Open output file in binary mode for explicit \r\n
    std::ofstream ofs(batPath, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "Error: Cannot create output file: " << batPath << std::endl;
        return false;
    }

    // ----------------------------------------------------------------
    // Header
    // ----------------------------------------------------------------
    writeLine(ofs, "@echo off");
    writeLine(ofs, "chcp 932 >nul");
    writeLine(ofs, "setlocal enabledelayedexpansion");
    writeLine(ofs);
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, "REM  " + proj.projectBaseName + " Parallel Compile & Link Batch");
    writeLine(ofs, "REM  Generated from " + bprFileName + " (C++Builder 3.0 project)");
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Settings
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Settings ===");
    writeLine(ofs, "REM Set the C++Builder 3.0 installation path if not set");
    writeLine(ofs, "if not \"%BCB%\"==\"\" goto BCB_SET");
    writeLine(ofs, "if exist \"C:\\PROGRA~2\\Borland\\CBUILD~1\\BIN\\bcc32.exe\" set \"BCB=C:\\PROGRA~2\\Borland\\CBUILD~1\"");
    writeLine(ofs, "if \"%BCB%\"==\"\" set \"BCB=C:\\Program Files\\Borland\\CBuilder3\"");
    writeLine(ofs, ":BCB_SET");
    writeLine(ofs);

    // Number of workers
    char numWorkersStr[8];
    std::snprintf(numWorkersStr, sizeof(numWorkersStr), "%d", numWorkers);
    writeLine(ofs, "REM Number of workers (adjust to your CPU cores, 1-16).");
    writeLine(ofs, std::string("set NUM_WORKERS=") + numWorkersStr);
    writeLine(ofs);

    // Project directory
    writeLine(ofs, "REM Project directory (directory where this .bat is located)");
    writeLine(ofs, "set PROJDIR=%~dp0");
    writeLine(ofs, "cd /d \"%PROJDIR%\"");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Compile options
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Compile options (from .bpr) ===");
    writeLine(ofs, "set CFLAG1=" + cflag1_conv);
    writeLine(ofs, "set CFLAG2=" + cflag2_conv);
    writeLine(ofs, "set CFLAG3=" + cflag3_conv);
    writeLine(ofs);

    // CDEFINES
    // If CFLAG2 had -D flags, use those.
    // Otherwise, use hlConditionals Item0 (the active IDE conditional defines).
    writeLine(ofs, "REM Preprocessor defines");
    if (cdefines.empty() && !proj.hlConditionalsItem0.empty()) {
        // hlConditionals Item0 may contain semicolon-separated defines
        // e.g. "_RTLDLL;USEPACKAGES" -> "-D_RTLDLL;USEPACKAGES"
        writeLine(ofs, "set \"CDEFINES=-D" + proj.hlConditionalsItem0 + "\"");
    } else {
        writeLine(ofs, "set \"CDEFINES=" + cdefines + "\"");
    }
    writeLine(ofs);

    // Output directory
    writeLine(ofs, "REM Output directory");
    writeLine(ofs, "set \"OBJDIR=" + objDir + "\"");
    writeLine(ofs, "if not exist \"%OBJDIR%\" mkdir \"%OBJDIR%\"");
    writeLine(ofs);

    // Pascal flags
    writeLine(ofs, "REM Delphi/Pascal compile flags (from .bpr)");
    writeLine(ofs, "set PFLAGS=" + pflags_conv);
    writeLine(ofs);

    // Assembler flags
    if (!aflags_conv.empty()) {
        writeLine(ofs, "REM Assembler flags (from .bpr)");
        writeLine(ofs, "set AFLAGS=" + aflags_conv);
        writeLine(ofs);
    }

    // Resource flags
    writeLine(ofs, "REM Resource compile flags (from .bpr)");
    writeLine(ofs, "set RFLAGS=" + rflags_conv);
    writeLine(ofs);

    // Linker flags
    writeLine(ofs, "REM Linker library paths (from .bpr)");
    writeLine(ofs, "set LFLAGS=" + lflags_conv);
    writeLine(ofs);

    // Libraries
    writeLine(ofs, "REM Library lists (from .bpr)");
    writeLine(ofs, "set \"LIBFILES=" + libFilesStr + "\"");
    writeLine(ofs, "set \"LIBRARIES=" + librariesStr + "\"");
    writeLine(ofs);

    // Res files
    writeLine(ofs, "REM Res files (from .bpr)");
    writeLine(ofs, "set \"RESFILES=" + resFilesStr + "\"");
    writeLine(ofs);

    // Common lib paths (use values from BPR, fall back to BCB defaults)
    writeLine(ofs, "REM Common lib paths");
    {
        std::string rlp = proj.releaseLibPath.empty()
            ? "$(BCB)\\lib\\release" : proj.releaseLibPath;
        std::string dlp = proj.debugLibPath.empty()
            ? "$(BCB)\\lib\\debug" : proj.debugLibPath;
        rlp = convertFlags(rlp);
        dlp = convertFlags(dlp);
        writeLine(ofs, "set \"RELEASELIBPATH=" + rlp + "\"");
        writeLine(ofs, "set \"DEBUGLIBPATH=" + dlp + "\"");
    }
    writeLine(ofs);

    // Source directories
    writeLine(ofs, "REM Source search directories");
    writeLine(ofs, "set SRCDIRS=" + srcDirsStr);
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Cleanup
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Cleanup: remove old worker files ===");
    writeLine(ofs, "for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "    if exist \"_worker%%i.bat\" del \"_worker%%i.bat\"");
    writeLine(ofs, "    if exist \"_done%%i.tmp\" del \"_done%%i.tmp\"");
    writeLine(ofs, ")");
    writeLine(ofs, "if exist \"_compile_errors.log\" del \"_compile_errors.log\"");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Timestamp
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Timestamp ===");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs, "echo  " + proj.projectBaseName + " Parallel Build Start (Workers: %NUM_WORKERS%)");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs, "set START_TIME=%time%");
    writeLine(ofs, "echo Start time: %START_TIME%");
    writeLine(ofs, "echo.");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Phase 1: Scan source files & assign to workers
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Phase 1: Scan source files & assign to workers ===");
    writeLine(ofs, "echo Scanning source files...");
    writeLine(ofs, "set FILE_COUNT=0");
    writeLine(ofs);

    // Worker batch headers
    writeLine(ofs, "REM Create worker batch headers");
    writeLine(ofs, "for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "    >\"_worker%%i.bat\" echo @echo off");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo setlocal enabledelayedexpansion");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set BCB=!BCB!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set CFLAG1=!CFLAG1!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set CFLAG2=!CFLAG2!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set CFLAG3=!CFLAG3!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set CDEFINES=!CDEFINES!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set PFLAGS=!PFLAGS!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set AFLAGS=!AFLAGS!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set OBJDIR=!OBJDIR!");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set /a ERRORS=0");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set /a COMPILED=0");
    writeLine(ofs, ")");
    writeLine(ofs);

    // Assign each source file to a worker (dynamic round-robin using NUM_WORKERS)
    writeLine(ofs, "REM Assign each source file to a worker (round-robin based on NUM_WORKERS)");
    writeLine(ofs, "set _WORKER_IDX=0");
    for (size_t i = 0; i < proj.objFiles.size(); ++i) {
        const std::string& objEntry = proj.objFiles[i];
        // Strip the .obj extension to get the base path for lookup
        std::string basePath;
        size_t dotPos = objEntry.find_last_of('.');
        if (dotPos != std::string::npos) {
            basePath = objEntry.substr(0, dotPos);
        } else {
            basePath = objEntry;
        }

        writeLine(ofs, "call :FIND_AND_ASSIGN \"" + basePath + "\"");
    }
    writeLine(ofs);

    writeLine(ofs, "echo %FILE_COUNT% source files assigned to %NUM_WORKERS% workers");
    writeLine(ofs, "echo.");
    writeLine(ofs);

    // Worker batch footers
    writeLine(ofs, "REM Create worker batch footers");
    writeLine(ofs, "for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo echo.");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo echo Worker%%i: compiled %%COMPILED%% files (errors: %%ERRORS%%^)");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo set /a \"ERRORS+=0\"");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo if %%ERRORS%% GTR 0 echo WORKER%%i_ERRORS ^>^> \"_compile_errors.log\"");
    writeLine(ofs, "    >>\"_worker%%i.bat\" echo echo done ^> \"_done%%i.tmp\"");
    writeLine(ofs, ")");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Phase 2: Run parallel compilation
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Phase 2: Run parallel compilation ===");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs, "echo  Running parallel compilation...");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs);

    // Launch workers with affinity masks (dynamic based on NUM_WORKERS)
    writeLine(ofs, "REM Launch each worker on a CPU core");
    writeLine(ofs, "for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "    set /a \"_aff=1 << (%%i - 1)\"");
    writeLine(ofs, "    call :LAUNCH_WORKER %%i !_aff!");
    writeLine(ofs, ")");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Phase 3: Wait for workers
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Phase 3: Wait for all workers to complete ===");
    writeLine(ofs, "echo Waiting for all workers to finish...");
    writeLine(ofs, ":WAIT_LOOP");
    writeLine(ofs, "timeout /t 2 /nobreak >nul");
    writeLine(ofs, "set ALL_DONE=1");
    writeLine(ofs, "for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "    if not exist \"_done%%i.tmp\" set ALL_DONE=0");
    writeLine(ofs, ")");
    writeLine(ofs, "if \"%ALL_DONE%\"==\"0\" goto WAIT_LOOP");
    writeLine(ofs);
    writeLine(ofs, "echo.");
    writeLine(ofs, "echo All workers finished compiling!");
    writeLine(ofs);

    // Error check
    writeLine(ofs, "REM Error check");
    writeLine(ofs, "if exist \"_compile_errors.log\" (");
    writeLine(ofs, "    echo [ERROR] Compilation errors occurred. Showing error lines from worker logs:");
    writeLine(ofs, "    echo.");
    writeLine(ofs, "    for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "        if exist \"_worker%%i.log\" (");
    writeLine(ofs, "            echo === Worker%%i errors ===");
    writeLine(ofs, "            findstr /i /n /c:\"Error \" /c:\"Fatal \" /c:\"cannot \" /c:\"undefined \" \"_worker%%i.log\"");
    writeLine(ofs, "            echo.");
    writeLine(ofs, "        )");
    writeLine(ofs, "    )");
    writeLine(ofs, "    echo Skipping link step.");
    writeLine(ofs, "    goto CLEANUP");
    writeLine(ofs, ")");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Phase 4: Version info & resources (if IncludeVerInfo=1)
    // ----------------------------------------------------------------
    if (proj.includeVerInfo) {
        writeLine(ofs, "REM === Phase 4: Parse version info from .bpr and compile resources ===");
        writeLine(ofs, "echo.");
        writeLine(ofs, "echo Parsing version info from " + bprFileName + "...");

        // Initialize version variables
        writeLine(ofs, "set \"VER_MAJOR=0\"");
        writeLine(ofs, "set \"VER_MINOR=0\"");
        writeLine(ofs, "set \"VER_RELEASE=0\"");
        writeLine(ofs, "set \"VER_BUILD=0\"");
        writeLine(ofs, "set \"VER_DEBUG=0\"");
        writeLine(ofs, "set \"VER_PRERELEASE=0\"");
        writeLine(ofs, "set \"VER_FILEVERSION=\"");
        writeLine(ofs, "set \"VER_PRODUCTVERSION=\"");
        writeLine(ofs, "set \"VER_COMPANYNAMELINE=\"");
        writeLine(ofs, "set \"VER_FILEDESCLINE=\"");
        writeLine(ofs, "set \"VER_INTERNALNAME=\"");
        writeLine(ofs, "set \"VER_LEGALCOPYRIGHT=\"");
        writeLine(ofs, "set \"VER_LEGALTRADEMARKS=\"");
        writeLine(ofs, "set \"VER_ORIGINALFILENAME=\"");
        writeLine(ofs, "set \"VER_PRODUCTNAME=\"");
        writeLine(ofs, "set \"VER_COMMENTS=\"");
        writeLine(ofs, "set \"VER_LOCALE=0\"");
        writeLine(ofs, "set \"VER_CODEPAGE=0\"");

        // Parse version info from .bpr at runtime
        writeLine(ofs, "for /f \"usebackq tokens=1,* delims==\" %%a in (\"" + bprFileName + "\") do (");
        writeLine(ofs, "    if \"%%a\"==\"MajorVer\" set \"VER_MAJOR=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"MinorVer\" set \"VER_MINOR=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"Release\" set \"VER_RELEASE=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"Build\" set \"VER_BUILD=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"Debug\" set \"VER_DEBUG=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"PreRelease\" set \"VER_PRERELEASE=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"Locale\" set \"VER_LOCALE=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"CodePage\" set \"VER_CODEPAGE=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"FileVersion\" set \"VER_FILEVERSION=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"ProductVersion\" set \"VER_PRODUCTVERSION=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"CompanyName\" set \"VER_COMPANYNAMELINE=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"FileDescription\" set \"VER_FILEDESCLINE=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"InternalName\" set \"VER_INTERNALNAME=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"LegalCopyright\" set \"VER_LEGALCOPYRIGHT=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"LegalTrademarks\" set \"VER_LEGALTRADEMARKS=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"OriginalFilename\" set \"VER_ORIGINALFILENAME=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"ProductName\" set \"VER_PRODUCTNAME=%%b\"");
        writeLine(ofs, "    if \"%%a\"==\"Comments\" set \"VER_COMMENTS=%%b\"");
        writeLine(ofs, ")");
        writeLine(ofs, "echo   Version: !VER_MAJOR!.!VER_MINOR!.!VER_RELEASE!.!VER_BUILD!");
        writeLine(ofs);

        // Calculate FILEFLAGS
        writeLine(ofs, "REM Calculate FILEFLAGS");
        writeLine(ofs, "set /a \"VER_FILEFLAGS=0\"");
        writeLine(ofs, "if \"!VER_DEBUG!\"==\"1\" set /a \"VER_FILEFLAGS=!VER_FILEFLAGS!|0x1\"");
        writeLine(ofs, "if \"!VER_PRERELEASE!\"==\"1\" set /a \"VER_FILEFLAGS=!VER_FILEFLAGS!|0x2\"");
        writeLine(ofs);

        // DEC2HEX4 for locale and codepage
        writeLine(ofs, "REM Convert Locale and CodePage to hex for block identifier");
        writeLine(ofs, "call :DEC2HEX4 !VER_LOCALE! VER_LOCALE_HEX");
        writeLine(ofs, "call :DEC2HEX4 !VER_CODEPAGE! VER_CODEPAGE_HEX");
        writeLine(ofs, "set \"VER_BLOCK=!VER_LOCALE_HEX!!VER_CODEPAGE_HEX!\"");
        writeLine(ofs, "echo   Block ID: !VER_BLOCK!");
        writeLine(ofs);

        // Generate .rc file
        writeLine(ofs, "REM Generate " + proj.projectBaseName + ".rc with version info");
        writeLine(ofs, "echo Generating " + proj.projectBaseName + ".rc...");
        // Write preamble lines from existing .rc (ICON declarations etc.)
        {
            // Check if preamble already has an ICON line
            bool hasIcon = false;
            for (const auto& pline : proj.rcPreambleLines) {
                std::string upper = pline;
                for (auto& c : upper) c = toupper((unsigned char)c);
                if (upper.find("ICON") != std::string::npos &&
                    upper.find("//") == std::string::npos &&
                    upper.find("VERSIONINFO") == std::string::npos) {
                    hasIcon = true;
                    break;
                }
            }

            bool first = true;
            // If no ICON in preamble, auto-add one if .ico exists
            if (!hasIcon) {
                writeLine(ofs, "if exist \"" + proj.projectBaseName + ".ico\" (");
                writeLine(ofs, "    > " + proj.projectBaseName + ".rc echo MAINICON ICON \"" + proj.projectBaseName + ".ico\"");
                writeLine(ofs, "    >> " + proj.projectBaseName + ".rc echo.");
                writeLine(ofs, ") else (");
                writeLine(ofs, "    > " + proj.projectBaseName + ".rc echo.");
                writeLine(ofs, ")");
                first = false;
            }

            if (!proj.rcPreambleLines.empty()) {
                for (const auto& pline : proj.rcPreambleLines) {
                    std::string redirect = first ? "> " : ">> ";
                    first = false;
                    writeLine(ofs, redirect + proj.projectBaseName + ".rc echo " + pline);
                }
                writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo.");
            } else if (first) {
                // No existing .rc and no .ico found - write empty first line
                writeLine(ofs, "> " + proj.projectBaseName + ".rc echo.");
            }
        }
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo 1 VERSIONINFO");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo FILEVERSION !VER_MAJOR!,!VER_MINOR!,!VER_RELEASE!,!VER_BUILD!");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo PRODUCTVERSION !VER_MAJOR!,!VER_MINOR!,!VER_RELEASE!,!VER_BUILD!");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo FILEFLAGSMASK 0x3fL");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo FILEFLAGS 0x!VER_FILEFLAGS!L");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo FILEOS 0x40004L");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo FILETYPE 0x1L");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo FILESUBTYPE 0x0L");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo BEGIN");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo   BLOCK \"StringFileInfo\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo   BEGIN");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo     BLOCK \"!VER_BLOCK!\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo     BEGIN");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"CompanyName\", \"!VER_COMPANYNAMELINE!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"FileDescription\", \"!VER_FILEDESCLINE!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"FileVersion\", \"!VER_FILEVERSION!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"InternalName\", \"!VER_INTERNALNAME!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"LegalCopyright\", \"!VER_LEGALCOPYRIGHT!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"LegalTrademarks\", \"!VER_LEGALTRADEMARKS!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"OriginalFilename\", \"!VER_ORIGINALFILENAME!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"ProductName\", \"!VER_PRODUCTNAME!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"ProductVersion\", \"!VER_PRODUCTVERSION!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo       VALUE \"Comments\", \"!VER_COMMENTS!\\0\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo     END");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo   END");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo   BLOCK \"VarFileInfo\"");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo   BEGIN");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo     VALUE \"Translation\", 0x!VER_LOCALE_HEX!, !VER_CODEPAGE!");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo   END");
        writeLine(ofs, ">> " + proj.projectBaseName + ".rc echo END");
        writeLine(ofs);

        // Compile resources
        writeLine(ofs, "echo Compiling resources...");
        writeLine(ofs, "\"%BCB%\\BIN\\brcc32\" -fo" + proj.projectBaseName + ".res " + proj.projectBaseName + ".rc");
        writeLine(ofs, "if errorlevel 1 (");
        writeLine(ofs, "    echo [ERROR] " + proj.projectBaseName + ".rc resource compilation failed.");
        writeLine(ofs, "    goto CLEANUP");
        writeLine(ofs, ")");

        // Compile secondary .res files (non-primary .res in RESFILES)
        // e.g. "ObjFiles\IniFile.res" -> compile IniFile\IniFile.rc, copy to ObjFiles
        {
            std::string primaryRes = toLower(proj.projectBaseName + ".res");
            std::vector<std::string> resEntries = splitBySpace(resFilesStr);
            for (const auto& resEntry : resEntries) {
                std::string resBaseName = getBaseName(resEntry);
                std::string resFileName = getFileName(resEntry);
                if (toLower(resFileName) == primaryRes) continue; // skip primary

                std::string resDir = getDirPart(resEntry);
                // The .rc source is expected at {baseName}\{baseName}.rc
                writeLine(ofs, "pushd " + resBaseName);
                writeLine(ofs, "\"%BCB%\\BIN\\brcc32\" -fo" + resBaseName +
                          ".res " + resBaseName + ".rc");
                writeLine(ofs, "popd");
                writeLine(ofs, "if errorlevel 1 (");
                writeLine(ofs, "    echo [ERROR] " + resBaseName +
                          ".rc resource compilation failed.");
                writeLine(ofs, "    goto CLEANUP");
                writeLine(ofs, ")");
                if (!resDir.empty() && resDir != ".") {
                    writeLine(ofs, "copy /Y " + resBaseName + "\\" + resBaseName +
                              ".res " + resDir + "\\" + resBaseName + ".res >nul");
                }
            }
        }

        writeLine(ofs);
    }

    // ----------------------------------------------------------------
    // Phase 5: Link
    // ----------------------------------------------------------------
    writeLine(ofs, "REM === Phase 5: Link (using a generated response file) ===");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs, "echo  Linking...");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs);

    writeLine(ofs, "set \"RSPFILE=_ilink_" + projBaseNameLower + ".rsp\"");
    writeLine(ofs, "REM Generate ilink32 response file (split long lists into multiple lines)");
    writeLine(ofs, "(");

    // LFLAGS line
    writeLine(ofs, "  echo %LFLAGS% +");

    // Build the obj list: startup objs + all OBJFILES entries
    // We need to group them into lines of reasonable length (~60 obj files per line)
    {
        std::vector<std::string> allObjs;

        // Startup objs first
        for (const auto& sObj : allobjInfo.startupObjs) {
            allObjs.push_back(sObj);
        }

        // If ALLOBJ has $(PACKAGES), insert the PACKAGES content
        if (allobjInfo.hasPackages) {
            std::vector<std::string> pkgs = splitBySpace(proj.packages);
            for (const auto& pkg : pkgs) {
                allObjs.push_back(pkg);
            }
        }

        // All OBJFILES entries
        for (const auto& obj : proj.objFiles) {
            allObjs.push_back(obj);
        }

        // Split into lines of ~60 entries per line
        const size_t OBJS_PER_LINE = 60;
        for (size_t i = 0; i < allObjs.size(); ) {
            std::string lineStr = "  echo ";
            size_t end = std::min(i + OBJS_PER_LINE, allObjs.size());
            for (size_t j = i; j < end; ++j) {
                if (j > i) lineStr += ' ';
                lineStr += allObjs[j];
            }

            if (end < allObjs.size()) {
                // More lines to come: continuation
                lineStr += " +";
            } else {
                // Last obj line: terminate with comma + space + plus
                lineStr += ", +";
            }
            writeLine(ofs, lineStr);
            i = end;
        }
    }

    // Project name line
    writeLine(ofs, "  echo " + proj.projectName + ",, +");

    // Libs line: LIBFILES + LIBRARIES + system libs
    {
        std::string libLine = "  echo ";
        if (!libFilesStr.empty()) {
            libLine += libFilesStr;
        }
        if (!librariesStr.empty()) {
            if (libLine.size() > 7) libLine += ' '; // after "  echo "
            libLine += librariesStr;
        }
        for (const auto& sLib : allLibInfo.systemLibs) {
            libLine += ' ';
            libLine += sLib;
        }
        libLine += ", +";
        writeLine(ofs, libLine);
    }

    // Empty def file line
    writeLine(ofs, "  echo , +");

    // Res files line
    writeLine(ofs, "  echo " + resFilesStr);

    writeLine(ofs, ") > \"%RSPFILE%\"");
    writeLine(ofs);

    // Run linker
    writeLine(ofs, "\"%BCB%\\BIN\\ilink32\" @\"%RSPFILE%\"");
    writeLine(ofs, "if errorlevel 1 (");
    writeLine(ofs, "    echo [ERROR] Link failed.");
    writeLine(ofs, "    goto CLEANUP");
    writeLine(ofs, ")");
    writeLine(ofs);

    writeLine(ofs, "echo.");
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs, "echo  Build output: " + proj.projectName);
    writeLine(ofs, "echo ============================================================");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Cleanup
    // ----------------------------------------------------------------
    writeLine(ofs, ":CLEANUP");
    writeLine(ofs, "set END_TIME=%time%");
    writeLine(ofs, "echo.");
    writeLine(ofs, "echo Start time: %START_TIME%");
    writeLine(ofs, "echo End time:   %END_TIME%");
    writeLine(ofs);

    writeLine(ofs, "REM Remove temporary files");
    writeLine(ofs, "for /L %%i in (1,1,%NUM_WORKERS%) do (");
    writeLine(ofs, "    if exist \"_worker%%i.bat\" del \"_worker%%i.bat\"");
    writeLine(ofs, "    if exist \"_done%%i.tmp\" del \"_done%%i.tmp\"");
    writeLine(ofs, ")");
    writeLine(ofs, "if exist \"_compile_errors.log\" del \"_compile_errors.log\"");
    writeLine(ofs, "REM Worker logs are kept for review: _worker1.log .. _workerN.log");
    writeLine(ofs, "echo.");
    writeLine(ofs, "pause");
    writeLine(ofs, "exit /b");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Subroutine: FIND_AND_ASSIGN
    // ----------------------------------------------------------------
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, "REM Subroutine: Find source file and assign it to a worker");
    writeLine(ofs, "REM   Worker ID is computed by round-robin using _WORKER_IDX");
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, ":FIND_AND_ASSIGN");
    writeLine(ofs, "set \"_NAME=%~1\"");

    // Strip objDir prefix to get the base filename
    // The prefix might be "ObjFiles\\" or other directory.
    if (objDir != ".") {
        writeLine(ofs, "REM Strip " + objDir + "\\ prefix to get the base filename");
        writeLine(ofs, "set \"_NAME=%_NAME:" + objDir + "\\=%\"");
    }

    // Compute worker ID dynamically: _WID = (_WORKER_IDX % NUM_WORKERS) + 1
    writeLine(ofs, "set /a \"_WID=(_WORKER_IDX %% NUM_WORKERS) + 1\"");
    writeLine(ofs, "set /a \"_WORKER_IDX+=1\"");
    writeLine(ofs);

    writeLine(ofs, "REM Search .cpp / .c / .pas / .asm in each source directory");
    writeLine(ofs, "for %%d in (%SRCDIRS%) do (");
    writeLine(ofs, "    if exist \"%%d\\%_NAME%.cpp\" (");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo echo Compiling: %%d\\%_NAME%.cpp");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo \"%%BCB%%\\BIN\\bcc32\" %%CFLAG1%% %%CFLAG2%% %%CFLAG3%% %%CDEFINES%% -n%%OBJDIR%% \"%%d\\%_NAME%.cpp\"");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo if errorlevel 1 set /a ERRORS+=1");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo set /a COMPILED+=1");
    writeLine(ofs, "        set /a FILE_COUNT+=1");
    writeLine(ofs, "        goto :EOF");
    writeLine(ofs, "    )");
    writeLine(ofs, "    if exist \"%%d\\%_NAME%.c\" (");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo echo Compiling: %%d\\%_NAME%.c");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo \"%%BCB%%\\BIN\\bcc32\" %%CFLAG1%% %%CFLAG2%% %%CFLAG3%% %%CDEFINES%% -n%%OBJDIR%% \"%%d\\%_NAME%.c\"");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo if errorlevel 1 set /a ERRORS+=1");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo set /a COMPILED+=1");
    writeLine(ofs, "        set /a FILE_COUNT+=1");
    writeLine(ofs, "        goto :EOF");
    writeLine(ofs, "    )");
    writeLine(ofs, "    if exist \"%%d\\%_NAME%.pas\" (");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo echo Compiling: %%d\\%_NAME%.pas");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo \"%%BCB%%\\BIN\\dcc32\" %%PFLAGS%% \"%%d\\%_NAME%.pas\"");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo if errorlevel 1 set /a ERRORS+=1");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo set /a COMPILED+=1");
    writeLine(ofs, "        set /a FILE_COUNT+=1");
    writeLine(ofs, "        goto :EOF");
    writeLine(ofs, "    )");
    writeLine(ofs, "    if exist \"%%d\\%_NAME%.asm\" (");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo echo Assembling: %%d\\%_NAME%.asm");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo \"%%BCB%%\\BIN\\tasm32\" %%AFLAGS%% \"%%d\\%_NAME%.asm\", \"%%OBJDIR%%\\%_NAME%.obj\"");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo if errorlevel 1 set /a ERRORS+=1");
    writeLine(ofs, "        >>\"_worker%_WID%.bat\" echo set /a COMPILED+=1");
    writeLine(ofs, "        set /a FILE_COUNT+=1");
    writeLine(ofs, "        goto :EOF");
    writeLine(ofs, "    )");
    writeLine(ofs, ")");
    writeLine(ofs);
    writeLine(ofs, "echo [WARN] Source not found: %_NAME%");
    writeLine(ofs, "goto :EOF");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Subroutine: LAUNCH_WORKER
    // ----------------------------------------------------------------
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, "REM Subroutine: Launch a worker with CPU affinity");
    writeLine(ofs, "REM   Usage: call :LAUNCH_WORKER <worker_id> <affinity_decimal>");
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, ":LAUNCH_WORKER");
    writeLine(ofs, "setlocal");
    writeLine(ofs, "set \"_wid=%~1\"");
    writeLine(ofs, "set /a \"_val=%~2\"");
    // Convert affinity to hex inline
    writeLine(ofs, "set \"_hexchars=0123456789ABCDEF\"");
    writeLine(ofs, "set \"_hexresult=\"");
    writeLine(ofs, ":_LAUNCH_HEXLOOP");
    writeLine(ofs, "set /a \"_digit=_val & 0xF\"");
    writeLine(ofs, "set /a \"_val>>=4\"");
    writeLine(ofs, "for %%d in (!_digit!) do set \"_hexresult=!_hexchars:~%%d,1!!_hexresult!\"");
    writeLine(ofs, "if !_val! GTR 0 goto _LAUNCH_HEXLOOP");
    writeLine(ofs, "if \"!_hexresult!\"==\"\" set \"_hexresult=0\"");
    writeLine(ofs, "start \"Worker%_wid%\" /affinity 0x!_hexresult! /min cmd /c \"_worker%_wid%.bat > _worker%_wid%.log 2>&1\"");
    writeLine(ofs, "endlocal");
    writeLine(ofs, "goto :EOF");
    writeLine(ofs);

    // ----------------------------------------------------------------
    // Subroutine: DEC2HEX4
    // ----------------------------------------------------------------
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, "REM Subroutine: Convert decimal to 4-digit hex string");
    writeLine(ofs, "REM   Usage: call :DEC2HEX4 <decimal_value> <output_var>");
    writeLine(ofs, "REM ============================================================");
    writeLine(ofs, ":DEC2HEX4");
    writeLine(ofs, "setlocal");
    writeLine(ofs, "set /a \"_val=%~1\"");
    writeLine(ofs, "set \"_hexchars=0123456789ABCDEF\"");
    writeLine(ofs, "set \"_result=\"");
    writeLine(ofs, "for /L %%i in (1,1,4) do (");
    writeLine(ofs, "    set /a \"_digit=_val & 0xF\"");
    writeLine(ofs, "    set /a \"_val>>=4\"");
    writeLine(ofs, "    for %%d in (!_digit!) do set \"_result=!_hexchars:~%%d,1!!_result!\"");
    writeLine(ofs, ")");
    writeLine(ofs, "endlocal & set \"%~2=%_result%\"");
    writeLine(ofs, "goto :EOF");

    ofs.close();

    std::cout << "Generated: " << batPath << std::endl;
    std::cout << "  Project:  " << proj.projectName << std::endl;
    std::cout << "  OBJ files: " << proj.objFiles.size() << std::endl;
    std::cout << "  Workers:  " << numWorkers << std::endl;
    if (proj.includeVerInfo) {
        std::cout << "  Version:  " << proj.majorVer << "." << proj.minorVer
                  << "." << proj.release << "." << proj.build << std::endl;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: BprToBat.exe <bprfile> [num_workers]" << std::endl;
        std::cerr << "  bprfile:     path to .bpr file (required)" << std::endl;
        std::cerr << "  num_workers: 1-16, default 4" << std::endl;
        return 1;
    }

    std::string bprPath = argv[1];
    int numWorkers = 4;

    if (argc >= 3) {
        numWorkers = std::atoi(argv[2]);
        if (numWorkers < 1 || numWorkers > 16) {
            std::cerr << "Error: num_workers must be between 1 and 16." << std::endl;
            return 1;
        }
    }

    BprProject proj;
    if (!parseBpr(bprPath, proj)) {
        std::cerr << "Error: Failed to parse BPR file: " << bprPath << std::endl;
        return 1;
    }

    if (proj.projectName.empty()) {
        std::cerr << "Error: PROJECT variable not found in BPR file." << std::endl;
        return 1;
    }

    if (proj.objFiles.empty()) {
        std::cerr << "Error: No OBJFILES found in BPR file." << std::endl;
        return 1;
    }

    // Read existing .rc preamble (ICON lines etc.) from same directory as .bpr
    {
        std::string bprDir = getDirPart(bprPath);
        if (bprDir.empty()) bprDir = ".";
        readRcPreamble(bprDir, proj);
    }

    if (!generateBat(bprPath, proj, numWorkers)) {
        return 1;
    }

    return 0;
}
