#include <jni.h>

#include "IncidentController.h"
#include "IncidentWriter.h"
#include "Platform.h"
#include "Recorder.h"
#include "Watchdog.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

using namespace rnti;

namespace {

JavaVM *gVm = nullptr;
jclass gBridge = nullptr;
jmethodID gPostProbe = nullptr;
jmethodID gOnSaved = nullptr;

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

struct PendingAcks {
  std::mutex mu;
  std::deque<std::pair<uint64_t, std::function<void()>>> queue;
  uint64_t next = 1;
  std::atomic<bool> threadNamed{false};
};

struct Inspector {
  Recorder recorder;
  std::unique_ptr<IncidentWriter> writer;
  IncidentController controller;
  Watchdog watchdog;
  PendingAcks acks[2];

  Inspector(RecorderConfig rc, IncidentConfig ic, WriterConfig wc, ExportMetadata meta)
      : recorder(rc),
        writer(std::make_unique<IncidentWriter>(recorder, std::move(wc), std::move(meta), &Inspector::saved)),
        controller(recorder, ic, [this](Incident &&i) { writer->enqueue(std::move(i)); }),
        watchdog(recorder, controller) {}

  static void saved(uint64_t id, const std::string &path) {
    JNIEnv *env = jniEnv();
    jstring jpath = env->NewStringUTF(path.c_str());
    env->CallStaticVoidMethod(gBridge, gOnSaved, static_cast<jlong>(id), jpath);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(jpath);
  }

  std::function<bool(std::function<void()>)> poster(int channel) {
    return [this, channel](std::function<void()> ack) {
      uint64_t token;
      {
        auto &p = acks[channel];
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

  void ack(int channel, uint64_t token) {
    auto &p = acks[channel];
    if (!p.threadNamed.exchange(true)) recorder.nameCurrentThread(channel == kJsChannel ? "RN JS" : "main");
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lk(p.mu);
      for (auto it = p.queue.begin(); it != p.queue.end(); ++it) {
        if (it->first == token) {
          fn = std::move(it->second);
          p.queue.erase(it);
          break;
        }
      }
    }
    if (fn) fn();
  }
};

// Created once and never freed: RN and app threads may record at any time.
std::atomic<Inspector *> gInspector{nullptr};
std::mutex gStartMu;

Inspector *inspector() { return gInspector.load(std::memory_order_acquire); }

std::string toString(JNIEnv *env, jstring s) {
  if (!s) return {};
  const char *chars = env->GetStringUTFChars(s, nullptr);
  std::string out(chars);
  env->ReleaseStringUTFChars(s, chars);
  return out;
}

} // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
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
  std::lock_guard<std::mutex> lk(gStartMu);
  Inspector *existing = inspector();
  if (existing) {
    existing->recorder.setEnabled(true);
    existing->watchdog.start();
    return JNI_FALSE;
  }

  jlong c[11];
  env->GetLongArrayRegion(jconfig, 0, 11, c);
  RecorderConfig rc;
  rc.eventsPerThread = static_cast<uint32_t>(c[9]);
  IncidentConfig ic;
  ic.prehistoryNs = c[0] * kMs;
  ic.tailNs = c[4] * kMs;
  ic.maxIncidentNs = c[5] * kMs;
  ic.cooldownNs = c[6] * kMs;
  ic.longTaskThresholdNs = c[7] * kMs;
  WriterConfig wc;
  wc.directory = toString(env, jdir);
  wc.maxIncidents = static_cast<uint32_t>(c[8]);
  ExportMetadata meta;
  meta.sessionId = toString(env, jsession);
  const jsize n = env->GetArrayLength(metaKeys);
  for (jsize i = 0; i < n; ++i) {
    auto k = static_cast<jstring>(env->GetObjectArrayElement(metaKeys, i));
    auto v = static_cast<jstring>(env->GetObjectArrayElement(metaValues, i));
    meta.fields[toString(env, k)] = toString(env, v);
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
  }

