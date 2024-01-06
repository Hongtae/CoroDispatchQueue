#include <format>
#include <chrono>
#include <random>
#include <iostream>

#define CPP_CORO_DISPATCHQUEUE_IMPLEMENTATION
#define CPP_CORO_DISPATCHQUEUE_MAX_GLOBAL_THREADS 2 /*std::jthread::hardware_concurrency()*/
#include "DispatchQueue.h"


#if __cplusplus < 202302L // no formatter for std::thread::id (C++20)
#include <sstream>
namespace std {
    template <> struct formatter<std::thread::id> : formatter<string> {
        auto format(const std::thread::id& arg, format_context& ctx) const {
            std::ostringstream oss;
            oss << arg;
            auto str = std::format("{}", oss.str());
            return formatter<string>::format(str, ctx);
        }
    };
}
#endif


template <typename... Types>
void print(const std::format_string<Types...> fmt, Types&&... args) {
    auto s = std::format("[{}] {}\n", std::this_thread::get_id(),
                         std::format(fmt, std::forward<Types>(args)...));
    printf("%s", s.c_str());
}

template <typename T, typename D>
constexpr auto duration(std::chrono::duration<T, D> d) {
    return std::chrono::duration<double>(d).count();
};

using Clock = std::chrono::high_resolution_clock;

std::random_device r{};
std::default_random_engine random(r());

std::atomic_flag flag;

struct CustomQueue {
    static DispatchQueue& queue() {
        static DispatchQueue queue(1);
        return queue;
    }
};

Task<int> syncTest2(int a, int b) {
    print("syncTest2 start");
    std::uniform_real_distribution<double> distrib(2.0, 6.0);
    auto s = double(distrib(random));
    print("syncTest2(): sleeping {:.3f} seconds", s);
    co_await asyncSleep(s);

    co_return a + b;
}

Task<> syncTest1() {
    co_await syncTest2(1, 2);
    co_await asyncYield();
}

AsyncGenerator<int> test2(int n) {
    print("test2(): start");

    print("test2(): Sleep for 2 seconds on an unknown thread.");
    co_await CustomQueue::queue().schedule(2.0);
    print("test2(): awaken.");

    for (int i = 0; i < 5; ++i)
        co_yield n + i;

    print("test2(): end.");
}

Async<int> test4(int n) {
    print("test4(): start");

    std::uniform_real_distribution<double> distrib(2.0, 6.0);
    auto s = double(distrib(random));
    print("test4(): sleeping {:.3f} seconds", s);
    co_await asyncSleep(s);

    co_return n;
}

Async<> test3(std::atomic<int>& counter) {
    print("test3(): start");

    std::uniform_real_distribution<double> distrib(3.0, 10.0);
    auto s = double(distrib(random));
    print("test3(): sleeping {:.3f} seconds", s);
    co_await asyncSleep(s);

    print("test3(): end. slept {:.3f} seconds.", s);
    co_return (void)counter.fetch_add(1);
}

Generator<int> testGen(int n) {
    for (int i = 0; i < 10; ++i)
        co_yield n + i;
}

Async<> test1() {
    print("test1(): start");

    print("test1(): Generator test.");
    auto gen = testGen(3);
    while (co_await gen) {
        print("generator yield: {}", gen.value());
    }

    co_await dispatchGlobal();
    print("test1(): switch - global");

    co_await dispatchMain();
    print("test1(): switch - main");

    print("test1(): waiting for test2()");
    auto task = test2(1);
    while (task) {
        int x = co_await task;
        print("co_await test2() yield: {}", x);
    }

    std::atomic<int> counter = 0;
    print("testing multiple tasks");
    AsyncTaskGroup<> tasks{
        test3(counter),
        test3(counter),
        test3(counter),
    };

    auto numTasks = tasks.size();
    co_await async(tasks);

    print("multiple tasks completed:{}, total:{}", counter.load(), numTasks);

    print("multiple tasks with generator.");
    AsyncTaskGroup<int> tasks2{
        test4(0),
        test4(1),
        test4(2)
    };
    auto x = async(tasks2);
    while (co_await x) {
        print("async result: {}", x.value());
    }

    co_await syncTest1();

    print("test1(): end. - signal to main thread");

    flag.test_and_set();
    flag.notify_all();

    // wait up main thread
    co_await DispatchQueue::main();
}

int main() {
    print("main thread:{}", std::this_thread::get_id());
    setDispatchQueueMainThread(); // to enable main-queue

    auto t1 = Clock::now();

    detachedTask(syncTest1());

    detachedTask(test1());

    print("waiting...");

    // activate main loop
    auto& dq = DispatchQueue::main();
    while (flag.test() == false) {
        if (dq.dispatcher()->dispatch() == 0) {
            dq.dispatcher()->wait();
            //std::this_thread::yield();
        }
    }

    auto t2 = Clock::now();
    print("done. terminate. (elapsed: {:.3f} seconds)", duration(t2 - t1));
}
