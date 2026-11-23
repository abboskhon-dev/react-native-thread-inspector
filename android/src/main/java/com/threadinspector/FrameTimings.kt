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
  private val thread = HandlerThread("rnti.frames").apply { start() }
  private val handler = Handler(thread.looper)
  private var window: WeakReference<Window>? = null
  private var frameLabel = 0
  private var slowLabel = 0
  private var named = false

  private val listener = Window.OnFrameMetricsAvailableListener { w, m, _ ->
    if (!named) {
      NativeBridge.nativeNameThread("rnti.frames")
      named = true
    }
    if (m.getMetric(FrameMetrics.FIRST_DRAW_FRAME) == 1L) return@OnFrameMetricsAvailableListener
    val start = m.getMetric(FrameMetrics.INTENDED_VSYNC_TIMESTAMP)
    val dur = m.getMetric(FrameMetrics.TOTAL_DURATION)
    val slow = dur > budgetNs(w, m)
    if (frameLabel == 0) frameLabel = NativeBridge.nativeIntern("frame")
    if (slowLabel == 0) slowLabel = NativeBridge.nativeIntern("frame.slow")
    NativeBridge.nativeFrame(if (slow) slowLabel else frameLabel, start, dur)
  }

  private fun budgetNs(w: Window, m: FrameMetrics): Long {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) return m.getMetric(FrameMetrics.DEADLINE)
    @Suppress("DEPRECATION")
    val hz = w.windowManager.defaultDisplay.refreshRate
    return (1e9 / (if (hz > 0) hz else 60f)).toLong()
  }

  // Main thread.
  fun attach(activity: Activity?) {
    val w = activity?.window
    if (w != null && window?.get() === w) return
    detach()
    if (w == null) return
    w.addOnFrameMetricsAvailableListener(listener, handler)
    window = WeakReference(w)
  }

  // Main thread.
  fun detach() {
    window?.get()?.let { runCatching { it.removeOnFrameMetricsAvailableListener(listener) } }
    window = null
  }
}
