#pragma once

namespace rnti {

class Inspector;

// Replaces any TurboModulePerfLogger another SDK installed; opt-in only.
bool installTurboModulePerfLogger(Inspector &inspector);

} // namespace rnti
