public extension RNThreadInspector {
  static func measure<T>(_ name: String, flowId: UInt64 = 0, _ block: () throws -> T) rethrows -> T {
    let span = begin(name, flowId: flowId)
    defer { end(span) }
    return try block()
  }
}
