// Project file `<Title>.rtsproj` at the root of a project folder: the file the user double-clicks
// to open the project in the editor (like Unity's / Godot's project files). game.json stays the
// runtime manifest (client, rtsbuild, packs read it); the .rtsproj only marks the folder as a
// project and carries editor-side facts. Format (JSON, comments allowed):
//
//   {"format": 1, "name": "My RTS", "engine": "0.2.0", "manifest": "game.json", "created": "2026-10-28"}
//
// Unknown keys are kept when the editor rewrites the file. rtsbuild never packs *.rtsproj.
#pragma once
#include <string>

namespace rts::editor {

inline constexpr const char* kProjectExt = ".rtsproj";
inline constexpr int kProjectFormat = 1;

struct ProjectFile {
    int format = kProjectFormat;
    std::string name;
    std::string engine;                  // engine version that created / last migrated the project
    std::string manifest = "game.json";  // runtime manifest, relative to the project folder
    std::string created;                 // YYYY-MM-DD (UTC)
};

std::string engineVersion();             // "0.2.0" (CMake project version)
bool isProjectFilePath(const std::string& path);   // extension .rtsproj (any case)

// `<dir>/<name>.rtsproj` (first one by name), or "" when the folder has none.
std::string findProjectFile(const std::string& dir);
bool readProjectFile(const std::string& path, ProjectFile& out, std::string* err);
bool writeProjectFile(const std::string& path, const ProjectFile& pf, std::string* err);
// File name for a project title: "Моя стратегия" -> "Моя стратегия.rtsproj".
std::string projectFileName(const std::string& title);

// Anything the user may hand over as "a project": a .rtsproj file, a project folder, or its
// game.json. Returns the absolute project folder (with the manifest inside), or "" + a readable error.
std::string resolveProjectDir(const std::string& path, std::string* err);

// Makes sure the project folder has a .rtsproj (migration of projects made before it existed):
// writes `<name from game.json>.rtsproj` when missing. Returns its path ("" if it can't be written,
// e.g. a read-only folder); `created` tells whether a new file was written.
std::string ensureProjectFile(const std::string& dir, bool* created = nullptr);

} // namespace rts::editor
