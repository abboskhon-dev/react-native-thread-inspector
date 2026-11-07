#import "ThreadInspector.h"

#import <UIKit/UIKit.h>
#import <ReactCommon/CallInvoker.h>
#import <jsi/jsi.h>

#include "Inspector.h"
#include "Platform.h"

#include <mutex>
#include <pthread.h>
#include <sys/sysctl.h>

using namespace rnti;

namespace {

std::mutex gInvokerMu;
std::shared_ptr<facebook::react::CallInvoker> gJsInvoker;
__weak ThreadInspector *gModule;

constexpr uint64_t kRunLoopReportNs = 32 * kMs;

bool postToJs(std::function<void()> ack) {
  std::shared_ptr<facebook::react::CallInvoker> invoker;
  {
    std::lock_guard<std::mutex> lk(gInvokerMu);
    invoker = gJsInvoker;
  }
  if (!invoker) return false;
  invoker->invokeAsync([ack = std::move(ack)](facebook::jsi::Runtime &) { ack(); });
  return true;
}

bool postToMain(std::function<void()> ack) {
  dispatch_async(dispatch_get_main_queue(), ^{
    ack();
  });
  return true;
}

void onSaved(uint64_t incidentId, const std::string &path) {
  NSDictionary *event = @{@"id" : @(incidentId), @"path" : [NSString stringWithUTF8String:path.c_str()]};
  dispatch_async(dispatch_get_main_queue(), ^{
    [gModule emitOnIncidentSaved:event];
  });
}

// Main run loop busy intervals: afterWaiting -> beforeWaiting, an envelope around whatever the loop serviced.
void installRunLoopObserver() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    dispatch_async(dispatch_get_main_queue(), ^{
      __block uint64_t busySince = 0;
      __block uint32_t label = 0;
      CFRunLoopObserverRef obs = CFRunLoopObserverCreateWithHandler(
          kCFAllocatorDefault, kCFRunLoopAfterWaiting | kCFRunLoopBeforeWaiting, true, INT_MIN,
          ^(CFRunLoopObserverRef, CFRunLoopActivity activity) {
            Inspector *ins = Inspector::shared();
            if (!ins) return;
            const uint64_t now = nowNs();
            if (activity == kCFRunLoopAfterWaiting) {
              busySince = now;
              return;
            }
            if (busySince && now - busySince >= kRunLoopReportNs) {
              if (!label) label = ins->recorder().intern("main.runloop_busy");
              ins->recorder().complete(Category::MainThread, label, busySince, now - busySince,
                                       flags::Envelope | flags::Approximate);
            }
            busySince = 0;
          });
      CFRunLoopAddObserver(CFRunLoopGetMain(), obs, kCFRunLoopCommonModes);
    });
  });
}

NSString *incidentDir() {
  NSURL *base = [[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                      inDomains:NSUserDomainMask].firstObject;
  return [base.path stringByAppendingPathComponent:@"rnti-incidents"];
}

NSString *deviceModel() {
  NSString *sim = NSProcessInfo.processInfo.environment[@"SIMULATOR_MODEL_IDENTIFIER"];
  if (sim) return sim;
  char buf[64] = {0};
  size_t len = sizeof(buf);
  sysctlbyname("hw.machine", buf, &len, nullptr, 0);
  return [NSString stringWithUTF8String:buf];
}

uint64_t msField(NSDictionary *d, NSString *key, uint64_t fallback) {
  id v = d[key];
  return [v isKindOfClass:NSNumber.class] ? [v unsignedLongLongValue] : fallback;
}

NSDictionary *status(NSString *s, NSString *reason = nil) {
  return reason ? @{@"status" : s, @"reason" : reason} : @{@"status" : s};
}

void busyWait(double ms) {
  const uint64_t end = nowNs() + static_cast<uint64_t>(ms * kMs);
  while (nowNs() < end) {
  }
}

template <typename F>
void measure(Category c, const char *name, uint64_t flow, F &&fn) {
  Inspector *ins = Inspector::shared();
  if (!ins) return fn();
  const uint32_t label = ins->recorder().intern(name);
  const uint64_t span = ins->recorder().begin(c, label, flow);
  fn();
  ins->recorder().end(c, label, span);
}

} // namespace

@implementation ThreadInspector {
  dispatch_queue_t _worker;
}

+ (NSString *)moduleName
{
  return @"ThreadInspector";
}

