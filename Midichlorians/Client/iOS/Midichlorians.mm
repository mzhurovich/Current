/*******************************************************************************
 The MIT License (MIT)
 Copyright (c) 2015:
 * Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 *******************************************************************************/

#import "Midichlorians.h"
#import "MidichloriansImpl.h"

@implementation Midichlorians

+ (void)setup:(NSString*)serverUrl withLaunchOptions:(NSDictionary*)options {
    [MidichloriansImpl setup:serverUrl withLaunchOptions:options];
}

+ (void)identify:(NSString *)identifier {
    [MidichloriansImpl identify:identifier];
    [MidichloriansImpl emit:iOSIdentifyEvent()];
}

+ (void)focusEvent:(BOOL)hasFocus source:(NSString *)source {
    if (!source) {
          CURRENT_NSLOG(@"Malformed `trackEvent` call with empty `event` argument.");
    } else {
          [MidichloriansImpl emit:iOSFocusEvent(static_cast<bool>(hasFocus), [source UTF8String])];
    }
}

+ (void)trackEvent:(NSString *)event source:(NSString *)eventSource properties:(NSDictionary *)eventProperties {
    if (!event) {
        CURRENT_NSLOG(@"Malformed `trackEvent` call with empty `event` argument.");
        return;
    }
    if (!eventSource) {
        CURRENT_NSLOG(@"Malformed `trackEvent` call with empty `source` argument.");
        return;
    }
    if (!eventProperties) {
        CURRENT_NSLOG(@"Malformed `trackEvent` call with empty `properties` argument.");
        return;
    }
    [MidichloriansImpl emit:iOSGenericEvent(event, eventSource, eventProperties)];
}

@end
