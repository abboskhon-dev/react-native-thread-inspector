#import <ReactCommon/RCTTurboModuleWithJSIBindings.h>
#import <ThreadInspectorSpec/ThreadInspectorSpec.h>

@interface ThreadInspector : NativeThreadInspectorSpecBase <NativeThreadInspectorSpec, RCTTurboModuleWithJSIBindings>

@end
