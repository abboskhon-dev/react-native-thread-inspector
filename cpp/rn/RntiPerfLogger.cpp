#include "RntiPerfLogger.h"

#include "Inspector.h"

#include <ReactCommon/TurboModulePerfLogger.h>
#include <reactperflogger/NativeModulePerfLogger.h>

#include <atomic>

namespace rnti {

namespace {

// Forwards into the inspector's tracker; unused callbacks stay empty.
// RN 0.88.1 iOS async blocks capture the caller's methodName pointer, which is dangling by execution time.
#if __APPLE__
constexpr bool kExecMethodNameSafe = false;
#else
constexpr bool kExecMethodNameSafe = true;
#endif

const char *execMethod(const char *m) { return kExecMethodNameSafe ? m : nullptr; }

class Forwarder final : public facebook::react::NativeModulePerfLogger {
 public:
  explicit Forwarder(TurboModuleTracker &tracker) : t(tracker) {}
  void moduleDataCreateStart(const char *, int32_t) override {}
  void moduleDataCreateEnd(const char *, int32_t) override {}
  void moduleCreateStart(const char *, int32_t) override {}
  void moduleCreateCacheHit(const char *, int32_t) override {}
  void moduleCreateConstructStart(const char *, int32_t) override {}
  void moduleCreateConstructEnd(const char *, int32_t) override {}
  void moduleCreateSetUpStart(const char *, int32_t) override {}
  void moduleCreateSetUpEnd(const char *, int32_t) override {}
  void moduleCreateEnd(const char *, int32_t) override {}
  void moduleCreateFail(const char *, int32_t) override {}
  void moduleJSRequireBeginningStart(const char *) override {}
  void moduleJSRequireBeginningCacheHit(const char *) override {}
  void moduleJSRequireBeginningEnd(const char *) override {}
  void moduleJSRequireBeginningFail(const char *) override {}
  void moduleJSRequireEndingStart(const char *) override {}
  void moduleJSRequireEndingEnd(const char *) override {}
  void moduleJSRequireEndingFail(const char *) override {}
  void syncMethodCallStart(const char *moduleName, const char *methodName) override { t.syncStart(moduleName, methodName); }
  void syncMethodCallArgConversionStart(const char *, const char *) override {}
  void syncMethodCallArgConversionEnd(const char *, const char *) override {}
  void syncMethodCallExecutionStart(const char *, const char *) override {}
  void syncMethodCallExecutionEnd(const char *, const char *) override {}
  void syncMethodCallReturnConversionStart(const char *, const char *) override {}
  void syncMethodCallReturnConversionEnd(const char *, const char *) override {}
  void syncMethodCallEnd(const char *moduleName, const char *methodName) override { t.syncEnd(moduleName, methodName, false); }
  void syncMethodCallFail(const char *moduleName, const char *methodName) override { t.syncEnd(moduleName, methodName, true); }
  void asyncMethodCallStart(const char *moduleName, const char *methodName) override { t.asyncCallStart(moduleName, methodName); }
  void asyncMethodCallArgConversionStart(const char *, const char *) override {}
  void asyncMethodCallArgConversionEnd(const char *, const char *) override {}
  void asyncMethodCallDispatch(const char *, const char *) override {}
  void asyncMethodCallEnd(const char *moduleName, const char *methodName) override { t.asyncCallEnd(moduleName, methodName, false); }
  void asyncMethodCallFail(const char *moduleName, const char *methodName) override { t.asyncCallEnd(moduleName, methodName, true); }
  void asyncMethodCallBatchPreprocessStart() override {}
  void asyncMethodCallBatchPreprocessEnd(int) override {}
  void asyncMethodCallExecutionStart(const char *moduleName, const char *methodName, int32_t id) override { t.asyncExecStart(moduleName, execMethod(methodName), id); }
  void asyncMethodCallExecutionArgConversionStart(const char *, const char *, int32_t) override {}
  void asyncMethodCallExecutionArgConversionEnd(const char *, const char *, int32_t) override {}
  void asyncMethodCallExecutionEnd(const char *moduleName, const char *methodName, int32_t id) override { t.asyncExecEnd(moduleName, execMethod(methodName), id, false); }
  void asyncMethodCallExecutionFail(const char *moduleName, const char *methodName, int32_t id) override { t.asyncExecEnd(moduleName, execMethod(methodName), id, true); }

 private:
  TurboModuleTracker &t;
};

std::atomic<bool> gInstalled{false};

} // namespace

bool installTurboModulePerfLogger(Inspector &inspector) {
  // RN keeps one global logger and frees the old one on replace, so install exactly once and never remove.
  if (gInstalled.exchange(true)) return true;
  inspector.turboModules().setIgnoredModule("ThreadInspector", "simulate");
  facebook::react::TurboModulePerfLogger::enableLogging(std::make_unique<Forwarder>(inspector.turboModules()));
  return true;
}

} // namespace rnti
