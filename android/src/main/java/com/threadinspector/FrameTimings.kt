package com.threadinspector

import android.app.Activity
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.view.FrameMetrics
import android.view.Window
import android.annotation.TargetApi
import java.lang.ref.WeakReference

// Frames the app's window drew, from FrameMetrics: intended vsync to the end of the app's frame work, not display.
@TargetApi(Build.VERSION_CODES.O)
internal class FrameTimings {
  private val handler by lazy { Handler(HandlerThread("rnti.frames").apply { start() }.looper) }
  private var window: WeakReference<Window>? = null
  @Volatile private var periodNs = 33_333_333L
  private var frameLabel = 0
  private var slowLabel = 0
  private var named = false

  private val listener = Window.OnFrameMetricsAvailableListener { _, m, _ ->
    if (!named) {
      NativeBridge.nativeNameThread("rnti.frames")
      frameLabel = NativeBridge.nativeIntern("frame")
      slowLabel = NativeBridge.nativeIntern("frame.slow")
      named = true
    }
    if (m.getMetric(FrameMetrics.FIRST_DRAW_FRAME) == 1L) return@OnFrameMetricsAvailableListener
    val start = m.getMetric(FrameMetrics.INTENDED_VSYNC_TIMESTAMP)
    val dur = m.getMetric(FrameMetrics.TOTAL_DURATION)
    val budget = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) m.getMetric(FrameMetrics.DEADLINE) else periodNs
    NativeBridge.nativeFrame(if (dur > budget) slowLabel else frameLabel, start, dur)
  }

  // Main thread.
  fun attach(activity: Activity?) {
    val w = activity?.window
    if (w != null && window?.get() === w) return
    detach()
    if (w == null) return
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
      @Suppress("DEPRECATION")
      val hz = runCatching { w.windowManager.defaultDisplay.refreshRate }.getOrDefault(0f)
      // Two periods, as JankStats: the render pipeline legitimately spans more than one.
      periodNs = (2e9 / (if (hz > 0) hz else 60f)).toLong()
    }
    runCatching { w.addOnFrameMetricsAvailableListener(listener, handler) }.onSuccess { window = WeakReference(w) }
  }

  // Main thread.
  fun detach() {
    window?.get()?.let { runCatching { it.removeOnFrameMetricsAvailableListener(listener) } }
    window = null
  }
}
