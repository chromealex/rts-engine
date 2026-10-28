// File and folder copies that also work with libstdc++ on Windows (MinGW).
//
// There stat() gives every file st_ino == 0, so libstdc++ takes any two files on the same drive
// for the same file: copy_file(overwrite_existing) onto an existing file fails with "File exists",
// and so does fs::copy(recursive) into a folder that already has the subfolders/files. These
// helpers replace the destination explicitly. Header-only: host tools, the runtime and the
// editor can use it without linking anything.
#pragma once
#include <filesystem>
#include <system_error>

namespace rts::pack {

// Copies `from` over `to` (replacing it). Refuses to copy a file onto itself.
inline bool copyFileReplace(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
    namespace fs = std::filesystem;
    std::error_code e1, e2;
    if (fs::weakly_canonical(from, e1) == fs::weakly_canonical(to, e2) && !e1 && !e2) {
        ec = std::make_error_code(std::errc::file_exists);
        return false;
    }
    const fs::file_status st = fs::symlink_status(to, e1);   // not_found sets e1: not an error here
    ec.clear();
    if (st.type() == fs::file_type::regular || st.type() == fs::file_type::symlink) fs::remove(to, ec);
    if (ec) return false;
    return fs::copy_file(from, to, ec) && !ec;
}

// Copies the contents of folder `from` into `to` (created if needed), replacing files that
// exist. Folder symlinks are followed, as fs::copy(recursive) does.
inline bool copyTree(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
    namespace fs = std::filesystem;
    ec.clear();
    fs::create_directories(to, ec);
    if (ec) return false;
    fs::recursive_directory_iterator it(from, fs::directory_options::follow_directory_symlink, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::path out = to / it->path().lexically_relative(from);
        if (it->is_directory(ec)) fs::create_directories(out, ec);
        else if (!ec && it->is_regular_file(ec)) copyFileReplace(it->path(), out, ec);
        if (ec) return false;
    }
    return !ec;
}

} // namespace rts::pack
