#import "RNThreadInspector.h"

#include "Inspector.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace rnti;

namespace {

struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

std::shared_mutex gLabelsMu;
std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>> gLabels;

// Recorder::intern is not for hot paths, so labels are cached; the inspector is never torn down.
uint32_t label(Inspector &ins, NSString *name) {
  const std::string_view key = name.UTF8String ?: "";
  {
    std::shared_lock<std::shared_mutex> lk(gLabelsMu);
    if (auto it = gLabels.find(key); it != gLabels.end()) return it->second;
  }
  std::lock_guard<std::shared_mutex> lk(gLabelsMu);
  const uint32_t id = ins.recorder().intern(key);
  if (id != 0) gLabels.emplace(key, id);
  return id;
}

} // namespace

@implementation RNThreadInspector

+ (uint64_t)newFlow
{
  Inspector *ins = Inspector::shared();
  return ins ? ins->recorder().newSpanId() : 0;
}

+ (RNTISpan)begin:(NSString *)name
{
  return [self begin:name flowId:0];
}

+ (RNTISpan)begin:(NSString *)name flowId:(uint64_t)flowId
{
  Inspector *ins = Inspector::shared();
  if (!ins || !ins->recorder().enabled()) return {0, 0};
  const uint32_t id = label(*ins, name);
  return {id, ins->recorder().begin(Category::Native, id, flowId)};
}

+ (void)end:(RNTISpan)span
{
  // A span begun before start() has no Begin; ending it would record a phantom truncated span.
  if (span.spanId == 0) return;
  if (Inspector *ins = Inspector::shared()) ins->recorder().end(Category::Native, span.label, span.spanId);
}

+ (void)measure:(NSString *)name block:(NS_NOESCAPE void (^)(void))block
{
  [self measure:name flowId:0 block:block];
}

+ (void)measure:(NSString *)name flowId:(uint64_t)flowId block:(NS_NOESCAPE void (^)(void))block
{
  const RNTISpan span = [self begin:name flowId:flowId];
  @try {
    block();
  } @finally {
    [self end:span];
  }
}

+ (void)nameCurrentThread:(NSString *)name
{
  if (Inspector *ins = Inspector::shared()) ins->recorder().nameCurrentThread(name.UTF8String ?: "");
}

@end
