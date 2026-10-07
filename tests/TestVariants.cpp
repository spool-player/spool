#include "TestMain.h"
#include <QByteArray>
#include <QtGlobal>

namespace {
int mediaInterpreter(int argc, char **argv)
{
    qputenv("QV4_FORCE_INTERPRETER", "1");
    return SpoolTests::invoke("provider-media-page", argc, argv);
}
int bundledContract(int argc, char **argv)
{
    Q_UNUSED(argc);
    QByteArray fixture = QByteArrayLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/bundled-jellyfin.mjs");
    char *arguments[] = { argv[0], fixture.data(), nullptr };
    return SpoolTests::invoke("bundled-jellyfin", 2, arguments);
}
int bundledInterpreter(int argc, char **argv)
{
    qputenv("QV4_FORCE_INTERPRETER", "1");
    return bundledContract(argc, argv);
}
[[maybe_unused]] const bool mediaRegistered = SpoolTests::registerTest("provider-media-page-interpreter", mediaInterpreter);
[[maybe_unused]] const bool interpreterRegistered = SpoolTests::registerTest("bundled-jellyfin-interpreter", bundledInterpreter);
}