  auto *ins = new Inspector(rc, ic, std::move(wc), std::move(meta));
  ins->watchdog.addChannel({.name = "js", .source = TriggerSource::JsWatchdog, .post = ins->poster(kJsChannel),
                            .intervalNs = static_cast<uint64_t>(c[3]) * kMs,
                            .thresholdNs = static_cast<uint64_t>(c[1]) * kMs});
  ins->watchdog.addChannel({.name = "main", .source = TriggerSource::MainWatchdog,
                            .post = ins->poster(kMainChannel),
                            .intervalNs = static_cast<uint64_t>(c[3]) * kMs,
                            .thresholdNs = static_cast<uint64_t>(c[2]) * kMs});
  ins->watchdog.setForeground(c[10] != 0);
  gInspector.store(ins, std::memory_order_release);
  ins->watchdog.start();
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeStop(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lk(gStartMu);
  if (auto *i = inspector()) {
    i->watchdog.stop();
    i->controller.disable(nowNs());
    i->recorder.setEnabled(false);
  }
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeSetForeground(JNIEnv *, jclass, jboolean fg) {
  if (auto *i = inspector()) i->watchdog.setForeground(fg == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeAck(JNIEnv *, jclass, jint channel, jlong token) {
  if (auto *i = inspector(); i && (channel == kJsChannel || channel == kMainChannel))
    i->ack(channel, static_cast<uint64_t>(token));
}

JNIEXPORT jlong JNICALL Java_com_threadinspector_NativeBridge_nativeNowNs(JNIEnv *, jclass) {
  return static_cast<jlong>(nowNs());
}

JNIEXPORT jint JNICALL Java_com_threadinspector_NativeBridge_nativeIntern(JNIEnv *env, jclass, jstring label) {
  auto *i = inspector();
  return i ? static_cast<jint>(i->recorder.intern(toString(env, label))) : 0;
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeNameThread(JNIEnv *env, jclass, jstring name) {
  if (auto *i = inspector()) i->recorder.nameCurrentThread(toString(env, name));
}

JNIEXPORT jlong JNICALL Java_com_threadinspector_NativeBridge_nativeNewFlowId(JNIEnv *, jclass) {
  auto *i = inspector();
  return i ? static_cast<jlong>(i->recorder.newSpanId()) : 0;
}

JNIEXPORT jlong JNICALL Java_com_threadinspector_NativeBridge_nativeBegin(JNIEnv *, jclass, jint category, jint label,
                                                                          jlong flow) {
  auto *i = inspector();
  return i ? static_cast<jlong>(i->recorder.begin(static_cast<Category>(category), label, flow)) : 0;
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeEnd(JNIEnv *, jclass, jint category, jint label,
                                                                       jlong span) {
  if (auto *i = inspector()) i->recorder.end(static_cast<Category>(category), label, span);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeInstant(JNIEnv *, jclass, jint category,
                                                                           jint label, jlong arg) {
  if (auto *i = inspector()) i->recorder.instant(static_cast<Category>(category), label, arg);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeComplete(JNIEnv *, jclass, jint category,
                                                                            jint label, jlong start, jlong dur,
                                                                            jint eventFlags, jlong flow) {
  if (auto *i = inspector())
    i->recorder.complete(static_cast<Category>(category), label, start, dur, static_cast<uint16_t>(eventFlags), flow);
}

JNIEXPORT void JNICALL Java_com_threadinspector_NativeBridge_nativeLongTask(JNIEnv *, jclass, jlong start,
                                                                            jlong dur) {
  auto *i = inspector();
  if (!i || start <= 0 || dur <= 0) return;
  static const uint32_t label = i->recorder.intern("js.long_task");
  i->recorder.complete(Category::Js, label, start, dur, flags::Approximate);
  i->controller.longTaskCompleted(currentTid(), start, dur, nowNs());
}

} // extern "C"
