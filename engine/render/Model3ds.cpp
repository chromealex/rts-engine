// 3D Studio .3ds loader. (stub: being implemented)
#include "Model.h"

#include <stdexcept>

namespace rts::render {

std::unique_ptr<Model> load3ds(const std::string& path, const std::string&, const FileReader&, std::vector<std::string>*) {
    throw std::runtime_error(path + ": format not implemented yet");
}

} // namespace rts::render