- (instancetype)init
{
  if (self = [super init]) {
    _worker = dispatch_queue_create("rnti.demo-worker", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params
{
  {
    std::lock_guard<std::mutex> lk(gInvokerMu);
    gJsInvoker = params.jsInvoker;
  }
  return std::make_shared<facebook::react::NativeThreadInspectorSpecJSI>(params);
}

- (NSString *)start:(NSString *)configJson
{
  NSDictionary *c = [NSJSONSerialization JSONObjectWithData:[configJson dataUsingEncoding:NSUTF8StringEncoding]
                                                    options:0
                                                      error:nil] ?: @{};
  NSDictionary *thr = [c[@"incidentThresholdMs"] isKindOfClass:NSDictionary.class] ? c[@"incidentThresholdMs"] : @{};

  InspectorConfig cfg;
  cfg.incident.prehistoryNs = msField(c, @"historyMs", 5000) * kMs;
  cfg.jsThresholdNs = msField(thr, @"js", 200) * kMs;
  cfg.mainThresholdNs = msField(thr, @"main", 200) * kMs;
  cfg.probeIntervalNs = msField(c, @"probeIntervalMs", 100) * kMs;
  cfg.incident.tailNs = msField(c, @"tailAfterRecoveryMs", 1000) * kMs;
  cfg.incident.maxIncidentNs = msField(c, @"maxIncidentMs", 10000) * kMs;
  cfg.incident.cooldownNs = msField(c, @"cooldownMs", 30000) * kMs;
  cfg.incident.longTaskThresholdNs = msField(c, @"longTaskThresholdMs", 200) * kMs;
  cfg.writer.maxIncidents = static_cast<uint32_t>(msField(c, @"maxIncidents", 20));
  cfg.recorder.eventsPerThread = static_cast<uint32_t>(msField(c, @"bufferEventsPerThread", 2048));

  NSString *dir = incidentDir();
  [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  cfg.writer.directory = dir.UTF8String;
  cfg.meta.sessionId = [[NSUUID UUID].UUIDString substringToIndex:8].lowercaseString.UTF8String;

  NSProcessInfo *pi = NSProcessInfo.processInfo;
  NSOperatingSystemVersion v = pi.operatingSystemVersion;
  auto &m = cfg.meta.fields;
  m["platform"] = "ios";
  m["os_version"] = [NSString stringWithFormat:@"%ld.%ld.%ld", (long)v.majorVersion, (long)v.minorVersion,
                                               (long)v.patchVersion].UTF8String;
  m["device"] = deviceModel().UTF8String;
  m["simulator"] = pi.environment[@"SIMULATOR_MODEL_IDENTIFIER"] ? "true" : "false";
#if DEBUG
  m["app_debug_build"] = "true";
#else
  m["app_debug_build"] = "false";
#endif
  m["app_bundle"] = (NSBundle.mainBundle.bundleIdentifier ?: @"").UTF8String;
  m["js_probe"] = "CallInvoker::invokeAsync acknowledgement";
  m["main_probe"] = "dispatch_async(main queue) acknowledgement";

  gModule = self;
  bool created = false;
  Inspector &ins = Inspector::install(std::move(cfg), postToJs, postToMain, onSaved, created);
  ins.markJsThread();
  ins.start(true);
  installRunLoopObserver();

  NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
  [nc removeObserver:self];
  [nc addObserver:self selector:@selector(didBecomeActive) name:UIApplicationDidBecomeActiveNotification object:nil];
  [nc addObserver:self selector:@selector(didEnterBackground) name:UIApplicationDidEnterBackgroundNotification object:nil];
  dispatch_async(dispatch_get_main_queue(), ^{
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground) {
      if (auto *i = Inspector::shared()) i->setForeground(false);
    }
  });

  NSDictionary *caps = @{
    @"jsWatchdog" : status(@"available"),
    @"mainThread" : status(@"available"),
    @"explicitNativeSpans" : status(@"available"),
    @"asyncNativeFlows" : status(@"partial", @"explicit flow IDs required"),
    @"turboModuleIntervals" : status(@"disabled", @"automatic adapter not implemented"),
    @"reactRenderTimings" : status(@"unavailable", @"production renderer"),
    @"frameTimings" : status(@"partial", @"main run loop busy intervals only"),
    @"hermesSamples" : status(@"disabled"),
    @"systemScheduling" : status(@"unavailable", @"no system trace attached"),
    @"config" : created ? status(@"available") : status(@"partial", @"recorder already running; first config kept"),
    @"incidentDirectory" : dir,
  };
  NSData *out = [NSJSONSerialization dataWithJSONObject:caps options:0 error:nil];
  return [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];
}

- (void)didBecomeActive
{
  if (auto *i = Inspector::shared()) i->setForeground(true);
}

- (void)didEnterBackground
{
  if (auto *i = Inspector::shared()) i->setForeground(false);
}

- (void)stop
{
  if (auto *i = Inspector::shared()) i->stop();
}

- (NSNumber *)nowNs
{
  return @(static_cast<double>(rnti::nowNs()));
}

- (NSNumber *)intern:(NSString *)label
{
  auto *i = Inspector::shared();
  return @(i ? i->recorder().intern(label.UTF8String) : 0);
}

- (NSNumber *)newFlowId
{
  auto *i = Inspector::shared();
  return @(i ? static_cast<double>(i->recorder().newSpanId()) : 0);
}

- (NSNumber *)beginSpan:(double)labelId flowId:(double)flowId
{
  auto *i = Inspector::shared();
  if (!i) return @0;
  return @(static_cast<double>(
      i->recorder().begin(Category::Js, static_cast<uint32_t>(labelId), static_cast<uint64_t>(flowId))));
}

- (NSNumber *)endSpan:(double)labelId spanId:(double)spanId
{
  if (auto *i = Inspector::shared())
    i->recorder().end(Category::Js, static_cast<uint32_t>(labelId), static_cast<uint64_t>(spanId));
  return @0;
}

- (NSNumber *)instant:(double)labelId arg:(double)arg
{
  if (auto *i = Inspector::shared())
    i->recorder().instant(Category::User, static_cast<uint32_t>(labelId), static_cast<uint64_t>(arg));
  return @0;
}

- (NSNumber *)completeSpan:(double)labelId startNs:(double)startNs durationNs:(double)durationNs flags:(double)f
{
  if (auto *i = Inspector::shared())
    i->recorder().complete(Category::Js, static_cast<uint32_t>(labelId), static_cast<uint64_t>(startNs),
                           static_cast<uint64_t>(durationNs), static_cast<uint16_t>(f));
  return @0;
}

- (NSNumber *)reportLongTask:(double)startNs durationNs:(double)durationNs
{
  if (auto *i = Inspector::shared(); i && startNs > 0 && durationNs > 0)
    i->reportLongTask(static_cast<uint64_t>(startNs), static_cast<uint64_t>(durationNs));
  return @0;
}

- (NSArray<NSString *> *)listIncidents
{
  NSString *dir = incidentDir();
  NSFileManager *fm = NSFileManager.defaultManager;
  NSMutableArray<NSDictionary *> *found = [NSMutableArray new];
  for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil] ?: @[]) {
    if (![name hasPrefix:@"incident-"]) continue;
    NSString *p = [dir stringByAppendingPathComponent:name];
    NSDate *mod = [fm attributesOfItemAtPath:p error:nil].fileModificationDate ?: NSDate.distantPast;
    [found addObject:@{@"p" : p, @"m" : mod}];
  }
  [found sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
    return [b[@"m"] compare:a[@"m"]];
  }];
  return [found valueForKey:@"p"];
}

