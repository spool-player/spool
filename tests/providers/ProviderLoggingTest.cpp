#include "TestMain.h"
#include "media/MediaTypes.h"
#include "provider/ScriptRuntime.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>

#include <cstdlib>
#include <iostream>

namespace {
struct Message {
    QtMsgType type;
    QString category;
    QString text;
};
QMutex messagesMutex;
QList<Message> messages;

void capture(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    const QString category = QString::fromLatin1(context.category);
    if (category != QLatin1String("spool.provider") && category != QLatin1String("spool.provider.trace"))
        return;
    const QMutexLocker lock(&messagesMutex);
    messages.append({ type, category, text });
}
QList<Message> takeMessages()
{
    const QMutexLocker lock(&messagesMutex);
    QList<Message> result;
    result.swap(messages);
    return result;
}
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

SPOOL_TEST_MAIN("provider-logging")
{
    QCoreApplication app(argc, argv);
    const auto previous = qInstallMessageHandler(capture);
    QLoggingCategory::setFilterRules(QStringLiteral("spool.provider=false\nspool.provider.trace=false"));
    {
        Spool::ScriptRuntime runtime(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/logging.mjs"), {}, {},
            nullptr, QStringLiteral("fixture.logging"));
        const QVariantMap configuration { { "token", "source-credential" }, { "refreshToken", "refresh-credential" } };
        QCoro::waitFor(runtime.addSource("private-account-name", configuration, {}));
        const auto disabled = QCoro::waitFor(runtime.call("private-account-name", "exercise"));
        require(disabled.value("lazy").toInt() == 0 && disabled.value("reads").toInt() == 0,
            "disabled source/operation logging must not evaluate lazy messages or field getters");
        for (const auto& flag : disabled.value("enabled").toMap())
            require(!flag.toBool(), "disabled native filters must be reflected by the JS guard");
        require(takeMessages().isEmpty(), "disabled logging must not emit native messages");
        require(!disabled.value("invalid").toBool(), "unknown levels are disabled");

        QLoggingCategory::setFilterRules(QStringLiteral("spool.provider=true\nspool.provider.trace=true"));
        const auto enabled = QCoro::waitFor(runtime.call("private-account-name", "exercise"));
        require(enabled.value("lazy").toInt() == 6 && enabled.value("reads").toInt() == 5,
            "enabled operation and retained source hosts use current native filters");
        for (const auto& flag : enabled.value("enabled").toMap())
            require(flag.toBool(), "all enabled levels are exposed by the JS guard");
        auto lines = takeMessages();
        require(lines.size() == 6, "five operation levels and retained source debug emit exactly once");
        const QList<QtMsgType> severities { QtDebugMsg, QtDebugMsg, QtInfoMsg, QtWarningMsg, QtCriticalMsg,
            QtDebugMsg };
        QString firstContext;
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const auto& line = lines.at(i);
            require(line.type == severities.at(i), "JS log levels map to native Qt severities");
            require(line.category == (i == 0 ? "spool.provider.trace" : "spool.provider"),
                "trace has a separate native filter category");
            require(line.text.contains("provider=fixture.logging account="),
                "trusted provider and account context is attached");
            require(!line.text.contains("source-credential") && !line.text.contains("private-account-name"),
                "credentials and account identities never enter output");
            if (i == 1)
                firstContext = line.text.section(' ', 0, 1);
        }

        // Trace remains off when ordinary provider debug is enabled.
        QLoggingCategory::setFilterRules(
            QStringLiteral("spool.provider=false\nspool.provider.debug=true\nspool.provider.trace=false"));
        const auto debugOnly = QCoro::waitFor(runtime.call("private-account-name", "exercise"));
        require(debugOnly.value("enabled").toMap().value("debug").toBool()
                && !debugOnly.value("enabled").toMap().value("trace").toBool(),
            "debug and trace filtering are independent");
        require(takeMessages().size() == 2, "debug-only rules suppress every other severity");

        QLoggingCategory::setFilterRules(
            QStringLiteral("spool.provider=false\nspool.provider.info=true\nspool.provider.trace=false"));
        Spool::setDiagnosticUrlsUnredacted(true);
        QCoro::waitFor(runtime.call("private-account-name", "safety"));
        lines = takeMessages();
        require(lines.size() == 5, "safety fixture emits all bounded diagnostic cases");
        const QStringList forbidden { "source-credential", "refresh-credential", "two word password", "private.example",
            "url-secret", "socket-secret", "0123456789abcdef0123456789abcdef01234567", "field-secret", "Alice",
            "field-hash" };
        for (const auto& line : lines) {
            require(line.text.size() <= 4096 && !line.text.contains('\n') && !line.text.contains('\r'),
                "provider output is bounded and cannot inject log lines");
            for (const auto& secret : forbidden)
                require(
                    !line.text.contains(secret), "provider diagnostics stay private even with unredacted URL opt-in");
        }
        require(lines.at(1).text.contains("\"count\":12") && lines.at(1).text.contains("\"success\":true"),
            "useful non-sensitive structured fields survive redaction");
        require(
            lines.at(2).text.contains("<dropped:message-limit>") && lines.at(3).text.contains("<dropped:field-limit>"),
            "oversized values are replaced without exposing a credential prefix");
        QCoro::waitFor(runtime.addSource("another-private-account", configuration, {}));
        lines = takeMessages();
        require(lines.size() == 1 && !lines.front().text.startsWith(firstContext)
                && lines.front().text.contains("source-created") && !lines.front().text.contains("source-credential"),
            "source factory logs are attributed to a distinct opaque account");
        Spool::setDiagnosticUrlsUnredacted(false);
    }
    qInstallMessageHandler(previous);
    QLoggingCategory::setFilterRules({});
    return 0;
}
