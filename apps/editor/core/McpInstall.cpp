#include "McpInstall.h"
#include "engine/pack/FileCopy.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

// ------------------------------------------------------------------ minimal JSON text scanner
// Finds value ranges without rebuilding the document (our Json reader turns numbers into fixed
// point, so a parse + dump round trip could alter unrelated numbers in the user's config).

size_t skipWs(const std::string& s, size_t i) {
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') { size_t e = s.find("*/", i + 2); i = e == std::string::npos ? s.size() : e + 2; continue; }
        break;
    }
    return i;
}

// End (one past) of the string starting at s[i] == '"', or npos.
size_t stringEnd(const std::string& s, size_t i) {
    for (++i; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '"') return i + 1;
    }
    return std::string::npos;
}

// End (one past) of the value starting at s[i], or npos.
size_t valueEnd(const std::string& s, size_t i) {
    if (i >= s.size()) return std::string::npos;
    if (s[i] == '"') return stringEnd(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '"') { i = stringEnd(s, i); if (i == std::string::npos) return i; --i; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) return i + 1; }
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') ++i;
    return i;
}

std::string unquote(const std::string& s, size_t a, size_t b) {   // [a, b) includes the quotes
    std::string out;
    for (size_t i = a + 1; i + 1 < b; ++i) {
        if (s[i] == '\\' && i + 2 < b) {
            const char n = s[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
        } else out += s[i];
    }
    return out;
}

struct Member { size_t keyStart, valueStart, valueEnd; std::string key; };

// Members of the object starting at s[open] == '{'. `close` gets the index of its '}'.
bool members(const std::string& s, size_t open, std::vector<Member>& out, size_t& close) {
    size_t i = skipWs(s, open + 1);
    if (i < s.size() && s[i] == '}') { close = i; return true; }
    while (i < s.size()) {
        if (s[i] != '"') return false;
        const size_t ke = stringEnd(s, i);
        if (ke == std::string::npos) return false;
        Member m{i, 0, 0, unquote(s, i, ke)};
        i = skipWs(s, ke);
        if (i >= s.size() || s[i] != ':') return false;
        m.valueStart = skipWs(s, i + 1);
        m.valueEnd = valueEnd(s, m.valueStart);
        if (m.valueEnd == std::string::npos) return false;
        out.push_back(m);
        i = skipWs(s, m.valueEnd);
        if (i < s.size() && s[i] == ',') { i = skipWs(s, i + 1); continue; }
        if (i < s.size() && s[i] == '}') { close = i; return true; }
        return false;
    }
    return false;
}

std::string jsonStr(const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o + "\"";
}

std::string tomlStr(const std::string& v) { return jsonStr(v); }   // TOML basic strings escape the same way

bool readText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeText(const fs::path& p, const std::string& text, std::string* error) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (fs::exists(p, ec)) rts::pack::copyFileReplace(p, fs::path(p.string() + ".bak"), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) { if (error) *error = "не удалось записать " + p.string(); return false; }
        f << text;
    }
    fs::rename(tmp, p, ec);
    if (ec) { if (error) *error = "не удалось записать " + p.string() + ": " + ec.message(); return false; }
    return true;
}

std::string home() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const char* h = std::getenv("USERPROFILE"); h && *h) return h;
    return {};
}

std::string claudeDesktopConfig() {
#if defined(_WIN32)
    const char* a = std::getenv("APPDATA");
    return a ? (fs::path(a) / "Claude" / "claude_desktop_config.json").string() : std::string();
#elif defined(__APPLE__)
    return (fs::path(home()) / "Library" / "Application Support" / "Claude" / "claude_desktop_config.json").string();
#else
    return (fs::path(home()) / ".config" / "Claude" / "claude_desktop_config.json").string();
#endif
}

bool haveCommand(const char* name) {
#if defined(_WIN32)
    std::string cmd = std::string("where ") + name + " >NUL 2>&1";
#else
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
#endif
    return std::system(cmd.c_str()) == 0;
}

std::string shellArg(const std::string& a) {
#if defined(_WIN32)
    return "\"" + a + "\"";
#else
    std::string o = "'";
    for (char c : a) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}

struct Client { const char* id; const char* name; enum Kind { Json, Toml, ClaudeCode } kind; };
const Client kClients[] = {
    {"claude-code", "Claude Code", Client::ClaudeCode},
    {"claude-desktop", "Claude Desktop", Client::Json},
    {"cursor", "Cursor", Client::Json},
    {"windsurf", "Windsurf", Client::Json},
    {"codex", "Codex CLI", Client::Toml},
    {"project", "Проект (.mcp.json в папке движка)", Client::Json},
};

