package com.threadinspector

import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.facebook.hermes.instrumentation.HermesSamplingProfiler
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.WritableArray
import com.facebook.react.turbomodule.core.interfaces.BindingsInstallerHolder
import com.facebook.react.turbomodule.core.interfaces.TurboModuleWithJSIBindings
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors

class ThreadInspectorModule(reactContext: ReactApplicationContext) :
  NativeThreadInspectorSpec(reactContext), LifecycleEventListener, TurboModuleWithJSIBindings {

  external override fun getBindingsInstaller(): BindingsInstallerHolder

  private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "rnti.demo-worker") }

  private fun incidentDir(): File =
    File(reactApplicationContext.getExternalFilesDir(null) ?: reactApplicationContext.filesDir, "rnti-incidents")

  override fun start(configJson: String): String {
    val c = JSONObject(configJson)
    val threshold = c.optJSONObject("incidentThresholdMs")
    maxSessions = c.optInt("maxSessions", DEFAULT_MAX_SESSIONS).coerceAtLeast(1)
    val config = longArrayOf(
      c.optLong("historyMs", 5_000),
      threshold?.optLong("js", 200) ?: 200,
      threshold?.optLong("main", 200) ?: 200,
      c.optLong("probeIntervalMs", 100),
      c.optLong("tailAfterRecoveryMs", 1_000),
      c.optLong("maxIncidentMs", 10_000),
      c.optLong("cooldownMs", 30_000),
      c.optLong("longTaskThresholdMs", 200),
      c.optLong("maxIncidents", 20),
      c.optLong("bufferEventsPerThread", 2_048),
      1L,
      if (c.optBoolean("turboModules", false)) 1L else 0L,
      if (c.optBoolean("fabric", false)) 1L else 0L,
    )
    val meta = linkedMapOf(
      "platform" to "android",
      "os_version" to Build.VERSION.RELEASE,
      "api_level" to Build.VERSION.SDK_INT.toString(),
      "device" to "${Build.MANUFACTURER} ${Build.MODEL}",
      "app_debuggable" to ((reactApplicationContext.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0).toString(),
      "app_package" to reactApplicationContext.packageName,
      "js_probe" to "ReactContext.runOnJSQueueThread acknowledgement",
      "main_probe" to "main Looper Handler.post acknowledgement",
    )

    val context = reactApplicationContext
    NativeBridge.jsPoster = { r -> context.runOnJSQueueThread(r) }
    NativeBridge.savedListener = { id, path ->
      val event = Arguments.createMap().apply {
        putDouble("id", id.toDouble())
        putString("path", path)
      }
      emitOnIncidentSaved(event)
    }
    val dir = incidentDir().apply { mkdirs() }
    val sessionId = UUID.randomUUID().toString().substring(0, 8)
    val fresh = NativeBridge.nativeStart(
      config, dir.absolutePath, sessionId,
      meta.keys.toTypedArray(), meta.values.toTypedArray(),
    )
    if (fresh) recorderSessionId = sessionId
    context.removeLifecycleEventListener(this)
    context.addLifecycleEventListener(this)

    return JSONObject().apply {
      put("jsWatchdog", status("available"))
      put("mainThread", status("available"))
      put("explicitNativeSpans", status("available"))
      put("asyncNativeFlows", status("partial", "explicit flow IDs required"))
      put(
        "turboModuleIntervals",
        if (c.optBoolean("turboModules", false)) status("partial", "sync calls and async execution; dispatch is not linked to execution")
        else status("disabled", "opt in with turboModules: true"),
      )
      put(
        "fabricCommits",
        when {
          !c.optBoolean("fabric", false) -> status("disabled", "opt in with fabric: true")
          NativeBridge.nativeFabricHooked() -> status("partial", "commit hook to didCommit (includes layout) and mount reports")
          else -> status("unavailable", "no Fabric UIManager on this runtime")
        },
      )
      put("reactRenderTimings", status("unavailable", "production renderer"))
      put("frameTimings", status("unavailable", "not implemented in this build"))
      put("hermesSamples", status("disabled"))
      put("systemScheduling", status("unavailable", "no system trace attached"))
      put("config", status(if (fresh) "available" else "partial", if (fresh) null else "recorder already running; first config kept"))
      put("incidentDirectory", dir.absolutePath)
    }.toString()
  }

  private fun status(s: String, reason: String? = null) =
    JSONObject().apply {
      put("status", s)
      if (reason != null) put("reason", reason)
    }

  override fun stop() = NativeBridge.nativeStop()

  override fun nowNs(): Double = NativeBridge.nativeNowNs().toDouble()

  override fun intern(label: String): Double = NativeBridge.nativeIntern(label).toDouble()

  override fun newFlowId(): Double = NativeBridge.nativeNewFlowId().toDouble()

  override fun beginSpan(labelId: Double, flowId: Double, async: Boolean): Double =
    NativeBridge.nativeBegin(NativeBridge.CATEGORY_JS, labelId.toInt(), flowId.toLong(), async).toDouble()

  override fun endSpan(labelId: Double, spanId: Double, async: Boolean): Double {
    NativeBridge.nativeEnd(NativeBridge.CATEGORY_JS, labelId.toInt(), spanId.toLong(), async)
    return 0.0
  }

  override fun instant(labelId: Double, arg: Double): Double {
    NativeBridge.nativeInstant(NativeBridge.CATEGORY_USER, labelId.toInt(), arg.toLong())
    return 0.0
  }

  override fun completeSpan(labelId: Double, startNs: Double, durationNs: Double, flags: Double): Double {
    NativeBridge.nativeComplete(NativeBridge.CATEGORY_JS, labelId.toInt(), startNs.toLong(), durationNs.toLong(), flags.toInt(), 0)
    return 0.0
  }

  override fun reportLongTask(startNs: Double, durationNs: Double): Double {
    NativeBridge.nativeLongTask(startNs.toLong(), durationNs.toLong())
    return 0.0
  }

  override fun markScreen(labelId: Double): Double {
    NativeBridge.nativeMarkScreen(labelId.toInt())
    return 0.0
  }

  private var samplingStart: JSONObject? = null

  // Pairs the monotonic recorder clock with wall time so wall-clock profiles can be mapped later.
  private fun clockAnchor(): JSONObject {
    val before = NativeBridge.nativeNowNs()
    val wallUs = System.currentTimeMillis() * 1000
    val after = NativeBridge.nativeNowNs()
    return JSONObject().apply {
      put("mono_ns", (before + after) / 2)
      put("wall_us", wallUs)
      put("wall_resolution_us", 1000)
      put("uncertainty_ns", (after - before) / 2)
    }
  }

  override fun startHermesSampling(hz: Double): String {
    return try {
      val anchor = clockAnchor()
      HermesSamplingProfiler.enable()
      samplingStart = anchor
      NativeBridge.nativeCpuMark()
      NativeBridge.nativeInstant(NativeBridge.CATEGORY_INSPECTOR, NativeBridge.nativeIntern("hermes.sampling.start"), 0)
      JSONObject().put("status", "available").put("anchor", anchor).put("note", "Android API has no frequency parameter").toString()
    } catch (e: Throwable) {
      JSONObject().put("status", "unavailable").put("reason", e.toString()).toString()
    }
  }

  override fun stopHermesSampling(): String {
    val start = samplingStart ?: return JSONObject().put("status", "error").put("reason", "not sampling").toString()
    return try {
      // Read before the dump, so writing the profile is not counted as the app's work.
      val cpu = runCatching { JSONObject(String(NativeBridge.nativeCpuSinceMark(), Charsets.UTF_8)) }.getOrNull()
      NativeBridge.nativeInstant(NativeBridge.CATEGORY_INSPECTOR, NativeBridge.nativeIntern("hermes.sampling.stop"), 0)
      val dir = File(sessionsDir(), "session-${NativeBridge.nativeNowNs() / 1_000_000}").apply { mkdirs() }
      HermesSamplingProfiler.dumpSampledTraceToFile(File(dir, PROFILE_FILE).absolutePath)
      HermesSamplingProfiler.disable()
      val session = JSONObject().apply {
        put("start", start)
        put("stop", clockAnchor())
        put("profile", PROFILE_FILE)
        put("profile_clock", "as emitted by Hermes; map with the anchors")
        put("recorder_session_id", recorderSessionId ?: JSONObject.NULL)
        put("pid", android.os.Process.myPid())
        // Synchronous module calls run on the JS thread.
        put("js_tid", android.os.Process.myTid())
        put("cpu", cpu ?: JSONObject.NULL)
        put("clock_domain", "CLOCK_MONOTONIC")
      }
      File(dir, "session.json").writeText(session.toString())
      val removed = entries(sessionsDir(), "session-").drop(maxSessions).count { it.deleteRecursively() }
      samplingStart = null
      JSONObject().put("status", "available").put("path", dir.absolutePath).put("removedSessions", removed).toString()
    } catch (e: Throwable) {
      JSONObject().put("status", "error").put("reason", e.toString()).toString()
    }
  }

  private fun sessionsDir(): File = File(incidentDir().parentFile, "rnti-sessions")

  // Only finished entries carry the prefix; in-progress writes use dot-prefixed temp names.
  private fun entries(dir: File, prefix: String): List<File> =
    dir.listFiles { f -> f.isDirectory && f.name.startsWith(prefix) }
      ?.sortedByDescending { it.lastModified() } ?: emptyList()

  override fun listIncidents(): WritableArray {
    val out = Arguments.createArray()
    entries(incidentDir(), "incident-").forEach { out.pushString(it.absolutePath) }
    return out
  }

  override fun deleteIncident(path: String): Boolean {
    val target = File(path).canonicalFile
    val inside = target.parentFile == incidentDir().canonicalFile && target.name.startsWith("incident-")
    return inside && target.deleteRecursively()
  }

  override fun clearData(): Double =
    (entries(incidentDir(), "incident-") + entries(sessionsDir(), "session-"))
      .count { it.deleteRecursively() }.toDouble()

  override fun readIncidentFile(path: String, file: String): String {
    val root = incidentDir().canonicalFile
    val target = File(path, file).canonicalFile
    require(target.path.startsWith(root.path + File.separator)) { "path outside incident directory" }
    return target.readText()
  }

  override fun simulateNativeBlock(ms: Double): Double {
    RNThreadInspector.measure("debug.nativeBlock") { busyWait(ms.toLong()) }
    return 0.0
  }

  override fun simulateMainThreadBlock(ms: Double) {
    Handler(Looper.getMainLooper()).post {
      RNThreadInspector.measure("debug.mainThreadBlock") { busyWait(ms.toLong()) }
    }
  }

  override fun simulateAsyncNativeWork(ms: Double, flowId: Double, promise: Promise) {
    val flow = flowId.toLong()
    worker.execute {
      RNThreadInspector.nameCurrentThread("rnti.demo-worker")
      RNThreadInspector.measure("debug.asyncWork", flow) { Thread.sleep(ms.toLong()) }
      promise.resolve(null)
    }
  }

  private fun busyWait(ms: Long) {
    val end = SystemClock.elapsedRealtime() + ms
    while (SystemClock.elapsedRealtime() < end) Unit
  }

  override fun onHostResume() = NativeBridge.nativeSetForeground(true)

  override fun onHostPause() = NativeBridge.nativeSetForeground(false)

  override fun onHostDestroy() = Unit

  override fun invalidate() {
    reactApplicationContext.removeLifecycleEventListener(this)
    NativeBridge.jsPoster = null
    NativeBridge.savedListener = null
    super.invalidate()
  }

  companion object {
    const val NAME = NativeThreadInspectorSpec.NAME
    private const val DEFAULT_MAX_SESSIONS = 5
    // Chrome trace format, as Hermes dumps it.
    private const val PROFILE_FILE = "hermes.trace.json"
    @Volatile private var maxSessions = DEFAULT_MAX_SESSIONS
    // Outlives module instances, like the native recorder it names.
    @Volatile private var recorderSessionId: String? = null

    init {
      System.loadLibrary("rnti")
    }
  }
}
