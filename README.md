# Cpp Coroutine Dispatch Queue
C++ concurrency library with C++20 coroutines.

This project is a single header library.  
Just include ["DispatchQueue.h"](DispatchQueue.h) in your project.  

See the example file for more details: [Sample code (test.cpp)](test.cpp)

> [!NOTE]  
> The sample project is for Visual Studio 2022.

**Requires a C++20 or later compiler.**

## Types
- Task
  - Common coroutine function type
    ```
    Task<int> getValue() {
        co_return 1;
    }

    int n = co_await getValue();
    ```
    
- Generator
   - Function type that yields multiple values without returning
     ```
     Generator<int> gen(int n) {
         for (int i = 0; i < n; ++i)
             co_yield i;
     }

     for (auto g = gen(10); co_await g; ) {
         std::cout << "value: " << g.value();
     }
     ```
- AsyncTask
   - Asynchronous function type that runs on a specific dispatch queue.
   - When it completes execution, it returns to the initial dispatch queue.
     ```
     AsyncTask<int> getValueThread() {
         std::cout << "this_thread: " << std::this_thread::get_id();
         co_return 0;
     }

     // Running on a background dispatch-queue and then returning to the original dispatch-queue.
     // NOTE: Returning to the same dispatch queue does not mean the same thread.
     //  If the current dispatch queue owns multiple threads, it may return to a different thread than it was before the call.
     int value = getValueThread(); 
     ```
- AsyncGenerator
  - Asynchronous function type that runs on a specific dispatch queue.
  - This function can yield multiple values to the caller without returning.
    ```
    AsyncGenerator<int> gen(int n) {
        for (int i = 0; i < n; ++i) {
            std::cout << "callee-thread: " << std::this_thread::get_id();
            co_yield i;
        }
    }

    for (auto g = gen(10); co_await g; ) {
        std::cout << "caller-thread: " << std::this_thread::get_id();
        std::cout << "value: " << g.value();
     }
    ```
- DispatchQueue
   - A queue that manages threads. A queue can have multiple threads.
   - See the source code for a detailed implementation.

## Usage
```
// async function using background thread.
Async<> func1() {
    co_await asyncSleep(10.0);  // sleep 10s
    co_return;
}

// async function that returns int.
Async<int> func2() {
    int n = co_await doSomething();
    co_return n;
}

// a coroutine function that returns float.
Task<float> func3() {
    co_return 1.0f;
}
```

```
// async generator to yield some values
AsyncGenerator<int> asyncGen(int n) {
    for (int i = 0; i < n; ++i)
        co_yield i;
}

auto x = asyncGen(10);
while (co_await x) {
    printf("yield: %d", x.value());
}
```

```

// coroutine generator to yield INT values
Generator<int> generator(int n) {
    for (int i = 0; i < n; ++i)
        co_yield i;
}

auto x = generator(3);
while (co_await x) {
    printf("yield: %d", x.value());
}
```

```
Async<> test() {
    co_await asyncSleep(10.0);
    co_return;
}

// run a new task in a background thread
dispatchTask(test());

// run multiple new tasks in a background thread
auto group = AsyncTaskGroup{ test(), test(), test() }
co_await async(group);
```

```
// run multiple new tasks that return values in a background thread
Async<int> func1(int n) {
    co_return n * 2;
}

auto group = AsyncTaskGroup({ func1(3), func1(10), func1(20) });
auto x = co_await async(group);
while (co_await x) {
    printf("yield: %d", x.value());
}
```

```
// switching the running task thread

DispatchQueue myQueue(3);  // my queue with 3 threads

Async<> work() {
    std::cout << std::this_thread::get_id() << std::end;

    co_await myQueue;       // switching thread.

    std::cout << std::this_thread::get_id() << std::end;

    co_return;
}
```
### Registering main thread
- To use dispatchMain(), the main thread must be enabled.
  ```
  std::atomic_flag stop_running;
  
  int main() {
      setDispatchQueueMainThread(); // Set the current thread as the main thread.

     // activate main loop
     auto& dq = DispatchQueue::main();
     while (!stop_running.test()) {
     if (dq.dispatcher()->dispatch() == 0) {
        dq.dispatcher()->wait();
    }
  }
  ```

## Tips & Tricks
#### Using Win32 Main-thread as MainQueue
- What if you can't control the main message loop?
  - Use timer, run the following code once on the main thread.
    ```
    TIMERPROC timerProc = [](HWND, UINT, UINT_PTR, DWORD) {
        auto& dq = dispatchMain();
        while (dq.dispatcher()->dispatch()) {}
    };
    setDispatchQueueMainThread();
    SetTimer(nullptr, 0, USER_TIMER_MINIMUM, timerProc);
    ```
    - Install a timer in the main loop. With TIMEPROC, it is not affected by the modal-state of the Win32 message loop.
    - Using Win32 Timer will result in a resolution of 10ms minimum, but we don't expect this to be a problem for asynchronous function execution in general.