std::string configOf(const std::string& id, const std::string& engineDir) {
    const fs::path h = home();
    if (id == "claude-code") return (h / ".claude.json").string();
    if (id == "claude-desktop") return claudeDesktopConfig();
    if (id == "cursor") return (h / ".cursor" / "mcp.json").string();
    if (id == "windsurf") return (h / ".codeium" / "windsurf" / "mcp_config.json").string();
    if (id == "codex") return (h / ".codex" / "config.toml").string();
    if (id == "project") return (fs::path(engineDir) / ".mcp.json").string();
    return {};
}

// command of mcpServers[name] in a JSON config ("" when absent)
std::string jsonCommand(const std::string& text, const std::string& name) {
    const size_t root = skipWs(text, 0);
    if (root >= text.size() || text[root] != '{') return {};
    std::vector<Member> top; size_t close = 0;
    if (!members(text, root, top, close)) return {};
    for (auto& m : top) {
        if (m.key != "mcpServers" || text[m.valueStart] != '{') continue;
        std::vector<Member> servers; size_t c2 = 0;
        if (!members(text, m.valueStart, servers, c2)) return {};
        for (auto& s : servers) {
            if (s.key != name || text[s.valueStart] != '{') continue;
            std::vector<Member> fields; size_t c3 = 0;
            if (!members(text, s.valueStart, fields, c3)) return {};
            for (auto& f : fields) if (f.key == "command" && text[f.valueStart] == '"') return unquote(text, f.valueStart, f.valueEnd);
            return "?";
        }
    }
    return {};
}

// [mcp_servers.<name>] section range in a TOML file
bool tomlSection(const std::string& text, const std::string& name, size_t& a, size_t& b) {
    const std::string head = "[mcp_servers." + name + "]";
    size_t p = 0;
    while ((p = text.find(head, p)) != std::string::npos) {
        if (p == 0 || text[p - 1] == '\n') break;
        p += head.size();
    }
    if (p == std::string::npos) return false;
    a = p;
    b = text.size();
    for (size_t q = text.find('\n', p); q != std::string::npos; q = text.find('\n', q + 1))
        if (q + 1 < text.size() && text[q + 1] == '[') { b = q + 1; break; }
    return true;
}

std::string tomlEntry(const McpServerSpec& spec) {
    std::string s = "[mcp_servers." + spec.name + "]\ncommand = " + tomlStr(spec.command) + "\nargs = [";
    for (size_t i = 0; i < spec.args.size(); ++i) s += (i ? ", " : "") + tomlStr(spec.args[i]);
    return s + "]\n";
}

} // namespace

std::string mcpEntryJson(const McpServerSpec& spec, const char* indent) {
    std::string in = indent;
    std::string s = "{\n" + in + "  \"command\": " + jsonStr(spec.command) + ",\n" + in + "  \"args\": [";
    for (size_t i = 0; i < spec.args.size(); ++i) s += (i ? ", " : "") + jsonStr(spec.args[i]);
    return s + "]\n" + in + "}";
}

bool mcpPatchJson(std::string& text, const std::string& name, const std::string& entryJson, std::string* error) {
    const bool remove = entryJson.empty();
    size_t root = skipWs(text, 0);
    if (root >= text.size()) {
        if (remove) return true;
        text = "{\n  \"mcpServers\": {\n    " + jsonStr(name) + ": " + entryJson + "\n  }\n}\n";
        return true;
    }
    if (text[root] != '{') { if (error) *error = "файл настроек — не JSON-объект"; return false; }
    std::vector<Member> top; size_t close = 0;
    if (!members(text, root, top, close)) { if (error) *error = "файл настроек повреждён (не удалось разобрать JSON)"; return false; }
    for (auto& m : top) {
        if (m.key != "mcpServers") continue;
        if (text[m.valueStart] != '{') { if (error) *error = "\"mcpServers\" — не объект"; return false; }
        std::vector<Member> servers; size_t sclose = 0;
        if (!members(text, m.valueStart, servers, sclose)) { if (error) *error = "не удалось разобрать \"mcpServers\""; return false; }
        for (size_t k = 0; k < servers.size(); ++k) {
            const Member& s = servers[k];
            if (s.key != name) continue;
            if (!remove) { text.replace(s.valueStart, s.valueEnd - s.valueStart, entryJson); return true; }
            // remove the member together with one neighbouring comma
            size_t a = s.keyStart, b = s.valueEnd;
            size_t after = skipWs(text, b);
            if (after < text.size() && text[after] == ',') b = skipWs(text, after + 1);
            else if (k > 0) {
                size_t comma = text.rfind(',', a);
                if (comma != std::string::npos && comma > servers[k - 1].valueEnd - 1) a = comma;
            }
            text.erase(a, b - a);
            return true;
        }
        if (remove) return true;
        const std::string ins = "\n    " + jsonStr(name) + ": " + entryJson + (servers.empty() ? "\n  " : ",");
        text.insert(m.valueStart + 1, ins);
        return true;
    }
    if (remove) return true;
    const std::string ins = "\n  \"mcpServers\": {\n    " + jsonStr(name) + ": " + entryJson + "\n  }" + (top.empty() ? "\n" : ",");
    text.insert(root + 1, ins);
    return true;
}

