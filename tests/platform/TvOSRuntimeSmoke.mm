#include "TestMain.h"
#include "platform/CredentialStore.h"
#include "platform/PlatformPaths.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>
#include <QUuid>

#include <cmath>
#include <cstring>
#include <cstdio>

#import <AVFoundation/AVFoundation.h>
#import <Security/Security.h>
extern "C" {
#include <mpv/client.h>
}

#ifndef SPOOL_TRADITIONAL_TEST_BUNDLE
SPOOL_TEST_MAIN("tvos-audio")
{
    QGuiApplication app(argc, argv);
    QTemporaryFile audio(QDir::tempPath() + QStringLiteral("/spool-audio-XXXXXX.wav"));
    if (!audio.open())
        return 1;
    constexpr int rate = 48000;
    constexpr int bytes = rate * 2;
    QByteArray wav(44 + bytes, '\0');
    const auto le32 = [&wav](int offset, quint32 value) {
        for (int index = 0; index < 4; ++index)
            wav[offset + index] = static_cast<char>(value >> (index * 8));
    };
    std::memcpy(wav.data(), "RIFF", 4);
    le32(4, 36 + bytes);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    le32(16, 16);
    wav[20] = 1;
    wav[22] = 1;
    le32(24, rate);
    le32(28, rate * 2);
    wav[32] = 2;
    wav[34] = 16;
    std::memcpy(wav.data() + 36, "data", 4);
    le32(40, bytes);
    for (int sample = 0; sample < rate; ++sample) {
        const auto value = static_cast<qint16>(800 * std::sin(sample * (440.0 * 6.283185307179586 / rate)));
        wav[44 + sample * 2] = static_cast<char>(value);
        wav[45 + sample * 2] = static_cast<char>(value >> 8);
    }
    if (audio.write(wav) != wav.size() || !audio.flush())
        return 1;
    mpv_handle *handle = mpv_create();
    if (!handle)
        return 1;
    const auto finish = [handle](int result) { mpv_terminate_destroy(handle); return result; };
    if (mpv_set_option_string(handle, "vo", "null") < 0
        || mpv_set_option_string(handle, "ao", "audiounit") < 0
        || mpv_set_option_string(handle, "audio-exclusive", "yes") < 0
        || mpv_set_option_string(handle, "audio-fallback-to-null", "no") < 0
        || mpv_set_option_string(handle, "loop-file", "inf") < 0
        || mpv_initialize(handle) < 0)
        return finish(1);
    const QByteArray path = QFile::encodeName(audio.fileName());
    const char *load[] = { "loadfile", path.constData(), nullptr };
    if (mpv_command(handle, load) < 0)
        return finish(1);
    bool output = false;
    QElapsedTimer timer;
    timer.start();
    while (!output && timer.elapsed() < 10000) {
        app.processEvents(QEventLoop::AllEvents, 20);
        char *ao = mpv_get_property_string(handle, "current-ao");
        double position = 0;
        output = ao && QByteArray(ao) == "audiounit"
            && mpv_get_property(handle, "time-pos", MPV_FORMAT_DOUBLE, &position) == 0 && position > 0.1;
        mpv_free(ao);
        QThread::msleep(10);
    }
    const bool session = [AVAudioSession.sharedInstance.category isEqualToString:AVAudioSessionCategoryPlayback]
        && !(AVAudioSession.sharedInstance.categoryOptions & AVAudioSessionCategoryOptionMixWithOthers);
    if (!output || !session) {
        std::fprintf(stderr, "tvOS audio smoke: realOutput=%d exclusivePlaybackSession=%d\n", output, session);
        return finish(1);
    }
    std::fprintf(stderr, "tvOS audio smoke: AudioUnit output advanced with exclusive playback session\n");
    return finish(0);
}
#else

namespace {
int secureStoreSmoke(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "tvOS credentials smoke: receipt invocation token is required\n");
        return 1;
    }
    const QString nonce = QString::fromUtf8(argv[1]);
    QGuiApplication app(argc, argv);
    const QString account = QStringLiteral("smoke-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString value = QUuid::createUuid().toString();
    const bool saved = Spool::CredentialStore::save(account, value);
    const bool loaded = saved && Spool::CredentialStore::load(account) == value;
    Spool::CredentialStore::remove(account);
    const bool removed = Spool::CredentialStore::load(account).isEmpty();
    QTemporaryFile data(QDir(Spool::persistentDataRoot()).filePath(QStringLiteral("smoke-XXXXXX")));
    const bool writable = data.open() && data.write("sandbox") == 7 && data.flush();
    const QByteArray report = QJsonDocument(QJsonObject {
        { QStringLiteral("case"), QStringLiteral("tvos-credentials") },
        { QStringLiteral("nonce"), nonce },
        { QStringLiteral("saved"), saved },
        { QStringLiteral("loaded"), loaded },
        { QStringLiteral("removed"), removed },
        { QStringLiteral("writable"), writable },
    }).toJson(QJsonDocument::Compact);
    QSaveFile receipt(QDir::tempPath() + QStringLiteral("/tvos-credentials-result.json"));
    if (!receipt.open(QIODevice::WriteOnly) || receipt.write(report) != report.size() || !receipt.commit()) {
        std::fprintf(stderr, "tvOS credentials smoke: failed to write native result receipt\n");
        return 1;
    }
    if (!saved || !loaded || !removed || !writable) {
        std::fprintf(stderr, "tvOS credentials smoke: saved=%d loaded=%d removed=%d writable=%d\n",
            saved, loaded, removed, writable);
        // Read the actual native status without logging any account/value.
        NSDictionary *query = @{ (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService: @SPOOL_APPLE_CREDENTIAL_SERVICE,
            (__bridge id)kSecAttrAccount: [NSString stringWithUTF8String:account.toUtf8().constData()],
            (__bridge id)kSecReturnData: @YES };
        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
        if (result)
            CFRelease(result);
        std::fprintf(stderr, "tvOS credentials smoke: nativeStatus=%d\n", int(status));
        return 1;
    }
    std::fprintf(stderr, "tvOS credentials smoke: Keychain roundtrip and sandbox file persistence passed\n");
    return 0;
}
[[maybe_unused]] const bool secureRegistered = SpoolTests::registerTest("tvos-credentials", &secureStoreSmoke);
}
#endif
