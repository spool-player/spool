#include "platform/webos/WebOSUpdateInstaller.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QProcess>
#include <QTimer>

namespace Spool {
namespace {

    constexpr qsizetype kMaximumResponseBytes = 64 * 1024;
    constexpr qsizetype kMaximumErrorBytes = 4096;
    constexpr auto kPackageId = "com.sachk.spool";

    QString errorDescription(const QJsonObject& object, const QString& textKey)
    {
        QString text = object.value(textKey).toString();
        const QJsonValue code = object.value(QStringLiteral("errorCode"));
        QString codeText;
        if (code.isString())
            codeText = code.toString();
        else if (code.isDouble())
            codeText = QString::number(code.toDouble(), 'g', 16);
        if (!codeText.isEmpty())
            text = text.isEmpty() ? codeText : codeText + QStringLiteral(": ") + text;
        return text;
    }

} // namespace

WebOSUpdateInstaller::WebOSUpdateInstaller(QObject *parent)
    : QObject(parent)
{
}

bool WebOSUpdateInstaller::isRunning() const
{
    return m_process != nullptr;
}

void WebOSUpdateInstaller::install(const QString& packagePath)
{
    if (isRunning())
        return;

    const QFileInfo package(packagePath);
    if (!package.isAbsolute() || !package.isFile() || !package.isReadable()) {
        emit failed(tr("The downloaded update package is not a readable local file."));
        return;
    }

    m_output.clear();
    m_errorOutput.clear();
    m_process = new QProcess(this);
    QProcess *process = m_process;
    process->setProcessChannelMode(QProcess::SeparateChannels);

    // Protocol inspired by Homebrew Channel (MIT, webOS Brew): services/service.ts
    // installPackage and services/webos-service-remote/execbus.js, at
    // https://github.com/webosbrew/webos-homebrew-channel. Only its public transport
    // is used: luna-send-pub rejects -a; no private-bus or elevated fallback.
    const QJsonObject payload {
        { QStringLiteral("id"), QString::fromLatin1(kPackageId) },
        { QStringLiteral("ipkUrl"), package.absoluteFilePath() },
        { QStringLiteral("subscribe"), true },
    };
    process->setProgram(QStringLiteral("/usr/bin/luna-send-pub"));
    process->setArguments({ QStringLiteral("-i"), QStringLiteral("luna://com.webos.appInstallService/dev/install"),
        QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)) });

    // Keep the subscription alive until a terminal response. webOS may stop
    // Spool to replace it after accepting the update; the package must survive
    // application teardown so the installer can finish.
    connect(process, &QProcess::readyReadStandardOutput, this, [this] { readStandardOutput(); });
    connect(process, &QProcess::readyReadStandardError, this, &WebOSUpdateInstaller::readStandardError);
    connect(process, &QProcess::started, this,
        [this] { emit statusChanged(tr("Waiting for webOS to install the update…")); });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // A crash also emits finished, where the last buffered response must be
        // consumed before deciding whether installation completed successfully.
        if (error == QProcess::Crashed)
            return;
        readStandardError();
        complete(false,
            processFailure(tr("Could not communicate with the webOS installer: %1").arg(m_process->errorString())));
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        readStandardError();
        readStandardOutput(true);
        if (!m_process)
            return;
        const QString message = exitStatus == QProcess::CrashExit
            ? tr("The webOS installer process crashed before confirming installation.")
            : tr("The webOS installer exited with code %1 before confirming installation.").arg(exitCode);
        complete(false, processFailure(message));
    });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    // Interactive luna-send watches stdin for HUP/ERR. Keep its input pipe open,
    // even though this protocol never writes to it.
    process->start(QIODevice::ReadWrite);
}

