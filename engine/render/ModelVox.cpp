// MagicaVoxel .vox and Tiberian Sun / Red Alert 2 .vxl + .hva loaders. (stub: being implemented)
#include "Model.h"

#include <stdexcept>

namespace rts::render {

std::unique_ptr<Model> loadVox(const std::string& path, const std::string&, const FileReader&, std::vector<std::string>*) {
    throw std::runtime_error(path + ": format not implemented yet");
}

std::unique_ptr<Model> loadVxl(const std::string& path, const std::string&, const FileReader&, std::vector<std::string>*) {
    throw std::runtime_error(path + ": format not implemented yet");
}

} // namespace rts::render
