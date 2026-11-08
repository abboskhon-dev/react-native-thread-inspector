package com.threadinspector

import android.os.Handler
import android.os.Looper

internal object NativeBridge {
  init {
    System.loadLibrary("rnti")
  }

  const val CATEGORY_JS = 1
  const val CATEGORY_NATIVE = 2
  const val CATEGORY_MAIN = 3
  const val CATEGORY_INSPECTOR = 10
  const val CATEGORY_USER = 11

  private const val JS_CHANNEL = 0
  private const val MAIN_CHANNEL = 1

  @Volatile var jsPoster: ((Runnable) -> Boolean)? = null
  @Volatile var savedListener: ((Long, String) -> Unit)? = null
  private val mainHandler = Handler(Looper.getMainLooper())

  @JvmStatic
  fun postProbe(channel: Int, token: Long): Boolean {
    val ack = Runnable { nativeAck(channel, token) }
    return when (channel) {
      JS_CHANNEL -> jsPoster?.invoke(ack) ?: false
      MAIN_CHANNEL -> mainHandler.post(ack)
      else -> false
    }
  }

  @JvmStatic
  fun onIncidentSaved(id: Long, path: String) {
    savedListener?.invoke(id, path)
  }

  @JvmStatic external fun nativeStart(
    config: LongArray,
    directory: String,
    sessionId: String,
    metaKeys: Array<String>,
    metaValues: Array<String>,
  ): Boolean

  @JvmStatic external fun nativeStop()
  @JvmStatic external fun nativeSetForeground(foreground: Boolean)
  @JvmStatic external fun nativeAck(channel: Int, token: Long)
  @JvmStatic external fun nativeNowNs(): Long
  @JvmStatic external fun nativeIntern(label: String): Int
  @JvmStatic external fun nativeNameThread(name: String)
  @JvmStatic external fun nativeNewFlowId(): Long
  @JvmStatic external fun nativeBegin(category: Int, label: Int, flowId: Long): Long
  @JvmStatic external fun nativeEnd(category: Int, label: Int, spanId: Long)
  @JvmStatic external fun nativeInstant(category: Int, label: Int, arg: Long)
  @JvmStatic external fun nativeComplete(category: Int, label: Int, startNs: Long, durNs: Long, flags: Int, flowId: Long)
  @JvmStatic external fun nativeLongTask(startNs: Long, durNs: Long)

  @JvmStatic external fun nativeMarkScreen(label: Int)
}
