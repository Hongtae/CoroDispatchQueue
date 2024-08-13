//
// C++ Coroutine Dispatch Queue
// ---------------------------------------------------------------------------
//
//  File: DispatchQueue.h
//  Author: Hongtae Kim (tiff2766@gmail.com)
//
//  Copyright (c) 2024 Hongtae Kim. All rights reserved.
//

#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <exception>

#ifdef max
#define _POP_MACRO_MAX
#pragma push_macro(max)
#undef max
#endif

#ifndef CPP_CORO_DISPATCHQUEUE_EXPORT_API
#define CPP_CORO_DISPATCHQUEUE_EXPORT_API
#endif

#ifdef CPP_CORO_DISPATCHQUEUE_NAMESPACE
namespace CPP_CORO_DISPATCHQUEUE_NAMESPACE {
#endif
    class CPP_CORO_DISPATCHQUEUE_EXPORT_API DispatchQueue {
    public:
        DispatchQueue(uint32_t numThreads) noexcept;
        DispatchQueue(DispatchQueue&&) noexcept;
        ~DispatchQueue() noexcept {
            shutdown();
        }

        auto schedule() const noexcept {
            struct [[nodiscard]] Awaiter {
                constexpr bool await_ready() const noexcept { return false; }
                constexpr void await_resume() const noexcept {}

                void await_suspend(std::coroutine_handle<> handle) const {
                    queue._dispatcher->enqueue(handle);
                }
                const DispatchQueue& queue;
            };
            return Awaiter{ *this };
        }

        auto schedule(double after) const noexcept {
            struct [[nodiscard]] Awaiter {
                constexpr bool await_ready() const noexcept { return false; }
                constexpr void await_resume() const noexcept {}

                void await_suspend(std::coroutine_handle<> handle) const {
                    queue._dispatcher->enqueue(handle, after);
                }
                const DispatchQueue& queue;
                double after;
            };
            return Awaiter{ *this, after };
        }

        auto operator co_await() const noexcept {
            return schedule();
        }

        static DispatchQueue& main() noexcept;
        static DispatchQueue& global() noexcept;

        uint32_t numThreads() const noexcept { return _numThreads; }

        class _Dispatcher {
        public:
            virtual ~_Dispatcher() {}
            virtual uint32_t dispatch() = 0;
            virtual void wait() = 0;
            virtual bool wait(double timeout) = 0;
            virtual void notify() = 0;
            virtual void enqueue(std::coroutine_handle<>) = 0;
            virtual void enqueue(std::coroutine_handle<>, double) = 0;
            virtual bool isMain() const = 0;
            auto enter() noexcept {
                struct [[nodiscard]] Awaiter {
                    constexpr bool await_ready() const noexcept { return false; }
                    constexpr void await_resume() const noexcept {}
                    void await_suspend(std::coroutine_handle<> handle) const {
                        dispatcher->enqueue(handle);
                    }
                    _Dispatcher* dispatcher;
                };
                return Awaiter{ this };
            }
            // automatically destroyed when it finishes running.
            virtual void detach(std::coroutine_handle<>) = 0;
        };
        static std::shared_ptr<_Dispatcher> localDispatcher() noexcept;
        static std::shared_ptr<_Dispatcher> threadDispatcher(std::thread::id) noexcept;
        std::shared_ptr<_Dispatcher> dispatcher() const noexcept { return _dispatcher; }

        static bool isMainThread() noexcept;
        bool isMain() const noexcept { return _dispatcher->isMain(); }

        DispatchQueue& operator = (DispatchQueue&&) noexcept;
    private:
        struct _mainQueue {};
        DispatchQueue(_mainQueue) noexcept;
        void shutdown() noexcept;

        uint32_t _numThreads;
        std::atomic_flag _stopRequest;
        std::vector<std::thread> _threads;
        std::shared_ptr<_Dispatcher> _dispatcher;

