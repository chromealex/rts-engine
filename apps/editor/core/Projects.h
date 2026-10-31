// Projects live anywhere on the user's disk (like Unity projects); the engine ships templates in
// <engine>/templates/<id>/ (sample games). A new project is a copy of a template (or the empty
// template) in a folder the user picks. Recently opened projects are remembered in
// ~/.rtsengine/projects.json, shared by the editor and rtsmcp.
#pragma once
#include <string>
#include <vector>

namespace rts::editor {

struct TemplateInfo {
    std::string id, dir, name, description, genre, preview;   // preview: image path inside the template (may be empty)
    int maps = 0;
};

struct ProjectInfo {
    std::string dir, name;
    std::string file;           // <dir>/<Title>.rtsproj when known (ProjectFile.h)
    long long lastOpened = 0;   // unix seconds
    bool exists = false;
};

std::vector<TemplateInfo> listTemplates(const std::string& engineDir);   // "empty" first, then templates/ sorted by name

std::string projectsRegistry();                         // ~/.rtsengine/projects.json
std::string defaultProjectsDir();                       // ~/RTSProjects
std::vector<ProjectInfo> recentProjects();              // most recent first
void rememberProject(const std::string& dir, const std::string& name, const std::string& file = "");
void forgetProject(const std::string& dir);

// Folder name for a project title: "Моя стратегия" -> "Моя стратегия" (keeps letters, digits, spaces, - _).
std::string projectFolderName(const std::string& title);
// Creates <parentDir>/<folder> from a template id ("empty" or a templates/ folder) with its
// <Title>.rtsproj (the template's own .rtsproj is not copied). Returns the new project folder or ""
// with a readable error.
std::string createProject(const std::string& engineDir, const std::string& templateId, const std::string& title,
                          const std::string& parentDir, const std::string& folder, std::string* error);

} // namespace rts::editor
