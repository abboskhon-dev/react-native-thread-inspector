#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef struct {
  uint32_t label;
  uint64_t spanId;
} RNTISpan;

/// Explicit native spans for app Objective-C/Swift code; no-ops until the inspector is started.
@interface RNThreadInspector : NSObject

+ (uint64_t)newFlow;
+ (RNTISpan)begin:(NSString *)name NS_SWIFT_NAME(begin(_:));
+ (RNTISpan)begin:(NSString *)name flowId:(uint64_t)flowId NS_SWIFT_NAME(begin(_:flowId:));
+ (void)end:(RNTISpan)span NS_SWIFT_NAME(end(_:));
// Swift uses the generic measure in RNThreadInspector.swift, which returns the block's result.
+ (void)measure:(NSString *)name block:(NS_NOESCAPE void (^)(void))block NS_REFINED_FOR_SWIFT;
+ (void)measure:(NSString *)name flowId:(uint64_t)flowId block:(NS_NOESCAPE void (^)(void))block NS_REFINED_FOR_SWIFT;
+ (void)nameCurrentThread:(NSString *)name;

@end

NS_ASSUME_NONNULL_END