        DispatchQueue(const DispatchQueue&) = delete;
        DispatchQueue& operator = (const DispatchQueue&) = delete;
    };

    CPP_CORO_DISPATCHQUEUE_EXPORT_API
    void setDispatchQueueMainThread();

    inline DispatchQueue& dispatchGlobal() noexcept {
        return DispatchQueue::global();
    }

    inline DispatchQueue& dispatchMain() noexcept {
        return DispatchQueue::main();
    }

    template <typename T> concept AsyncQueue = requires {
        {T::queue()}->std::convertible_to<DispatchQueue&>;
    };

    struct AsyncQueueGlobal {
        static DispatchQueue& queue() noexcept { return DispatchQueue::global(); }
    };

    struct AsyncQueueMain {
        static DispatchQueue& queue() noexcept { return DispatchQueue::main(); }
    };

    static_assert(AsyncQueue<AsyncQueueGlobal>);
    static_assert(AsyncQueue<AsyncQueueMain>);

    template <typename T>
    struct _AwaiterContinuation {
        constexpr bool await_ready() const noexcept { return false; }
        constexpr auto await_suspend(std::coroutine_handle<typename T::promise_type> handle) const noexcept {
            auto continuation = handle.promise().continuation;
            handle.promise().continuation = std::noop_coroutine();
            return continuation;
        }
        constexpr void await_resume() noexcept {}
    };

    template <typename T>
    struct _AwaiterCoroutineBase {
        constexpr bool await_ready() const noexcept {
            return !handle || handle.done();
        }
        constexpr auto await_suspend(std::coroutine_handle<> awaiting_coroutine) const noexcept {
            handle.promise().continuation = awaiting_coroutine;
            return handle;
        }
        std::coroutine_handle<typename T::promise_type> handle;
    };

    template <typename T>
    struct _PromiseBase {
        constexpr std::suspend_always initial_suspend() const noexcept {
            return {}; 
        }
        constexpr auto final_suspend() const noexcept {
            return _AwaiterContinuation<T>{};
        }
        constexpr T get_return_object() const noexcept {
            return T{ std::coroutine_handle<typename T::promise_type>::from_promise((typename T::promise_type&)*this) };
        }
        constexpr void unhandled_exception() noexcept {
            exception = std::current_exception();
        }
        constexpr void rethrow_exception_if_caught() const {
            if (exception)
                std::rethrow_exception(exception);
        }
        std::coroutine_handle<> continuation = std::noop_coroutine();
        std::exception_ptr exception;
    };

    CPP_CORO_DISPATCHQUEUE_EXPORT_API
    void _threadLocalDeferred(std::function<void()>);

    template <typename T>
    struct _AsyncAwaiterDispatchContinuation {
        constexpr bool await_ready() const noexcept {
            return false;
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<typename T::promise_type> handle) const noexcept {
            auto continuation = handle.promise().continuation;
            handle.promise().continuation = {};

            auto target = DispatchQueue::threadDispatcher(continuation.threadID);
            if (target == nullptr)
                return continuation.coroutine;
            auto current = DispatchQueue::localDispatcher();
            if (current == nullptr)
                return continuation.coroutine;
            if (target == current && target->isMain() == false)
                return continuation.coroutine;
            _threadLocalDeferred([=] { target->enqueue(continuation.coroutine); });
            return std::noop_coroutine();
        }
        constexpr void await_resume() const noexcept {}
    };

