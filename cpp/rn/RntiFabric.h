#pragma once

namespace facebook::jsi {
class Runtime;
}

namespace rnti {

// Call on the JS thread; returns false when the runtime has no Fabric UIManager.
bool installFabricObserver(facebook::jsi::Runtime &runtime);

} // namespace rnti
