#include "platform/PlatformApplicationServices.h"
#include "platform/NativeAppWindow.h"
#include "player/PlayerController.h"
#include "AppleMobileRuntime.h"
#include "AppleSystemMediaControls.h"
#include <atomic>

#include <QGuiApplication>
#include <QMetaObject>
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#import <TargetConditionals.h>

namespace Spool {
namespace {
    std::atomic_bool renderingAllowed { true };
}
bool appleMobileRenderingAllowed() { return renderingAllowed.load(std::memory_order_acquire); }
struct PlatformApplicationServices::PlatformData : QObject {
    ApplicationHooks& hooks;
    NativeAppWindow& window;
    PlayerController *player;
    NSMutableArray *observers = [NSMutableArray array];
    std::unique_ptr<AppleSystemMediaControls> mediaControls;
    bool resumeAfterInterruption = false;

    PlatformData(ApplicationHooks& hooks, NativeAppWindow& window)
        : hooks(hooks), window(window), player(hooks.player) { }
    ~PlatformData()
    {
        for (id observer in observers)
            [NSNotificationCenter.defaultCenter removeObserver:observer];
        mediaControls.reset();
        [AVAudioSession.sharedInstance setActive:NO
            withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nil];
    }
    void syncSession()
    {
        NSError *error = nil;
        AVAudioSession *session = AVAudioSession.sharedInstance;
        if (player->sessionActive()) {
            [session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeMoviePlayback options:0 error:&error];
            if (!error)
                [session setActive:YES error:&error];
        } else {
            [session setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&error];
        }
        if (error)
            emit hooks.toastRequested(QStringLiteral("Could not activate the system audio output."));
        mediaControls->updateNowPlaying();
    }
};
PlatformApplicationServices::PlatformApplicationServices(
    QGuiApplication&, NativeAppWindow& window, ApplicationHooks& hooks, RouterController&)
    : m_platform(std::make_unique<PlatformData>(hooks, window)) { }
PlatformApplicationServices::~PlatformApplicationServices() = default;
void PlatformApplicationServices::start()
{
    auto *p = m_platform.get();
    QObject::connect(p->player, &PlayerController::sessionActiveChanged, p, [p] { p->syncSession(); });
    p->mediaControls = std::make_unique<AppleSystemMediaControls>(p->hooks);
    QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, p, [p](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            renderingAllowed.store(true, std::memory_order_release);
            p->player->resyncForForeground();
            p->window.update();
        } else if (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended) {
            renderingAllowed.store(false, std::memory_order_release);
            if (p->player->sessionActive() && p->player->mediaKind() != QStringLiteral("audio"))
                p->player->setPaused(true);
        }
    });
    NSNotificationCenter *notifications = NSNotificationCenter.defaultCenter;
    [p->observers addObject:[notifications addObserverForName:AVAudioSessionInterruptionNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            const auto type = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
            QMetaObject::invokeMethod(p, [p, type, note] {
                if (type == AVAudioSessionInterruptionTypeBegan) {
                    p->resumeAfterInterruption = p->player->sessionActive() && !p->player->paused();
                    p->player->setPaused(true);
                } else {
                    const auto options = [note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
                    if (p->resumeAfterInterruption && (options & AVAudioSessionInterruptionOptionShouldResume)
                        && (p->player->mediaKind() == QStringLiteral("audio") || qGuiApp->applicationState() == Qt::ApplicationActive)) {
                        p->syncSession();
                        p->player->setPaused(false);
                    }
                    p->resumeAfterInterruption = false;
                }
            }, Qt::QueuedConnection);
        }]];
    [p->observers addObject:[notifications addObserverForName:AVAudioSessionRouteChangeNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            if ([note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue] == AVAudioSessionRouteChangeReasonOldDeviceUnavailable)
                QMetaObject::invokeMethod(p, [p] { p->player->setPaused(true); }, Qt::QueuedConnection);
        }]];
    [p->observers addObject:[notifications addObserverForName:AVAudioSessionMediaServicesWereResetNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
            QMetaObject::invokeMethod(p, [p] { p->syncSession(); p->player->setPaused(true); }, Qt::QueuedConnection);
        }]];
    [p->observers addObject:[notifications addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *) {
            QMetaObject::invokeMethod(p, [p] { if (p->hooks.memoryPressure) p->hooks.memoryPressure(QStringLiteral("critical")); }, Qt::QueuedConnection);
        }]];
#if !TARGET_OS_TV
    QObject::connect(&p->hooks, &ApplicationHooks::diagnosticsReportSaved, p, [p](const QString& path) {
        const QByteArray encoded = path.toUtf8();
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:encoded.constData()]];
        dispatch_async(dispatch_get_main_queue(), ^{
            UIWindow *window = nil;
            for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
                if ([scene isKindOfClass:UIWindowScene.class] && scene.activationState == UISceneActivationStateForegroundActive) {
                    for (UIWindow *candidate in ((UIWindowScene *)scene).windows)
                        if (candidate.isKeyWindow) window = candidate;
                }
            }
            UIViewController *presenter = window.rootViewController;
            while (presenter.presentedViewController) presenter = presenter.presentedViewController;
            if (!presenter) return;
            auto *share = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
            share.popoverPresentationController.sourceView = presenter.view;
            share.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(presenter.view.bounds), CGRectGetMidY(presenter.view.bounds), 1, 1);
            [presenter presentViewController:share animated:YES completion:nil];
        });
    });
#endif
}
} // namespace Spool