    template <typename T>
    struct _AsyncAwaiterDispatchCoroutineBase {
        constexpr bool await_ready() const noexcept {
            return !handle || handle.done();
        }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) {
            handle.promise().continuation.coroutine = awaiting_coroutine;
            handle.promise().continuation.threadID = std::this_thread::get_id();

            auto dispatcher = T::Queue::queue().dispatcher();
            if (dispatcher->isMain() == false && dispatcher == DispatchQueue::localDispatcher())
                return handle;
            dispatcher->enqueue(handle);
            return std::noop_coroutine();
        }
        std::coroutine_handle<typename T::promise_type> handle;
    };

    template <typename T>
    struct _AsyncPromiseBase {
        constexpr std::suspend_always initial_suspend() const noexcept {
            return {}; 
        }
        constexpr auto final_suspend() const noexcept {
            return _AsyncAwaiterDispatchContinuation<T>{};
        }
        constexpr T get_return_object() const noexcept {
            return T{ std::coroutine_handle<typename T::promise_type>::from_promise((typename T::promise_type&)*this) };
        }
        constexpr void unhandled_exception() noexcept {
            exception = std::current_exception();
        }
        constexpr void rethrow_exception_if_caught() const {
            if (exception)
                std::rethrow_exception(exception);
        }
        struct {
            std::coroutine_handle<> coroutine = std::noop_coroutine();
            std::thread::id threadID = {};
        } continuation;
        std::exception_ptr exception;
    };

    template <typename T = void> struct [[nodiscard]] Task {
        template <typename R> struct _Promise : _PromiseBase<Task> {
            template <std::convertible_to<R> V>
            constexpr void return_value(V&& value) noexcept {
                _result.emplace(std::forward<V>(value));
            }
            constexpr auto&& result() const {
                return _result.value();
            }
            std::optional<R> _result;
        };
        template <> struct _Promise<void> : _PromiseBase<Task> {
            constexpr void return_void() const noexcept {}
            constexpr void result() const noexcept {}
        };
        using promise_type = _Promise<T>;
        std::coroutine_handle<promise_type> handle;

        auto operator co_await() const noexcept {
            struct Awaiter : _AwaiterCoroutineBase<Task> {
                using _AwaiterCoroutineBase<Task>::handle;
                constexpr auto await_resume() -> decltype(handle.promise().result()) const {
                    handle.promise().rethrow_exception_if_caught();
                    return handle.promise().result();
                }
            };
            return Awaiter{ handle };
        }

        auto result() -> decltype(handle.promise().result()) const {
            return handle.promise().result();
        }

        bool done() const noexcept {
            return (!handle || handle.done());
        }

        explicit operator bool() const noexcept {
            return done() == false;
        }

        Task() = default;
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        Task(Task&& other) noexcept : handle{ other.handle } { other.handle = {}; }
        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                if (handle)
                    handle.destroy();
                handle = other.handle;
                other.handle = {};
            }
            return *this;
        }
        explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        ~Task() {
            if (handle)
                handle.destroy();
        }
    };

    template <typename T, AsyncQueue Q>
    struct [[nodiscard]] AsyncTask {
        using Queue = Q;
        template <typename R> struct _Promise : _AsyncPromiseBase<AsyncTask> {
            template <std::convertible_to<R> V>
            constexpr void return_value(V&& value) noexcept {
                _result.emplace(std::forward<V>(value));
            }
            constexpr auto&& result() const {
                return _result.value(); 
            }
            std::optional<R> _result;
        };
        template <> struct _Promise<void> : _AsyncPromiseBase<AsyncTask> {
            constexpr void return_void() const noexcept {}
            constexpr void result() const noexcept {}
        };
        using promise_type = _Promise<T>;
        std::coroutine_handle<promise_type> handle;

        auto operator co_await() const noexcept {
            struct Awaiter : _AsyncAwaiterDispatchCoroutineBase<AsyncTask> {
                using _AsyncAwaiterDispatchCoroutineBase<AsyncTask>::handle;
                constexpr auto await_resume() -> decltype(handle.promise().result()) const {
                    handle.promise().rethrow_exception_if_caught();
                    return handle.promise().result();
                }
            };
            return Awaiter{ handle };
        }

        auto result() -> decltype(handle.promise().result()) const {
            return handle.promise().result();
        }

        bool done() const noexcept {
            return (!handle || handle.done());
        }

        explicit operator bool() const noexcept {
            return done() == false;
        }

        AsyncTask() = default;
        AsyncTask(const AsyncTask&) = delete;
        AsyncTask& operator=(const AsyncTask&) = delete;
        AsyncTask(AsyncTask&& other) noexcept : handle{ other.handle } {
            other.handle = {};
        }
        AsyncTask& operator=(AsyncTask&& other) noexcept {
            if (this != &other) {
                if (handle)
                    handle.destroy();
                handle = other.handle;
                other.handle = {};
            }
            return *this;
        }
        explicit AsyncTask(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        ~AsyncTask() {
            if (handle)
                handle.destroy();
        }
    };

    template <typename T> requires (!std::same_as<T, void>)
    struct [[nodiscard]] Generator {
        struct promise_type : _PromiseBase<Generator> {
            template <std::convertible_to<T> R>
            constexpr auto yield_value(R&& v) noexcept {
                _value.emplace(std::forward<R>(v));
                return _AwaiterContinuation<Generator>{};
            }
            constexpr void return_void() const noexcept {}
            constexpr auto&& value() const {
                return _value.value(); 
            }
            std::optional<T> _value;
        };
        std::coroutine_handle<promise_type> handle;

        auto operator co_await() const noexcept {
            struct Awaiter : _AwaiterCoroutineBase<Generator> {
                constexpr bool await_resume() const noexcept {
                    this->handle.promise().rethrow_exception_if_caught();
                    return this->await_ready() == false;
                }
            };
            return Awaiter{ handle };
        }

        auto&& value() const {
            return handle.promise().value();
        }

        bool done() const noexcept {
            return (!handle || handle.done());
        }

        explicit operator bool() const noexcept {
            return done() == false;
        }

        Generator() = default;
        Generator(const Generator&) = delete;
        Generator& operator=(const Generator&) = delete;
        Generator(Generator&& other) noexcept : handle{ other.handle } {
            other.handle = {};
        }
        Generator& operator=(Generator&& other) noexcept {
            if (this != &other) {
                if (handle)
                    handle.destroy();
                handle = other.handle;
                other.handle = {};
            }
            return *this;
        }
        explicit Generator(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        ~Generator() {
            if (handle)
                handle.destroy();
        }
    };

    template <typename T, AsyncQueue Q = AsyncQueueGlobal> requires (!std::same_as<T, void>)
    struct [[nodiscard]] AsyncGenerator {
        using Queue = Q;
        struct promise_type : _AsyncPromiseBase<AsyncGenerator> {
            template <std::convertible_to<T> R>
            constexpr auto yield_value(R&& v) noexcept {
                value.emplace(std::forward<R>(v));
                return _AsyncAwaiterDispatchContinuation<AsyncGenerator>{};
            }
            constexpr void return_void() const noexcept {}
            std::optional<T> value;
        };
        std::coroutine_handle<promise_type> handle;

        auto operator co_await() const noexcept {
            struct Awaiter : _AsyncAwaiterDispatchCoroutineBase<AsyncGenerator> {
                constexpr bool await_resume() const noexcept {
                    this->handle.promise().rethrow_exception_if_caught();
                    return this->await_ready() == false;
                }
            };
            return Awaiter{ handle };
        }

        auto&& value() const {
            return handle.promise().value.value();
        }

        bool done() const noexcept {
            return (!handle || handle.done());
        }

        explicit operator bool() const noexcept {
            return done() == false;
        }

        AsyncGenerator() = default;
        AsyncGenerator(const AsyncGenerator&) = delete;
        AsyncGenerator& operator=(const AsyncGenerator&) = delete;
        AsyncGenerator(AsyncGenerator&& other) noexcept : handle{ other.handle } {
            other.handle = {};
        }
        AsyncGenerator& operator=(AsyncGenerator&& other) noexcept {
            if (this != &other) {
                if (handle)
                    handle.destroy();
                handle = other.handle;
                other.handle = {};
            }
            return *this;
        }
        explicit AsyncGenerator(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
        ~AsyncGenerator() {
            if (handle)
                handle.destroy();
        }
    };

    template <AsyncQueue Q>
    inline auto detachedTask(AsyncTask<void, Q>&& task) {
        Q::queue().dispatcher()->detach(task.handle);
        task.handle = {};
    }
    template <AsyncQueue Q>
    inline auto detachedTask(AsyncTask<void, Q>& task) {
        Q::queue().dispatcher()->detach(task.handle);
        task.handle = {};
    }
    template <std::convertible_to<Task<void>> T>
    inline auto detachedTask(T&& task, DispatchQueue& queue = dispatchGlobal()) {
        queue.dispatcher()->detach(task.handle);
        task.handle = {};
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline AsyncTask<void, Q> asyncSleep(double t) {
        co_await Q::queue().schedule(t);
        co_return;
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline AsyncTask<void, Q> asyncYield() {
        co_await Q::queue().schedule();
        co_return;
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline AsyncTask<void, Q> asyncWaitQueue(DispatchQueue& queue) {
        co_await queue;
        assert(queue.dispatcher() == DispatchQueue::localDispatcher());
        queue.dispatcher()->wait();
        co_return;
    }

    template <typename _Task> struct _TaskGroup {
        std::vector<_Task> tasks;

        template <std::convertible_to<_Task> ... Ts>
        constexpr _TaskGroup(Ts&&... ts) {
            tasks.reserve(sizeof...(Ts));
            (tasks.emplace_back(std::forward<Ts>(ts)), ...);
        }

        template <std::convertible_to<_Task> ... Ts>
        constexpr void emplace(Ts&&... ts) {
            tasks.reserve(tasks.size() + sizeof...(Ts));
            (tasks.emplace_back(std::forward<Ts>(ts)), ...);
        }

        template <std::convertible_to<_Task> ... Ts>
        _TaskGroup& operator << (Ts&&... ts) {
            emplace(std::forward<Ts>(ts)...);
            return *this;
        }

        constexpr size_t size() const noexcept {
            return tasks.size();
        }

        _TaskGroup(const _TaskGroup&) = delete;
        _TaskGroup& operator = (const _TaskGroup&) = delete;
        _TaskGroup(_TaskGroup&& tmp) : tasks(std::move(tmp.tasks)) {}
        _TaskGroup& operator = (_TaskGroup&& other) {
            tasks = std::move(other.tasks);
            return *this;
        }
    };

    template <typename T = void>
    using TaskGroup = _TaskGroup<Task<T>>;

    template <typename T = void, AsyncQueue Q = AsyncQueueGlobal>
    using AsyncTaskGroup = _TaskGroup<AsyncTask<T, Q>>;

    template <AsyncQueue Q, typename Group>
    inline Task<void> _async(Group group) {
        if (group.tasks.empty())
            co_return;

        auto& tasks = group.tasks;
        auto& queue = Q::queue();
        std::for_each(tasks.begin(), tasks.end(), [&](auto& task) {
            queue.schedule().await_suspend(task.handle);
        });

        while (1) {
            bool done = true;
            for (auto& task : tasks) {
                if (task.handle.done() == false) {
                    done = false;
                    break;
                }
            }
            if (done)
                break;
            co_await asyncYield();
        }
        co_return;
    }

    template <typename T, AsyncQueue Q, typename Group>
    inline Generator<T> _async(Group group) {
        if (group.tasks.empty())
            co_return;

        auto& tasks = group.tasks;
        auto& queue = Q::queue();
        std::for_each(tasks.begin(), tasks.end(), [&](auto& task) {
            queue.schedule().await_suspend(task.handle);
        });
        while (tasks.empty() == false) {
            if (tasks.front().handle.done()) {
                co_yield tasks.front().result();
                tasks.erase(tasks.begin());
            }
            co_await asyncYield();
        }
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline Task<void> async(TaskGroup<void>& group) {
        return _async<Q>(std::move(group));
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline Task<void> async(TaskGroup<void>&& group) {
        return _async<Q>(std::move(group));
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline Task<void> async(AsyncTaskGroup<void, Q>& group) {
        return _async<Q>(std::move(group));
    }

    template <AsyncQueue Q = AsyncQueueGlobal>
    inline Task<void> async(AsyncTaskGroup<void, Q>&& group) {
        return _async<Q>(std::move(group));
    }

    template <typename T, AsyncQueue Q = AsyncQueueGlobal>
    inline Generator<T> async(TaskGroup<T>& group) {
        return _async<T, Q>(std::move(group));
    }

    template <typename T, AsyncQueue Q = AsyncQueueGlobal>
    inline Generator<T> async(TaskGroup<T>&& group) {
        return _async<T, Q>(std::move(group));
    }

    template <typename T, AsyncQueue Q = AsyncQueueGlobal>
    inline Generator<T> async(AsyncTaskGroup<T, Q>& group) {
        return _async<T, Q>(std::move(group));
    }

    template <typename T, AsyncQueue Q = AsyncQueueGlobal>
    inline Generator<T> async(AsyncTaskGroup<T, Q>&& group) {
        return _async<T, Q>(std::move(group));
    }

    template <typename T = void>
    using Async = AsyncTask<T, AsyncQueueGlobal>;

    template <typename T = void>
    using AsyncMain = AsyncTask<T, AsyncQueueMain>;

#ifdef CPP_CORO_DISPATCHQUEUE_IMPLEMENTATION
    namespace {
        static bool _initializedLocalStorage;
        struct _local { // linkage local storage
            std::unordered_map<std::thread::id, std::weak_ptr<DispatchQueue::_Dispatcher>> dispatchers;
            std::weak_ptr<DispatchQueue::_Dispatcher> mainDispatcher;
            std::thread::id mainThreadID;
            std::mutex mutex;
            std::unordered_set<std::coroutine_handle<>> detachedCoroutines;
            std::unordered_map<std::thread::id, std::vector<std::function<void()>>> threadLocalDeferred;
            _local() { _initializedLocalStorage = true; }
            ~_local() { _initializedLocalStorage = false; }
            _local(_local&) = delete;
            _local(_local&&) = delete;
            static _local& get() {
                static _local local{};
                return local;
            }
        };

        class Dispatcher : public DispatchQueue::_Dispatcher {
        public:
            using Clock = std::chrono::steady_clock;
            struct Task {
                std::coroutine_handle<> coroutine;
                std::chrono::time_point<Clock> timepoint;
            };
            std::deque<Task> tasks;
            struct {
                std::mutex mutex;
                std::condition_variable cv;
            } cond;

            uint32_t dispatch() override {
                uint32_t fetch = 0;
                Task task{};
                do {
                    auto lock = std::scoped_lock{ cond.mutex };
                    if (tasks.empty() == false &&
                        tasks.front().timepoint <= Clock::now()) {
                        task = tasks.front();
                        tasks.pop_front();
                        fetch += 1;
                    }
                } while (0);
                if (task.coroutine) {
                    task.coroutine.resume();

                    if (task.coroutine.done()) {
                        auto& local = _local::get();
                        auto lock = std::scoped_lock{ local.mutex };
                        if (local.detachedCoroutines.contains(task.coroutine)) {
                            local.detachedCoroutines.erase(task.coroutine);
                            task.coroutine.destroy();
                        }
                    }
                }
                std::vector<std::function<void()>> deferred;
                do {
                    auto& local = _local::get();
                    auto lock = std::scoped_lock{ local.mutex };
                    if (auto it = local.threadLocalDeferred.find(std::this_thread::get_id());
                        it != local.threadLocalDeferred.end()) {
                        it->second.swap(deferred);
                        it->second.clear();
                    }
                } while (0);
                std::for_each(deferred.begin(), deferred.end(), [](auto&& fn) { fn(); });
                return fetch;
            }

            void enqueue(std::coroutine_handle<> coro) override {
                auto tp = Clock::now();
                auto lock = std::unique_lock{ cond.mutex };
                auto pos = std::lower_bound(tasks.begin(), tasks.end(), tp,
                                            [](const auto& value, const auto& tp) {
                    return value.timepoint < tp;
                });
                tasks.emplace(pos, coro, tp);
                cond.cv.notify_all();
            }

            void enqueue(std::coroutine_handle<> coro, double t) override {
                auto offset = std::chrono::duration<double>(std::max(t, 0.0));
                auto timepoint = Clock::now() + offset;
                auto tp = std::chrono::time_point_cast<Clock::duration>(timepoint);
                auto lock = std::unique_lock{ cond.mutex };
                auto pos = std::lower_bound(tasks.begin(), tasks.end(), tp,
                                            [](const auto& value, const auto& tp) {
                    return value.timepoint < tp;
                });
                tasks.emplace(pos, coro, tp);
                cond.cv.notify_all();
            }

            void detach(std::coroutine_handle<> coro) override {
                if (coro) {
                    enqueue(coro);
                    auto& local = _local::get();
                    auto lock = std::unique_lock{ local.mutex };
                    local.detachedCoroutines.insert(coro);
                }
            }

            void wait() override {
                auto lock = std::unique_lock{ cond.mutex };
                if (tasks.empty() == false) {
                    std::chrono::duration<double> interval = tasks.front().timepoint - Clock::now();
                    if (interval.count() > 0)
                        cond.cv.wait_for(lock, interval);
                } else {
                    cond.cv.wait(lock);
                }
            }

            bool wait(double timeout) override {
                auto offset = std::chrono::duration<double>(std::max(timeout, 0.0));
                auto tp = std::chrono::time_point_cast<Clock::duration>(Clock::now() + offset);
                auto lock = std::unique_lock{ cond.mutex };
                if (tasks.empty() == false && tasks.front().timepoint < tp) {
                    std::chrono::duration<double> interval = tasks.front().timepoint - Clock::now();
                    if (interval.count() > 0) {
                        cond.cv.wait_for(lock, interval);
                    }
                    return true;
                }
                return cond.cv.wait_until(lock, tp) == std::cv_status::no_timeout;
            }

            void notify() override {
                cond.cv.notify_all();
            }

            bool isMain() const override {
                return _local::get().mainDispatcher.lock().get() == this;
            }
        };

        void setThreadDispatcher(std::shared_ptr<DispatchQueue::_Dispatcher> dispatcher) {
            auto threadID = std::this_thread::get_id();
            auto& local = _local::get();
            auto lock = std::unique_lock{ local.mutex };
            if (dispatcher) {
                local.dispatchers.emplace(threadID, dispatcher);
            } else {
                if (_initializedLocalStorage) {
                    local.dispatchers.erase(threadID);
                    local.threadLocalDeferred.erase(threadID);
                } else {
                    // app is begin terminated or unloading DLL. do nothing.
                }
            }
        }

        std::shared_ptr<DispatchQueue::_Dispatcher> getThreadDispatcher(std::thread::id threadID) {
            if (threadID != std::thread::id()) {
                auto& local = _local::get();
                auto lock = std::unique_lock{ local.mutex };

                if (threadID == local.mainThreadID) {
                    return local.mainDispatcher.lock();
                }
                if (auto iter = local.dispatchers.find(threadID);
                    iter != local.dispatchers.end()) {
                    return iter->second.lock();
                }
            }
            return nullptr;
        }
    }

    CPP_CORO_DISPATCHQUEUE_EXPORT_API
    void setDispatchQueueMainThread() {
        auto& local = _local::get();
        auto lock = std::unique_lock{ local.mutex };
        local.mainThreadID = std::this_thread::get_id();
        lock.unlock();
    }
    CPP_CORO_DISPATCHQUEUE_EXPORT_API
    void _threadLocalDeferred(std::function<void()> fn) {
        if (fn) {
            auto& local = _local::get();
            auto lock = std::unique_lock{ local.mutex };
            local.threadLocalDeferred[std::this_thread::get_id()].push_back(fn);
        }
    }
    
    DispatchQueue::DispatchQueue(_mainQueue) noexcept
        : _numThreads(1) {
        this->_dispatcher = std::make_shared<Dispatcher>();
        auto& local = _local::get();
        auto lock = std::unique_lock{ local.mutex };
        local.mainDispatcher = this->_dispatcher;
    }
    
    DispatchQueue::DispatchQueue(uint32_t maxThreads) noexcept
        : _numThreads(std::max(maxThreads, 1U)) {
        _dispatcher = std::make_shared<Dispatcher>();

        auto work = [this]{
            setThreadDispatcher(_dispatcher);
            while (!_stopRequest.test()) {
                if (_dispatcher->dispatch() == 0)
                    _dispatcher->wait();
            }
            setThreadDispatcher(nullptr);
        };

        this->_threads.reserve(_numThreads);
        this->_stopRequest.clear();
        for (uint32_t i = 0; i < _numThreads; ++i)
            this->_threads.push_back(std::thread(work));
    }
    
    DispatchQueue::DispatchQueue(DispatchQueue&& tmp) noexcept
        : _numThreads(tmp._numThreads)
        , _threads(std::move(tmp._threads))
        , _dispatcher(std::move(tmp._dispatcher)) {
        tmp._threads.clear();
        tmp._dispatcher = {};
        tmp._numThreads = 0;
    }
    
    DispatchQueue& DispatchQueue::operator = (DispatchQueue&& tmp) noexcept {
        shutdown();
        this->_threads = std::move(tmp._threads);
        this->_dispatcher = std::move(tmp._dispatcher);
        this->_numThreads = tmp._numThreads;
        tmp._threads.clear();
        tmp._dispatcher = {};
        tmp._numThreads = 0;
        return *this;
    }
    
    void DispatchQueue::shutdown() noexcept {
        if (_threads.empty() == false) {
            _stopRequest.test_and_set();
            _stopRequest.notify_all();

            _dispatcher->notify();

            for (auto& t : _threads)
                t.join();
        }
        _threads.clear();
        _dispatcher = nullptr;
        _numThreads = 0;
    }
    
    DispatchQueue& DispatchQueue::main() noexcept {
        static DispatchQueue queue(_mainQueue{});
        return queue;
    }
    
    DispatchQueue& DispatchQueue::global() noexcept {
        static uint32_t maxThreads =
#ifdef CPP_CORO_DISPATCHQUEUE_MAX_GLOBAL_THREADS
        (uint32_t)std::max(int(CPP_CORO_DISPATCHQUEUE_MAX_GLOBAL_THREADS), 1);
#else
            std::max(std::jthread::hardware_concurrency(), 3U) - 1;
#endif
        static DispatchQueue queue(maxThreads);
        return queue;
    }

    std::shared_ptr<DispatchQueue::_Dispatcher>
        DispatchQueue::threadDispatcher(std::thread::id threadID) noexcept {
        return getThreadDispatcher(threadID);
    }

    std::shared_ptr<DispatchQueue::_Dispatcher>
        DispatchQueue::localDispatcher() noexcept {
        return getThreadDispatcher(std::this_thread::get_id());
    }

    bool DispatchQueue::isMainThread() noexcept {
        return std::this_thread::get_id() == _local::get().mainThreadID;
    }
#endif

#ifdef CPP_CORO_DISPATCHQUEUE_NAMESPACE
}
#endif

#ifdef _POP_MACRO_MAX
#pragma pop_macro("max")
#endif
