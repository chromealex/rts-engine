// Mock store ("mock-store") and ads ("mock-ads") plugins for desktop development and tests.
#pragma once
#include <string>

namespace rts::services {
// Registers both mocks / one of them ("mock-store", "mock-ads"). The bridge does it for mocks listed
// in game.json "plugins" or with RTS_MOCK_PLUGINS=1; call it yourself in tests or tools.
void registerMockPlugins();
void registerMockPlugin(const std::string& name);
} // namespace rts::services