std::vector<McpClientInfo> mcpClients(const McpServerSpec& spec, const std::string& engineDir) {
    std::vector<McpClientInfo> out;
    for (const Client& c : kClients) {
        McpClientInfo info{c.id, c.name, configOf(c.id, engineDir)};
        std::error_code ec;
        const fs::path cfg = info.configPath;
        info.present = c.kind == Client::ClaudeCode ? (haveCommand("claude") || fs::exists(cfg, ec))
                     : std::string(c.id) == "project" ? true
                     : fs::exists(cfg.parent_path(), ec);
        std::string text;
        if (readText(cfg, text)) {
            if (c.kind == Client::Toml) {
                size_t a = 0, b = 0;
                info.installed = tomlSection(text, spec.name, a, b);
                info.upToDate = info.installed && text.substr(a, b - a).find(tomlStr(spec.command)) != std::string::npos;
            } else {
                const std::string cmd = jsonCommand(text, spec.name);
                info.installed = !cmd.empty();
                info.upToDate = cmd == spec.command;
            }
        }
        out.push_back(info);
    }
    return out;
}

static bool apply(const std::string& clientId, const McpServerSpec& spec, const std::string& engineDir, bool remove, std::string* error) {
    const Client* client = nullptr;
    for (const Client& c : kClients) if (clientId == c.id) client = &c;
    if (!client) { if (error) *error = "неизвестный клиент '" + clientId + "'"; return false; }
    if (!remove) {
        std::error_code ec;
        if (spec.command.empty() || !fs::exists(spec.command, ec)) { if (error) *error = "не найден rtsmcp рядом с редактором (" + spec.command + "): соберите цель rtsmcp"; return false; }
    }
    if (client->kind == Client::ClaudeCode && haveCommand("claude")) {
        // the official way: the CLI owns its config file
        const int removed = std::system(("claude mcp remove --scope user " + shellArg(spec.name) + " >" +
#if defined(_WIN32)
                     "NUL 2>&1"
#else
                     "/dev/null 2>&1"
#endif
                     ).c_str());
        (void)removed;   // "not found" is fine
        if (remove) return true;
        std::string cmd = "claude mcp add --scope user " + shellArg(spec.name) + " -- " + shellArg(spec.command);
        for (auto& a : spec.args) cmd += " " + shellArg(a);
        if (std::system(cmd.c_str()) == 0) return true;
        // fall through to the file edit
    }
    const fs::path cfg = configOf(clientId, engineDir);
    std::string text;
    readText(cfg, text);
    if (client->kind == Client::Toml) {
        size_t a = 0, b = 0;
        const bool has = tomlSection(text, spec.name, a, b);
        if (remove) { if (!has) return true; text.erase(a, b - a); }
        else if (has) text.replace(a, b - a, tomlEntry(spec));
        else text += (text.empty() || text.back() == '\n' ? "\n" : "\n\n") + tomlEntry(spec);
    } else if (!mcpPatchJson(text, spec.name, remove ? std::string() : mcpEntryJson(spec), error)) {
        if (error) *error = cfg.string() + ": " + *error;
        return false;
    }
    return writeText(cfg, text, error);
}

bool mcpInstall(const std::string& clientId, const McpServerSpec& spec, const std::string& engineDir, std::string* error) {
    return apply(clientId, spec, engineDir, false, error);
}

bool mcpUninstall(const std::string& clientId, const McpServerSpec& spec, const std::string& engineDir, std::string* error) {
    return apply(clientId, spec, engineDir, true, error);
}

} // namespace rts::editor
