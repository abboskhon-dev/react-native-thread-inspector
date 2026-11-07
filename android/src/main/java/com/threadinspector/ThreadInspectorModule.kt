package com.threadinspector

import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.LifecycleEventListener
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.WritableArray
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors

class ThreadInspectorModule(reactContext: ReactApplicationContext) :
  NativeThreadInspectorSpec(reactContext), LifecycleEventListener {

  private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "rnti.demo-worker") }

  private fun incidentDir(): File =
    File(reactApplicationContext.getExternalFilesDir(null) ?: reactApplicationContext.filesDir, "rnti-incidents")

  override fun start(configJson: String): String {
    val c = JSONObject(configJson)
    val threshold = c.optJSONObject("incidentThresholdMs")
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
    val fresh = NativeBridge.nativeStart(
      config, dir.absolutePath, UUID.randomUUID().toString().substring(0, 8),
      meta.keys.toTypedArray(), meta.values.toTypedArray(),
    )
    context.removeLifecycleEventListener(this)
    context.addLifecycleEventListener(this)

    return JSONObject().apply {
      put("jsWatchdog", status("available"))
      put("mainThread", status("available"))
      put("explicitNativeSpans", status("available"))
      put("asyncNativeFlows", status("partial", "explicit flow IDs required"))
      put("turboModuleIntervals", status("disabled", "automatic adapter not implemented"))
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

  override fun beginSpan(labelId: Double, flowId: Double): Double =
    NativeBridge.nativeBegin(NativeBridge.CATEGORY_JS, labelId.toInt(), flowId.toLong()).toDouble()

  override fun endSpan(labelId: Double, spanId: Double): Double {
    NativeBridge.nativeEnd(NativeBridge.CATEGORY_JS, labelId.toInt(), spanId.toLong())
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

  override fun listIncidents(): WritableArray {
    val out = Arguments.createArray()
    incidentDir().listFiles { f -> f.isDirectory && f.name.startsWith("incident-") }
      ?.sortedByDescending { it.lastModified() }
      ?.forEach { out.pushString(it.absolutePath) }
    return out
  }

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
  }
}
