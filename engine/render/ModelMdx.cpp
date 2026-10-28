// Warcraft III .mdx (binary) and .mdl (text) loaders. (stub: being implemented)
#include "Model.h"

#include <stdexcept>

namespace rts::render {

std::unique_ptr<Model> loadMdx(const std::string& path, const std::string&, const FileReader&, std::vector<std::string>*) {
    throw std::runtime_error(path + ": format not implemented yet");
}

std::unique_ptr<Model> loadMdl(const std::string& path, const std::string&, const FileReader&, std::vector<std::string>*) {
    throw std::runtime_error(path + ": format not implemented yet");
}

} // namespace rts::render
