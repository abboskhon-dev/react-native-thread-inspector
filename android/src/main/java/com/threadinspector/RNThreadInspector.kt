package com.threadinspector

import java.util.concurrent.ConcurrentHashMap

/** Explicit native spans for app Kotlin/Java code; no-ops until the inspector is started. */
object RNThreadInspector {
  class Span internal constructor(internal val label: Int, internal val id: Long)

  private val labels = ConcurrentHashMap<String, Int>()

  private fun label(name: String): Int =
    labels[name] ?: NativeBridge.nativeIntern(name).also { if (it != 0) labels[name] = it }

  @JvmStatic fun newFlow(): Long = NativeBridge.nativeNewFlowId()

  @JvmStatic @JvmOverloads
  fun begin(name: String, flowId: Long = 0): Span {
    val id = label(name)
    return Span(id, NativeBridge.nativeBegin(NativeBridge.CATEGORY_NATIVE, id, flowId, false))
  }

  @JvmStatic fun end(span: Span) = NativeBridge.nativeEnd(NativeBridge.CATEGORY_NATIVE, span.label, span.id, false)

  inline fun <T> measure(name: String, flowId: Long = 0, block: () -> T): T {
    val span = begin(name, flowId)
    try {
      return block()
    } finally {
      end(span)
    }
  }

  @JvmStatic fun nameCurrentThread(name: String) = NativeBridge.nativeNameThread(name)
}
