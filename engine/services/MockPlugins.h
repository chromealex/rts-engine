// Mock store ("mock-store") and ads ("mock-ads") plugins for desktop development and tests.
#pragma once

namespace rts::services {
// Registers both mocks (done automatically on desktop builds; call it to use them elsewhere).
void registerMockPlugins();
} // namespace rts::services
