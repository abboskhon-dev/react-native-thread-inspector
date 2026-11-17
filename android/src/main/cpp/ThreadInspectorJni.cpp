#include <jni.h>

#include <ReactCommon/BindingsInstallerHolder.h>
#include <fbjni/fbjni.h>

#include "Inspector.h"
#include "Platform.h"
#include "RntiFabric.h"
#include "RntiPerfLogger.h"
#include "ThreadCpu.h"

#include <atomic>

#include <deque>
#include <functional>
#include <mutex>
#include <string>

using namespace rnti;

namespace {

JavaVM *gVm = nullptr;
jclass gBridge = nullptr;
jmethodID gPostProbe = nullptr;
jmethodID gOnSaved = nullptr;
std::atomic<bool> gFabricHooked{false};

constexpr int kJsChannel = 0;
constexpr int kMainChannel = 1;

struct ThreadEnv {
  JNIEnv *env = nullptr;
  bool attached = false;
  ~ThreadEnv() {
    if (attached) gVm->DetachCurrentThread();
  }
};

JNIEnv *jniEnv() {
  thread_local ThreadEnv t;
  if (!t.env && gVm->GetEnv(reinterpret_cast<void **>(&t.env), JNI_VERSION_1_6) != JNI_OK) {
    gVm->AttachCurrentThread(&t.env, nullptr);
    t.attached = true;
  }
  return t.env;
}

// Java runnables carry a token back; the ack closure stays native.
struct PendingAcks {
  std::mutex mu;
  std::deque<std::pair<uint64_t, std::function<void()>>> queue;
  uint64_t next = 1;
};
PendingAcks gAcks[2];

Inspector::Poster javaPoster(int channel) {
  return [channel](std::function<void()> ack) {
    uint64_t token;
    {
      auto &p = gAcks[channel];
      std::lock_guard<std::mutex> lk(p.mu);
      token = p.next++;
      p.queue.emplace_back(token, std::move(ack));
      while (p.queue.size() > 8) p.queue.pop_front();
    }
    JNIEnv *env = jniEnv();
    const jboolean ok = env->CallStaticBooleanMethod(gBridge, gPostProbe, channel, static_cast<jlong>(token));
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      return false;
    }
    return ok == JNI_TRUE;
  };
}

void onSaved(uint64_t id, const std::string &path) {
  JNIEnv *env = jniEnv();
  jstring jpath = env->NewStringUTF(path.c_str());
  env->CallStaticVoidMethod(gBridge, gOnSaved, static_cast<jlong>(id), jpath);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(jpath);
}

std::string toString(JNIEnv *env, jstring s) {
  if (!s) return {};
  const char *chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars);
  env->ReleaseStringUTFChars(s, chars);
  return out;
}

} // namespace

// Only touched by sampling start and stop, which run on the JS thread.
static CpuSnapshot gCpuMark;

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
  facebook::jni::initialize(vm, [] {});
  gVm = vm;
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass local = env->FindClass("com/threadinspector/NativeBridge");
  if (!local) return JNI_ERR;
  gBridge = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  gPostProbe = env->GetStaticMethodID(gBridge, "postProbe", "(IJ)Z");
  gOnSaved = env->GetStaticMethodID(gBridge, "onIncidentSaved", "(JLjava/lang/String;)V");
  return gPostProbe && gOnSaved ? JNI_VERSION_1_6 : JNI_ERR;
}

