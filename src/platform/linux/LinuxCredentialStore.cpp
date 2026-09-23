#include "../CredentialStore.h"
#include "../common/CredentialStoreFileBackend.h"

#include <QProcess>

namespace Spool::CredentialStore {
namespace {

    struct SecretResult {
        bool success = false;
        QByteArray output;
    };

    SecretResult runSecretTool(const QStringList& arguments, const QByteArray& input = {})
    {
        QProcess process;
        process.setProgram(QStringLiteral("secret-tool"));
        process.setArguments(arguments);
        process.start();
        if (!process.waitForStarted(3000))
            return {};
        if (!input.isEmpty()) {
            process.write(input);
            process.closeWriteChannel();
        }
        if (!process.waitForFinished(10000))
            return {};
        return { process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            process.readAllStandardOutput() };
    }

    QStringList attributes(const QString& profileId)
    {
        return { QStringLiteral("application"), QStringLiteral("spool"), QStringLiteral("profile"), profileId };
    }

} // namespace

QString load(const QString& profileId)
{
    if (profileId.isEmpty())
        return {};
    if (FileBackend::enabled())
        return FileBackend::load(profileId);
    const SecretResult result = runSecretTool(QStringList { QStringLiteral("lookup") } + attributes(profileId));
    return result.success ? QString::fromUtf8(result.output).trimmed() : QString();
}

bool save(const QString& profileId, const QString& accessToken)
{
    if (profileId.isEmpty() || accessToken.isEmpty())
        return false;
    if (FileBackend::enabled())
        return FileBackend::save(profileId, accessToken);
    return runSecretTool(
        QStringList { QStringLiteral("store"), QStringLiteral("--label=Spool") } + attributes(profileId),
        accessToken.toUtf8())
        .success;
}

void remove(const QString& profileId)
{
    if (profileId.isEmpty())
        return;
    if (FileBackend::enabled()) {
        FileBackend::remove(profileId);
        return;
    }
    runSecretTool(QStringList { QStringLiteral("clear") } + attributes(profileId));
}

void clear()
{
    if (FileBackend::enabled()) {
        FileBackend::clear();
        return;
    }
    runSecretTool({ QStringLiteral("clear"), QStringLiteral("application"), QStringLiteral("spool") });
}

} // namespace Spool::CredentialStore
