#import "RNThreadInspector.h"

#include "Inspector.h"

#include <mutex>
#include <string>
#include <unordered_map>

using namespace rnti;

namespace {

std::mutex gLabelsMu;
std::unordered_map<std::string, uint32_t> gLabels;

// Recorder::intern is not for hot paths, so labels are cached; the inspector is never torn down.
uint32_t label(Inspector &ins, NSString *name) {
  const std::string key = name.UTF8String ?: "";
  std::lock_guard<std::mutex> lk(gLabelsMu);
  auto it = gLabels.find(key);
  if (it != gLabels.end()) return it->second;
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
  if (!ins) return {0, 0};
  const uint32_t id = label(*ins, name);
  return {id, ins->recorder().begin(Category::Native, id, flowId)};
}

+ (void)end:(RNTISpan)span
{
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
