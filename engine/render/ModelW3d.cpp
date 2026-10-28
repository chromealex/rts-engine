// C&C Generals .w3d loader. (stub: being implemented)
#include "Model.h"

#include <stdexcept>

namespace rts::render {

std::unique_ptr<Model> loadW3d(const std::string& path, const std::string&, const FileReader&, std::vector<std::string>*) {
    throw std::runtime_error(path + ": format not implemented yet");
}

} // namespace rts::render
