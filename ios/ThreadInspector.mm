#import "ThreadInspector.h"
#import "RNThreadInspector.h"

#import <UIKit/UIKit.h>
#import <ReactCommon/CallInvoker.h>
#import <ReactCommon/RCTTurboModuleWithJSIBindings.h>
#import <jsi/jsi.h>

#include "Inspector.h"
#include "Platform.h"
#include "RntiFabric.h"
#include "RntiPerfLogger.h"
#include "ThreadCpu.h"

#if __has_include(<hermes/hermes.h>)
#include <hermes/hermes.h>
#define RNTI_HAS_HERMES 1
#endif

#include <atomic>
#include <chrono>
#include <mutex>
#include <pthread.h>
#include <sys/sysctl.h>

using namespace rnti;

namespace {

std::mutex gInvokerMu;
std::shared_ptr<facebook::react::CallInvoker> gJsInvoker;
__weak ThreadInspector *gModule;
NSString *gSessionId;
std::atomic<bool> gFabricHooked{false};

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

NSUInteger gMaxSessions = 5;
// Chrome trace format, as Hermes dumps it.
NSString *const kProfileFile = @"hermes.trace.json";

NSString *sessionsDir() {
  return [incidentDir().stringByDeletingLastPathComponent stringByAppendingPathComponent:@"rnti-sessions"];
}

NSString *remoteSamplingName(NSString *cmd) {
  return [NSString stringWithFormat:@"%@.rnti.sampling.%@", NSBundle.mainBundle.bundleIdentifier ?: @"", cmd];
}

NSString *remoteSamplingFile(NSString *name) {
  return [incidentDir().stringByDeletingLastPathComponent stringByAppendingPathComponent:name];
}

// Darwin notifications carry no payload, so `rnti record` passes a request id in a file and reads the reply from another.
void writeRemoteSamplingReply(long long request, NSString *json) {
  NSString *wrapped = [NSString stringWithFormat:@"{\"request\":%lld,\"reply\":%@}", request, json];
  [[wrapped dataUsingEncoding:NSUTF8StringEncoding] writeToFile:remoteSamplingFile(@"rnti-record-reply.json") atomically:YES];
}

// Darwin notifications arrive on the main thread; sampling calls belong on the JS thread like the JS API's.
void onRemoteSampling(CFNotificationCenterRef, void *, CFNotificationName name, const void *, CFDictionaryRef) {
  const bool start = [(__bridge NSString *)name isEqualToString:remoteSamplingName(@"start")];
  NSString *requestFile = remoteSamplingFile(start ? @"rnti-record-start.request" : @"rnti-record-stop.request");
  const long long request = [[NSString stringWithContentsOfFile:requestFile encoding:NSUTF8StringEncoding error:nil] longLongValue];
  const bool queued = postToJs([start, request] {
    ThreadInspector *module = gModule;
    NSString *result = start ? [module startHermesSampling:100] : [module stopHermesSampling];
    writeRemoteSamplingReply(request, result ?: @"{\"status\":\"error\",\"reason\":\"module gone\"}");
  });
  if (!queued) writeRemoteSamplingReply(request, @"{\"status\":\"error\",\"reason\":\"no JS thread\"}");
}

// Any process on the device can post a Darwin notification, so this stays off unless the app opts in.
void setRemoteSampling(bool enabled) {
  CFNotificationCenterRef center = CFNotificationCenterGetDarwinNotifyCenter();
  for (NSString *cmd in @[ @"start", @"stop" ]) {
    CFNotificationCenterRemoveObserver(center, &gModule, (__bridge CFStringRef)remoteSamplingName(cmd), nullptr);
    if (enabled)
      CFNotificationCenterAddObserver(center, &gModule, onRemoteSampling, (__bridge CFStringRef)remoteSamplingName(cmd), nullptr,
                                      CFNotificationSuspensionBehaviorDeliverImmediately);
  }
}

// Only finished entries carry the prefix; in-progress writes use dot-prefixed temp names.
NSArray<NSString *> *entries(NSString *dir, NSString *prefix) {
  NSFileManager *fm = NSFileManager.defaultManager;
  NSMutableArray<NSDictionary *> *found = [NSMutableArray new];
  for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil] ?: @[]) {
    if (![name hasPrefix:prefix]) continue;
    NSString *p = [dir stringByAppendingPathComponent:name];
    NSDate *mod = [fm attributesOfItemAtPath:p error:nil].fileModificationDate ?: NSDate.distantPast;
    [found addObject:@{@"p" : p, @"m" : mod}];
  }
  [found sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
    return [b[@"m"] compare:a[@"m"]];
  }];
  return [found valueForKey:@"p"];
}

