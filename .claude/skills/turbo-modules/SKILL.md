---
name: turbo-modules
description: >
  Adding or changing a native method in react-native-thread-inspector across the codegen spec,
  iOS, Android (Kotlin and JNI) and the TypeScript API. Trigger phrases: "add a native method",
  "TurboModule", "spec", "codegen", "JNI", "NativeThreadInspector", "expose to JS".
---

# TurboModules

One module, `ThreadInspector`, codegen name `ThreadInspectorSpec` (`codegenConfig` in `package.json`).

## Adding a method, in order

1. **Spec**, `src/NativeThreadInspector.ts`. `number` arrives as `double`.
   - A call whose timing matters must return a value (`number` or `string`): Android runs `void`
     methods asynchronously on `mqt_v_native`.
   - Work that waits on another thread returns a `Promise`.
   - Structured results cross as JSON strings (`start`, `startHermesSampling`).
2. **iOS**, `ios/ThreadInspector.mm`. Implement the codegen selector:
   `- (NSNumber *)name:(double)arg` for sync numbers, `NSString *` for strings,
   `resolve:reject:` for promises. Call C++ directly through `Inspector::shared()`; it is null
   before `start()`, so handle that.
3. **Android**, `ThreadInspectorModule.kt`: `override fun` from the generated
   `NativeThreadInspectorSpec`. If it needs `cpp/core`, add an `external fun` to `NativeBridge.kt`
   and its `Java_com_threadinspector_NativeBridge_<name>` in `android/src/main/cpp/ThreadInspectorJni.cpp`.
4. **TypeScript API**, `src/index.tsx`. Keep the native call out of hot loops; intern labels once.
5. **Jest**: add the method to `mockNative` in `src/__tests__/index.test.ts` and test the wrapper.
6. **Rebuild**: codegen runs on `pod install` (iOS) and Gradle (Android). Build both examples
   (`post-work-checks`).

## Public native API

App code calls the inspector through `RNThreadInspector` (Kotlin object in
`android/.../RNThreadInspector.kt`; Objective-C class in `ios/RNThreadInspector.h`, the pod's only
public header, importable from Swift because the podspec sets `DEFINES_MODULE`; Swift's generic
`measure` that returns the block's result is in `ios/RNThreadInspector.swift`). Keep the two
APIs matching. Every call there must be a no-op before `start()`.

## Capabilities

A new optional feature reports itself in the object `start()` returns
(`available`/`partial`/`disabled`/`unavailable` with a reason), on both platforms.