void WebOSUpdateInstaller::readStandardOutput(bool finalChunk)
{
    if (!m_process)
        return;
    m_process->setReadChannel(QProcess::StandardOutput);
    while (m_process && m_process->bytesAvailable() > 0) {
        // readLine may return only part of a line. Permit one byte beyond the
        // cap to detect oversized responses without buffering arbitrary output.
        const QByteArray chunk = m_process->readLine(kMaximumResponseBytes - m_output.size() + 2);
        if (chunk.isEmpty())
            break;
        m_output.append(chunk);
        if (m_output.size() > kMaximumResponseBytes) {
            complete(false, tr("The webOS installer returned an oversized response."));
            return;
        }
        if (m_output.endsWith('\n')) {
            const QByteArray line = m_output;
            m_output.clear();
            handleResponse(line);
        }
    }
    if (finalChunk && m_process && !m_output.isEmpty()) {
        const QByteArray line = m_output;
        m_output.clear();
        handleResponse(line);
    }
}

void WebOSUpdateInstaller::readStandardError()
{
    if (!m_process)
        return;
    m_process->setReadChannel(QProcess::StandardError);
    char chunk[4096];
    while (m_process->bytesAvailable() > 0) {
        const qint64 count = m_process->read(chunk, sizeof(chunk));
        if (count <= 0)
            break;
        const qsizetype available = kMaximumErrorBytes - m_errorOutput.size();
        m_errorOutput.append(chunk, qMin(available, static_cast<qsizetype>(count)));
    }
    m_process->setReadChannel(QProcess::StandardOutput);
}

void WebOSUpdateInstaller::handleResponse(const QByteArray& line)
{
    if (line.trimmed().isEmpty())
        return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        complete(false, tr("The webOS installer returned an invalid JSON response."));
        return;
    }

    const QJsonObject response = document.object();
    const QJsonValue returnValue = response.value(QStringLiteral("returnValue"));
    const QJsonObject details = response.value(QStringLiteral("details")).toObject();
    if (returnValue.isBool() && !returnValue.toBool()) {
        const QString reason = errorDescription(response, QStringLiteral("errorText"));
        complete(false, reason.isEmpty() ? tr("webOS refused the update installation.") : reason);
        return;
    }
    if (details.contains(QStringLiteral("errorCode"))) {
        const QString reason = errorDescription(details, QStringLiteral("reason"));
        complete(false, reason.isEmpty() ? tr("webOS reported an installation error.") : reason);
        return;
    }

    const QJsonValue status = response.value(QStringLiteral("statusValue"));
    if (status.isDouble() && status.toDouble() == 30) {
        if (details.value(QStringLiteral("packageId")).toString() != QString::fromLatin1(kPackageId)) {
            complete(false, tr("webOS confirmed installation of an unexpected package."));
            return;
        }
        complete(true, {});
        return;
    }

    QString text = response.value(QStringLiteral("statusText")).toString();
    if (text.isEmpty())
        text = details.value(QStringLiteral("statusText")).toString();
    if (text.isEmpty() && status.isDouble())
        text = tr("webOS installation status: %1").arg(status.toDouble(), 0, 'g', 16);
    if (text.isEmpty())
        text = tr("webOS is processing the update…");
    emit statusChanged(text);
}

QString WebOSUpdateInstaller::processFailure(const QString& message) const
{
    const QString diagnostic = QString::fromUtf8(m_errorOutput).trimmed();
    return diagnostic.isEmpty() ? message : message + QStringLiteral("\n") + diagnostic;
}

void WebOSUpdateInstaller::complete(bool success, const QString& message)
{
    if (!m_process)
        return;
    QProcess *process = m_process;
    m_process = nullptr;
    disconnect(process, nullptr, this, nullptr);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
    } else {
        // Like upstream req.cancel(), stop only the completed/failed subscription.
        // This is not an installation cancellation API and does not remove the IPK.
        process->terminate();
        QTimer::singleShot(2000, process, [process] {
            if (process->state() != QProcess::NotRunning)
                process->kill();
        });
    }
    m_output.clear();
    m_errorOutput.clear();
    if (success)
        emit finished();
    else
        emit failed(message);
}

} // namespace Spool