JNIEXPORT jboolean JNICALL Java_com_threadinspector_NativeBridge_nativeStart(
    JNIEnv *env, jclass, jlongArray jconfig, jstring jdir, jstring jsession, jobjectArray metaKeys,
    jobjectArray metaValues) {
  jlong c[13];
  env->GetLongArrayRegion(jconfig, 0, 13, c);
  InspectorConfig cfg;
  cfg.incident.prehistoryNs = c[0] * kMs;
  cfg.jsThresholdNs = c[1] * kMs;
  cfg.mainThresholdNs = c[2] * kMs;
  cfg.probeIntervalNs = c[3] * kMs;
  cfg.incident.tailNs = c[4] * kMs;
  cfg.incident.maxIncidentNs = c[5] * kMs;
  cfg.incident.cooldownNs = c[6] * kMs;
  cfg.incident.longTaskThresholdNs = c[7] * kMs;
  cfg.writer.maxIncidents = static_cast<uint32_t>(c[8]);
  cfg.recorder.eventsPerThread = static_cast<uint32_t>(c[9]);
  cfg.turboModules = c[11] != 0;
  cfg.fabric = c[12] != 0;
  cfg.writer.directory = toString(env, jdir);
  cfg.meta.sessionId = toString(env, jsession);
  const jsize n = env->GetArrayLength(metaKeys);
  for (jsize i = 0; i < n; ++i) {
    auto k = static_cast<jstring>(env->GetObjectArrayElement(metaKeys, i));
    auto v = static_cast<jstring>(env->GetObjectArrayElement(metaValues, i));
    cfg.meta.fields[toString(env, k)] = toString(env, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  }

  bool created = false;
  Inspector &ins = Inspector::install(std::move(cfg), javaPoster(kJsChannel), javaPoster(kMainChannel), onSaved, created);
  ins.markJsThread();
  if (c[11] != 0) installTurboModulePerfLogger(ins);
  ins.start(c[10] != 0);
  return created ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeStop(JNIEnv *, jclass) {
  if (auto *i = Inspector::shared()) i->stop();
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeSetForeground(JNIEnv *, jclass, jboolean fg) {
  if (auto *i = Inspector::shared()) i->setForeground(fg == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeAck(JNIEnv *, jclass, jint channel, jlong token) {
  if (channel != kJsChannel && channel != kMainChannel) return;
  std::function<void()> fn;
  {
    auto &p = gAcks[channel];
    std::lock_guard<std::mutex> lk(p.mu);
    for (auto it = p.queue.begin(); it != p.queue.end(); ++it) {
      if (it->first == static_cast<uint64_t>(token)) {
        fn = std::move(it->second);
        p.queue.erase(it);
        break;
      }
    }
  }
  if (fn) fn();
}

JNIEXPORT jlong JNICALL Java_com_threadinspector_NativeBridge_nativeNowNs(JNIEnv *, jclass) {
  return static_cast<jlong>(nowNs());
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeCpuMark(JNIEnv *, jclass) {
  gCpuMark = cpuSnapshot();
}

// Bytes, not a jstring: thread names are not guaranteed to be valid Modified UTF-8.
JNIEXPORT jbyteArray JNICALL Java_com_threadinspector_NativeBridge_nativeCpuSinceMark(JNIEnv *env, jclass) {
  const std::string json = cpuDeltaJson(gCpuMark, cpuSnapshot());
  jbyteArray out = env->NewByteArray(static_cast<jsize>(json.size()));
  if (out) env->SetByteArrayRegion(out, 0, static_cast<jsize>(json.size()), reinterpret_cast<const jbyte *>(json.data()));
  return out;
}

JNIEXPORT jint JNICALL Java_com_threadinspector_NativeBridge_nativeIntern(JNIEnv *env, jclass, jstring label) {
  auto *i = Inspector::shared();
  return i ? static_cast<jint>(i->recorder().intern(toString(env, label))) : 0;
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeNameThread(JNIEnv *env, jclass, jstring name) {
  if (auto *i = Inspector::shared()) i->recorder().nameCurrentThread(toString(env, name));
}

JNIEXPORT jlong JNICALL Java_com_threadinspector_NativeBridge_nativeNewFlowId(JNIEnv *, jclass) {
  auto *i = Inspector::shared();
  return i ? static_cast<jlong>(i->recorder().newSpanId()) : 0;
}

JNIEXPORT jlong JNICALL Java_com_threadinspector_NativeBridge_nativeBegin(JNIEnv *, jclass, jint category, jint label,
                                                                          jlong flow, jboolean async) {
  auto *i = Inspector::shared();
  return i ? static_cast<jlong>(i->recorder().begin(static_cast<Category>(category), label, flow, async)) : 0;
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeEnd(JNIEnv *, jclass, jint category, jint label,
                                                                       jlong span, jboolean async) {
  if (auto *i = Inspector::shared()) i->recorder().end(static_cast<Category>(category), label, span, 0, async);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeInstant(JNIEnv *, jclass, jint category,
                                                                           jint label, jlong arg) {
  if (auto *i = Inspector::shared()) i->recorder().instant(static_cast<Category>(category), label, arg);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeComplete(JNIEnv *, jclass, jint category,
                                                                            jint label, jlong start, jlong dur,
                                                                            jint eventFlags, jlong flow) {
  if (auto *i = Inspector::shared())
    i->recorder().complete(static_cast<Category>(category), label, start, dur, static_cast<uint16_t>(eventFlags), flow);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeLongTask(JNIEnv *, jclass, jlong start,
                                                                            jlong dur) {
  if (auto *i = Inspector::shared(); i && start > 0 && dur > 0) i->reportLongTask(start, dur);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeMarkScreen(JNIEnv *, jclass, jint label) {
  if (auto *i = Inspector::shared()) i->markScreen(static_cast<uint32_t>(label));
}

JNIEXPORT jboolean JNICALL Java_com_threadinspector_NativeBridge_nativeFabricHooked(JNIEnv *, jclass) {
  return gFabricHooked.load() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobject JNICALL Java_com_threadinspector_ThreadInspectorModule_getBindingsInstaller(JNIEnv *, jobject) {
  using facebook::react::BindingsInstallerHolder;
  auto holder = BindingsInstallerHolder::newObjectCxxArgs(
      [](facebook::jsi::Runtime &rt, const std::shared_ptr<facebook::react::CallInvoker> &) {
        gFabricHooked = installFabricObserver(rt);
      });
  return holder.release();
}

} // extern "C"