NSUInteger removeAll(NSArray<NSString *> *paths) {
  NSUInteger n = 0;
  for (NSString *p in paths) n += [NSFileManager.defaultManager removeItemAtPath:p error:nil] ? 1 : 0;
  return n;
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

// Pairs the monotonic recorder clock with wall time so wall-clock profiles can be mapped later.
NSDictionary *clockAnchor() {
  const uint64_t before = rnti::nowNs();
  const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
  const uint64_t after = rnti::nowNs();
  return @{@"mono_ns" : @((before + after) / 2), @"wall_us" : @(wall), @"uncertainty_ns" : @((after - before) / 2)};
}

NSString *jsonString(NSDictionary *d) {
  NSData *data = [NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
  return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

NSDictionary *gSamplingStart;
// Only touched by sampling start and stop, which run on the JS thread.
rnti::CpuSnapshot gSamplingCpu;
// Readable off the JS thread, unlike gSamplingStart.
std::atomic<bool> gSessionActive{false};

constexpr int64_t kStackWindowNs = 30 * NSEC_PER_SEC;
constexpr NSUInteger kKeptWindows = 2;
std::atomic<bool> gJsStacks{false};
std::atomic<bool> gForeground{true};
std::mutex gWindowMu;
// Guarded by gWindowMu: set while the sampler runs for `jsStacks`.
NSDictionary *gWindowStart;
dispatch_source_t gWindowTimer;

// Caches: windows are working data, copied into an incident only when one is saved.
NSString *windowsDir() {
  NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
  return [base.path stringByAppendingPathComponent:@"rnti-stacks"];
}

#if RNTI_HAS_HERMES
facebook::hermes::IHermesRootAPI *hermesRoot() {
  return facebook::jsi::castInterface<facebook::hermes::IHermesRootAPI>(facebook::hermes::makeHermesRootAPI());
}

// Runs on the JS thread; the folder gets its prefix only once it is complete.
NSString *dumpSession(facebook::hermes::IHermesRootAPI *root, NSString *parent, NSString *prefix, NSDictionary *start, id cpu) {
  NSFileManager *fm = NSFileManager.defaultManager;
  NSString *name = [NSString stringWithFormat:@"%lld", (long long)(rnti::nowNs() / 1000000)];
  NSString *tmp = [parent stringByAppendingPathComponent:[@".tmp-" stringByAppendingString:name]];
  [fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil];
  root->dumpSampledTraceToFile([tmp stringByAppendingPathComponent:kProfileFile].UTF8String);
  root->disableSamplingProfiler();
  NSDictionary *session = @{@"start" : start, @"stop" : clockAnchor(), @"profile" : kProfileFile,
                            @"profile_clock" : @"as emitted by Hermes; map with the anchors",
                            @"recorder_session_id" : gSessionId ?: (id)NSNull.null, @"pid" : @(getpid()), @"js_tid" : @(rnti::currentTid()),
                            @"cpu" : cpu ?: (id)NSNull.null, @"clock_domain" : @(rnti::clockDomainName())};
  [[jsonString(session) dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[tmp stringByAppendingPathComponent:@"session.json"] atomically:YES];
  NSString *dir = [parent stringByAppendingPathComponent:[prefix stringByAppendingString:name]];
  return [fm moveItemAtPath:tmp toPath:dir error:nil] ? dir : nil;
}
#endif

// Any thread: a stall that holds the JS thread on resume is still sampled.
void openWindow() {
#if RNTI_HAS_HERMES
  std::lock_guard<std::mutex> lk(gWindowMu);
  if (!gJsStacks || !gForeground || gSessionActive || gWindowStart) return;
  auto *root = hermesRoot();
  if (!root) return;
  NSDictionary *anchor = clockAnchor();
  root->enableSamplingProfiler(100);
  gWindowStart = anchor;
#endif
}

// JS thread. Dumping empties the sampler, so its memory stays flat.
void closeWindow() {
#if RNTI_HAS_HERMES
  std::lock_guard<std::mutex> lk(gWindowMu);
  NSDictionary *start = gWindowStart;
  auto *root = hermesRoot();
  if (!start || !root) return;
  gWindowStart = nil;
  dumpSession(root, windowsDir(), @"window-", start, nil);
  NSArray<NSString *> *windows = entries(windowsDir(), @"window-");
  if (windows.count > kKeptWindows) removeAll([windows subarrayWithRange:NSMakeRange(kKeptWindows, windows.count - kKeptWindows)]);
#endif
}

// Reopens only if still wanted, so a close that raced a start or a resume heals itself.
void rotateWindow() {
  closeWindow();
  openWindow();
}

void setJsStacks(bool enabled) {
  gJsStacks = enabled;
  {
    std::lock_guard<std::mutex> lk(gWindowMu);
    if (gWindowTimer) dispatch_source_cancel(gWindowTimer);
    gWindowTimer = nil;
    if (enabled) {
      gWindowTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
      dispatch_source_set_timer(gWindowTimer, dispatch_time(DISPATCH_TIME_NOW, kStackWindowNs), kStackWindowNs, NSEC_PER_SEC);
      dispatch_source_set_event_handler(gWindowTimer, ^{
        postToJs([] { rotateWindow(); });
      });
      dispatch_resume(gWindowTimer);
    }
  }
  if (enabled) openWindow();
  else postToJs([] { rotateWindow(); });
}

uint64_t monoNs(NSDictionary *d, NSString *outer, NSString *key) {
  id v = [d[outer] isKindOfClass:NSDictionary.class] ? d[outer][key] : nil;
  return [v isKindOfClass:NSNumber.class] ? [v unsignedLongLongValue] : 0;
}

NSDictionary *readJson(NSString *path) {
  NSData *data = [NSData dataWithContentsOfFile:path];
  id json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  return [json isKindOfClass:NSDictionary.class] ? json : nil;
}

// Copies the windows that overlap the incident into <incident>/js-stacks/, for `rnti merge`.
void attachStacks(NSString *incident) {
  NSDictionary *report = readJson([incident stringByAppendingPathComponent:@"incident.json"]);
  const uint64_t from = monoNs(report, @"window", @"start_ns");
  const uint64_t to = monoNs(report, @"window", @"capture_end_ns");
  if (!to) return;
  NSFileManager *fm = NSFileManager.defaultManager;
  NSString *stacks = [incident stringByAppendingPathComponent:@"js-stacks"];
  for (NSString *dir in entries(windowsDir(), @"window-")) {
    NSDictionary *session = readJson([dir stringByAppendingPathComponent:@"session.json"]);
    if (monoNs(session, @"start", @"mono_ns") >= to || monoNs(session, @"stop", @"mono_ns") <= from) continue;
    [fm createDirectoryAtPath:stacks withIntermediateDirectories:YES attributes:nil error:nil];
    [fm copyItemAtPath:dir toPath:[stacks stringByAppendingPathComponent:dir.lastPathComponent] error:nil];
  }
}

void onSaved(uint64_t incidentId, const std::string &path) {
  NSString *incident = [NSString stringWithUTF8String:path.c_str()];
  NSDictionary *event = @{@"id" : @(incidentId), @"path" : incident};
  dispatch_block_t emit = ^{
    dispatch_async(dispatch_get_main_queue(), ^{
      [gModule emitOnIncidentSaved:event];
    });
  };
  // The stall's samples are still in the sampler; dump them before listeners read the incident.
  const bool queued = gJsStacks && postToJs([incident, emit] {
    rotateWindow();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      attachStacks(incident);
      emit();
    });
  });
  if (!queued) emit();
}

void busyWait(double ms) {
  const uint64_t end = nowNs() + static_cast<uint64_t>(ms * kMs);
  while (nowNs() < end) {
  }
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

- (void)installJSIBindingsWithRuntime:(facebook::jsi::Runtime &)runtime
                          callInvoker:(const std::shared_ptr<facebook::react::CallInvoker> &)callInvoker
{
  gFabricHooked = installFabricObserver(runtime);
}

- (NSString *)start:(NSString *)configJson
{
  NSDictionary *c = [NSJSONSerialization JSONObjectWithData:[configJson dataUsingEncoding:NSUTF8StringEncoding]
                                                    options:0
                                                      error:nil] ?: @{};
  NSDictionary *thr = [c[@"incidentThresholdMs"] isKindOfClass:NSDictionary.class] ? c[@"incidentThresholdMs"] : @{};
  id maxSessions = c[@"maxSessions"];
  gMaxSessions = [maxSessions isKindOfClass:NSNumber.class] ? (NSUInteger)MAX(1LL, [maxSessions longLongValue]) : 5;

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
  cfg.turboModules = [c[@"turboModules"] isKindOfClass:NSNumber.class] && [c[@"turboModules"] boolValue];
  const bool wantTurboModules = cfg.turboModules;
  cfg.fabric = [c[@"fabric"] isKindOfClass:NSNumber.class] && [c[@"fabric"] boolValue];
  const bool wantFabric = cfg.fabric;
  const bool askedRemoteSampling = [c[@"remoteSampling"] isKindOfClass:NSNumber.class] && [c[@"remoteSampling"] boolValue];
  // rnti record reaches only simulators, so a device would listen with no use.
  const bool wantRemoteSampling = askedRemoteSampling && TARGET_OS_SIMULATOR;
  const bool wantJsStacks = [c[@"jsStacks"] isKindOfClass:NSNumber.class] && [c[@"jsStacks"] boolValue];

  NSString *dir = incidentDir();
  [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  cfg.writer.directory = dir.UTF8String;
  NSString *sessionId = [[NSUUID UUID].UUIDString substringToIndex:8].lowercaseString;
  cfg.meta.sessionId = sessionId.UTF8String;

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
  if (created) gSessionId = sessionId;
  ins.markJsThread();
  if (wantTurboModules) installTurboModulePerfLogger(ins);
  ins.start(true);
  installRunLoopObserver();
  setRemoteSampling(wantRemoteSampling);
  setJsStacks(wantJsStacks);

  NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
  [nc removeObserver:self];
  [nc addObserver:self selector:@selector(didBecomeActive) name:UIApplicationDidBecomeActiveNotification object:nil];
  [nc addObserver:self selector:@selector(didEnterBackground) name:UIApplicationDidEnterBackgroundNotification object:nil];
  dispatch_async(dispatch_get_main_queue(), ^{
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground) {
      gForeground = false;
      if (auto *i = Inspector::shared()) i->setForeground(false);
    }
  });

  NSDictionary *tmStatus = wantTurboModules
      ? status(@"partial", @"sync calls and async execution; dispatch is not linked to execution")
      : status(@"disabled", @"opt in with turboModules: true");
  NSDictionary *fabricStatus = !wantFabric ? status(@"disabled", @"opt in with fabric: true")
      : gFabricHooked ? status(@"partial", @"commit hook to didCommit (includes layout) and mount reports")
                      : status(@"unavailable", @"no Fabric UIManager on this runtime");
  NSDictionary *caps = @{
    @"fabricCommits" : fabricStatus,
    @"jsWatchdog" : status(@"available"),
    @"mainThread" : status(@"available"),
    @"explicitNativeSpans" : status(@"available"),
    @"asyncNativeFlows" : status(@"partial", @"explicit flow IDs required"),
    @"turboModuleIntervals" : tmStatus,
    @"reactRenderTimings" : status(@"unavailable", @"production renderer"),
    @"frameTimings" : status(@"partial", @"main run loop busy intervals only"),
#if RNTI_HAS_HERMES
    @"hermesSamples" : wantJsStacks ? status(@"available", @"sampled at 100 Hz in the foreground; paused during a sampling session")
                                    : status(@"disabled", @"opt in with jsStacks: true"),
#else
    @"hermesSamples" : status(@"unavailable", @"Hermes headers not found"),
#endif
    @"systemScheduling" : status(@"unavailable", @"no system trace attached"),
    @"remoteSampling" : wantRemoteSampling    ? status(@"available", @"any process on the simulator can post the notification")
                        : askedRemoteSampling ? status(@"unavailable", @"simulator only")
                                              : status(@"disabled", @"opt in with remoteSampling: true"),
    @"config" : created ? status(@"available") : status(@"partial", @"recorder already running; first config kept"),
    @"incidentDirectory" : dir,
  };
  NSData *out = [NSJSONSerialization dataWithJSONObject:caps options:0 error:nil];
  return [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding];
}

- (void)didBecomeActive
{
  if (auto *i = Inspector::shared()) i->setForeground(true);
  gForeground = true;
  // Off the main thread: the JS thread holds the window lock while it dumps.
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    openWindow();
  });
}

- (void)didEnterBackground
{
  if (auto *i = Inspector::shared()) i->setForeground(false);
  gForeground = false;
  postToJs([] { rotateWindow(); });
}

- (void)stop
{
  setRemoteSampling(false);
  setJsStacks(false);
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

- (NSNumber *)beginSpan:(double)labelId flowId:(double)flowId async:(BOOL)async
{
  auto *i = Inspector::shared();
  if (!i) return @0;
  return @(static_cast<double>(
      i->recorder().begin(Category::Js, static_cast<uint32_t>(labelId), static_cast<uint64_t>(flowId), async)));
}

- (NSNumber *)endSpan:(double)labelId spanId:(double)spanId async:(BOOL)async
{
  if (auto *i = Inspector::shared())
    i->recorder().end(Category::Js, static_cast<uint32_t>(labelId), static_cast<uint64_t>(spanId), 0, async);
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

- (NSNumber *)markScreen:(double)labelId
{
  if (auto *i = Inspector::shared()) i->markScreen(static_cast<uint32_t>(labelId));
  return @0;
}

- (NSString *)startHermesSampling:(double)hz
{
#if RNTI_HAS_HERMES
  auto *root = hermesRoot();
  if (!root) return jsonString(@{@"status" : @"unavailable", @"reason" : @"IHermesRootAPI not available"});
  // Set before closing the window, so none reopens under the session.
  gSessionActive = true;
  closeWindow();
  NSDictionary *anchor = clockAnchor();
  root->enableSamplingProfiler(hz > 0 ? hz : 100);
  gSamplingStart = anchor;
  gSamplingCpu = rnti::cpuSnapshot();
  if (auto *i = Inspector::shared()) i->recorder().instant(Category::Inspector, i->recorder().intern("hermes.sampling.start"));
  return jsonString(@{@"status" : @"available", @"anchor" : anchor});
#else
  return jsonString(@{@"status" : @"unavailable", @"reason" : @"Hermes headers not found"});
#endif
}

- (NSString *)stopHermesSampling
{
#if RNTI_HAS_HERMES
  if (!gSamplingStart) return jsonString(@{@"status" : @"error", @"reason" : @"not sampling"});
  auto *root = hermesRoot();
  if (!root) return jsonString(@{@"status" : @"unavailable"});
  // Read before the dump, so writing the profile is not counted as the app's work.
  const std::string cpuJson = rnti::cpuDeltaJson(gSamplingCpu, rnti::cpuSnapshot());
  id cpu = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:cpuJson.data() length:cpuJson.size()] options:0 error:nil];
  if (auto *i = Inspector::shared()) i->recorder().instant(Category::Inspector, i->recorder().intern("hermes.sampling.stop"));
  NSString *dir = dumpSession(root, sessionsDir(), @"session-", gSamplingStart, cpu);
  NSArray<NSString *> *sessions = entries(sessionsDir(), @"session-");
  NSUInteger removed = 0;
  if (sessions.count > gMaxSessions)
    removed = removeAll([sessions subarrayWithRange:NSMakeRange(gMaxSessions, sessions.count - gMaxSessions)]);
  gSamplingStart = nil;
  gSessionActive = false;
  openWindow();
  if (!dir) return jsonString(@{@"status" : @"error", @"reason" : @"cannot write the session"});
  return jsonString(@{@"status" : @"available", @"path" : dir, @"removedSessions" : @(removed)});
#else
  return jsonString(@{@"status" : @"unavailable"});
#endif
}

- (NSArray<NSString *> *)listIncidents
{
  return entries(incidentDir(), @"incident-");
}

- (NSNumber *)deleteIncident:(NSString *)path
{
  NSString *root = incidentDir().stringByResolvingSymlinksInPath;
  NSString *target = path.stringByStandardizingPath.stringByResolvingSymlinksInPath;
  BOOL inside = [target.stringByDeletingLastPathComponent isEqualToString:root] &&
                [target.lastPathComponent hasPrefix:@"incident-"];
  return @(inside && [NSFileManager.defaultManager removeItemAtPath:target error:nil]);
}

- (NSNumber *)clearData
{
  removeAll(entries(windowsDir(), @"window-"));
  return @(removeAll(entries(incidentDir(), @"incident-")) + removeAll(entries(sessionsDir(), @"session-")));
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
  [RNThreadInspector measure:@"debug.nativeBlock" block:^{ busyWait(ms); }];
  return @0;
}

- (void)simulateMainThreadBlock:(double)ms
{
  dispatch_async(dispatch_get_main_queue(), ^{
    [RNThreadInspector measure:@"debug.mainThreadBlock" block:^{ busyWait(ms); }];
  });
}

- (void)simulateAsyncNativeWork:(double)ms
                         flowId:(double)flowId
                        resolve:(RCTPromiseResolveBlock)resolve
                         reject:(RCTPromiseRejectBlock)reject
{
  const uint64_t flow = static_cast<uint64_t>(flowId);
  dispatch_async(_worker, ^{
    [RNThreadInspector nameCurrentThread:@"rnti.demo-worker"];
    [RNThreadInspector measure:@"debug.asyncWork" flowId:flow block:^{ [NSThread sleepForTimeInterval:ms / 1000.0]; }];
    resolve(nil);
  });
}

- (void)invalidate
{
  setRemoteSampling(false);
  setJsStacks(false);
  [NSNotificationCenter.defaultCenter removeObserver:self];
  std::lock_guard<std::mutex> lk(gInvokerMu);
  gJsInvoker.reset();
}

@end
