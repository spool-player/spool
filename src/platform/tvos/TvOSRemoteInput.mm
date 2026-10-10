#include "TvOSRemoteInput.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QWindow>

#include <algorithm>
#include <cmath>

#import <UIKit/UIKit.h>
#import <objc/runtime.h>

// Clickpad presses are already translated by Qt's UIKit plugin. Only indirect
// touchpad movement needs a bridge; direct touches and system-owned buttons do
// not enter it. The native view is Qt's documented platform window identifier.
@interface SpoolTVRemoteGestures : NSObject {
@public
    QPointer<QWindow> window;
    CGPoint consumed;
}
- (void)pan:(UIPanGestureRecognizer *)recognizer;
@end

@implementation SpoolTVRemoteGestures
- (void)pan:(UIPanGestureRecognizer *)recognizer
{
    if (!window)
        return;
    if (recognizer.state == UIGestureRecognizerStateBegan)
        consumed = CGPointZero;
    if (recognizer.state != UIGestureRecognizerStateBegan
        && recognizer.state != UIGestureRecognizerStateChanged)
        return;
    const CGPoint translation = [recognizer translationInView:recognizer.view];
    constexpr CGFloat step = 32;
    // A stationary finger never moves focus. Consume travelled distance rather
    // than velocity, so a long swipe can step through more than one TV tile.
    while (std::max(std::abs(translation.x - consumed.x), std::abs(translation.y - consumed.y)) >= step) {
        const CGFloat x = translation.x - consumed.x;
        const CGFloat y = translation.y - consumed.y;
        int key;
        if (std::abs(x) >= std::abs(y)) {
            key = x > 0 ? Qt::Key_Right : Qt::Key_Left;
            consumed.x += x > 0 ? step : -step;
            consumed.y = translation.y;
        } else {
            key = y > 0 ? Qt::Key_Down : Qt::Key_Up;
            consumed.y += y > 0 ? step : -step;
            consumed.x = translation.x;
        }
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &release);
    }
}
@end

namespace Spool {
void installTvOSRemoteInput(QWindow& window)
{
    static char association;
    UIView *view = (__bridge UIView *)reinterpret_cast<void *>(window.winId());
    if (!view || objc_getAssociatedObject(view, &association))
        return;
    SpoolTVRemoteGestures *target = [[SpoolTVRemoteGestures alloc] init];
    target->window = &window;
    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:target action:@selector(pan:)];
    pan.allowedTouchTypes = @[ @(UITouchTypeIndirect) ];
    pan.cancelsTouchesInView = YES;
    [view addGestureRecognizer:pan];
    objc_setAssociatedObject(view, &association, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
} // namespace Spool