- (NSString *)readIncidentFile:(NSString *)path file:(NSString *)file
{
  NSString *root = incidentDir().stringByResolvingSymlinksInPath;
  NSString *target = [path stringByAppendingPathComponent:file].stringByStandardizingPath.stringByResolvingSymlinksInPath;
  if (![target hasPrefix:[root stringByAppendingString:@"/"]]) {
    @throw [NSException exceptionWithName:@"RNTIPathError" reason:@"path outside incident directory" userInfo:nil];
  }
  return [NSString stringWithContentsOfFile:target encoding:NSUTF8StringEncoding error:nil] ?: @"";
}

- (NSNumber *)simulateNativeBlock:(double)ms
{
  measure(Category::Native, "debug.nativeBlock", 0, [ms] { busyWait(ms); });
  return @0;
}

- (void)simulateMainThreadBlock:(double)ms
{
  dispatch_async(dispatch_get_main_queue(), ^{
    measure(Category::Native, "debug.mainThreadBlock", 0, [ms] { busyWait(ms); });
  });
}

- (void)simulateAsyncNativeWork:(double)ms
                         flowId:(double)flowId
                        resolve:(RCTPromiseResolveBlock)resolve
                         reject:(RCTPromiseRejectBlock)reject
{
  const uint64_t flow = static_cast<uint64_t>(flowId);
  dispatch_async(_worker, ^{
    if (auto *i = Inspector::shared()) i->recorder().nameCurrentThread("rnti.demo-worker");
    measure(Category::Native, "debug.asyncWork", flow, [ms] { [NSThread sleepForTimeInterval:ms / 1000.0]; });
    resolve(nil);
  });
}

- (void)invalidate
{
  [NSNotificationCenter.defaultCenter removeObserver:self];
  std::lock_guard<std::mutex> lk(gInvokerMu);
  gJsInvoker.reset();
}

@end
