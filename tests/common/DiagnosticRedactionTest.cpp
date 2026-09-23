#include "diagnostics/Diagnostics.h"
#include "media/MediaTypes.h"

#include "TestMain.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <iostream>

using Spool::sanitizedDiagnosticUrl;
using Spool::sanitizedLogMessage;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

SPOOL_TEST_MAIN("diagnostic-redaction")
{
    const QString query = sanitizedDiagnosticUrl(
        QStringLiteral("https://server/QuickConnect/Connect?SeCrEt=first%26TOKEN%3Dsecond&api_key=third"));
    require(!query.contains(QStringLiteral("first")), "mixed-case query secret should be removed");
    require(!query.contains(QStringLiteral("second")), "URL-encoded duplicate token should be removed");
    require(!query.contains(QStringLiteral("third")), "duplicate API key should be removed");
    require(query.contains(QStringLiteral("<redacted:credential>")), "credentials should have a stable marker");

    const QString headers
        = sanitizedDiagnosticUrl(QStringLiteral("Authorization: Bearer auth-secret\nX-Emby-Token=header-secret"));
    require(!headers.contains(QStringLiteral("auth-secret")), "authorization value should be removed");
    require(!headers.contains(QStringLiteral("header-secret")), "token header should be removed");

    const QString json = sanitizedDiagnosticUrl(
        QStringLiteral(R"({"Password":"json-password","access_TOKEN":"json-token","Code":"123456"})"));
    require(!json.contains(QStringLiteral("json-password")), "JSON password should be removed");
    require(!json.contains(QStringLiteral("json-token")), "mixed-case JSON token should be removed");
    require(!json.contains(QStringLiteral("123456")), "Quick Connect code should be removed");

    const QString personal = sanitizedLogMessage(QStringLiteral(
        R"(serverUrl=https://media.example:8096 userName=Alice title="Recognisable Show" itemId=0123456789abcdef0123456789abcdef address=192.168.1.25)"));
    require(!personal.contains(QStringLiteral("media.example")), "server URL should be removed");
    require(!personal.contains(QStringLiteral("Alice")), "user name should be removed");
    require(!personal.contains(QStringLiteral("Recognisable")), "media title should be removed");
    require(!personal.contains(QStringLiteral("0123456789abcdef")), "stable item ID should be removed");
    require(!personal.contains(QStringLiteral("192.168.1.25")), "network address should be removed");
    const QJsonObject report = QJsonDocument::fromJson(Spool::Diagnostics::supportReportPreview().toUtf8()).object();
    QStringList keys = report.keys();
    keys.sort();
    const QStringList expectedKeys { QStringLiteral("appVersion"), QStringLiteral("architecture"),
        QStringLiteral("diagnosticsOptIn"), QStringLiteral("disclosure"), QStringLiteral("errorCategories"),
        QStringLiteral("platform"), QStringLiteral("platformVersion"), QStringLiteral("qtVersion"),
        QStringLiteral("schemaVersion"), QStringLiteral("uptimeSeconds") };
    require(keys == expectedKeys, "support report must expose only the reviewed allowlist");
    require(!report.value(QStringLiteral("disclosure")).toString().isEmpty(), "support report must explain disclosure");
    return EXIT_SUCCESS;
}
