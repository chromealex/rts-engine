// Small helpers shared by toolchain detection and the installers (tools/rtsbuild/Toolchains.h).
#pragma once
#include <string>
#include <vector>

#include "tools/rtsbuild/Toolchains.h"

namespace rts::build::tc {

bool there(const fs::path& p);
bool isFile(const fs::path& p);
std::string readFile(const fs::path& p);
std::vector<fs::path> subdirs(const fs::path& dir);          // sorted, newest version last
std::string capture(const std::string& cmd);                  // stdout + stderr of a command
std::string shq(const fs::path& p);                           // shell quoting
std::string firstLine(const std::string& s);
char pathSep(const ToolEnv& env);
std::vector<fs::path> extraDirs(const ToolEnv& env);          // searched after PATH
fs::path exeIn(const fs::path& dir, const std::string& name, const ToolEnv& env);
// cmd.exe strips the first and last quote of a command that starts with one: wrap it once more
inline std::string sysCmd(const std::string& c) {
#if defined(_WIN32)
    return "\"" + c + "\"";
#else
    return c;
#endif
}
void setEnvVar(const std::string& k, const std::string& v);

} // namespace rts::build::tc
