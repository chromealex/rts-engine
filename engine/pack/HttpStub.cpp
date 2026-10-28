// Placeholder until the platform HTTP fetchers land: file:// and custom schemes already work.
#include "Chunks.h"

namespace rts::pack {

std::unique_ptr<Download> fetchHttp(const std::string& url, const std::string&, std::string& error) {
    error = "HTTP downloads are not available in this build yet: " + url;
    return nullptr;
}

void setAndroidJavaVm(void*) {}

} // namespace rts::pack
