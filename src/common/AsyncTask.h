#pragma once

#include "../media/MediaTypes.h"
#include "RequestGeneration.h"

#include <QCoroTask>

#include <QDebug>
#include <QFuture>
#include <QPointer>
#include <QPromise>
#include <QThreadPool>

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace Spool::Async {

// Runs `work` on `pool` (the global pool by default) and hands back a future
// a coroutine can await, for disk and CPU work that must stay off the GUI
// thread. A one-thread pool runs its work strictly in order.
template <typename Work> auto background(Work work, QThreadPool *pool = QThreadPool::globalInstance())
{
    using Result = std::invoke_result_t<Work>;
    auto promise = std::make_shared<QPromise<Result>>();
    promise->start();
    QFuture<Result> future = promise->future();
    pool->start([promise, work = std::move(work)]() mutable {
        try {
            if constexpr (std::is_void_v<Result>) {
                work();
            } else {
                promise->addResult(work());
            }
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    });
    return future;
}

namespace detail {
    template <typename Failure>
    void reportFailure(const char *operation, Failure& failure, const std::exception_ptr& error)
    {
        qWarning().noquote() << "async:" << operation << "failed:" << exceptionMessage(error);
        std::invoke(failure, error);
    }

} // namespace detail

template <typename T, typename Success, typename Failure>
void runDetached(QCoro::Task<T> task, Success success, Failure failure, const char *operation = "detached task")
{
    std::move(task).then(std::move(success), [failure = std::move(failure), operation](const std::exception&) mutable {
        detail::reportFailure(operation, failure, std::current_exception());
    });
}

template <typename Context, typename T, typename Success, typename Failure>
void runScoped(
    Context *context, QCoro::Task<T> task, Success success, Failure failure, const char *operation = "detached task")
{
    QPointer<Context> guard(context);
    auto guardedFailure = [guard, failure = std::move(failure)](const std::exception_ptr& error) mutable {
        if (guard)
            std::invoke(failure, error);
    };

    if constexpr (std::is_void_v<T>) {
        runDetached(
            std::move(task),
            [guard, success = std::move(success)]() mutable {
                if (guard)
                    std::invoke(success);
            },
            std::move(guardedFailure), operation);
    } else {
        runDetached(
            std::move(task),
            [guard, success = std::move(success)](T value) mutable {
                if (guard)
                    std::invoke(success, std::move(value));
            },
            std::move(guardedFailure), operation);
    }
}

template <typename Context, typename T, typename Success, typename Failure>
void runLatest(Context *context, QCoro::Task<T> task, const RequestGeneration& generation,
    RequestGeneration::Token token, Success success, Failure failure, const char *operation = "latest task")
{
    const RequestGeneration *generationGuard = &generation;
    auto guardedFailure
        = [generationGuard, token, failure = std::move(failure)](const std::exception_ptr& error) mutable {
              if (generationGuard->isCurrent(token))
                  std::invoke(failure, error);
          };

    if constexpr (std::is_void_v<T>) {
        runScoped(
            context, std::move(task),
            [generationGuard, token, success = std::move(success)]() mutable {
                if (generationGuard->isCurrent(token))
                    std::invoke(success);
            },
            std::move(guardedFailure), operation);
    } else {
        runScoped(
            context, std::move(task),
            [generationGuard, token, success = std::move(success)](T value) mutable {
                if (generationGuard->isCurrent(token))
                    std::invoke(success, std::move(value));
            },
            std::move(guardedFailure), operation);
    }
}

} // namespace Spool::Async
