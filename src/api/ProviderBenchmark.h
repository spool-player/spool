#pragma once

#include <QCoroTask>
#include <QString>

namespace JellyfinNative {

class DatabaseManager;
class JellyfinProvider;
class ProviderRegistry;

struct ProviderBenchmarkOptions {
    int iterations = 5;
    QString libraryName = QStringLiteral("Movies");
    QString searchQuery = QStringLiteral("The");
    QString outputPath;
};

class ProviderBenchmark {
public:
    static QCoro::Task<int> run(DatabaseManager *database, ProviderRegistry *registry,
        JellyfinProvider *jellyfinProvider, const ProviderBenchmarkOptions& options);
};

} // namespace JellyfinNative
