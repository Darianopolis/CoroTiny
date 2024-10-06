#include <vector>
#include <memory>
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <print>
#include <chrono>
#include <coroutine>
#include <span>

using namespace std::literals;

// -----------------------------------------------------------------------------

#ifdef _MSC_VER
#include <shared_mutex>
using Mutex = std::shared_mutex;
using CV = std::condition_variable_any;
#else
using Mutex = std::mutex;
using CV = std::condition_variable;
#endif

// -----------------------------------------------------------------------------

static const uint32_t num_threads = std::min(4u, std::thread::hardware_concurrency());

// -----------------------------------------------------------------------------

static std::atomic_uint32_t next_task_id = 0;

struct Task
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct alignas(64) promise_type
    {
        Mutex mutex;

        std::uint32_t id = next_task_id++;

        std::atomic_uint32_t ref_count = 0;

        std::vector<Task> dependents;
        std::atomic_int remainingDependencies;

        bool complete = false;

        void acquire()
        {
            ref_count++;
        }

        void release()
        {
            if (!--ref_count) {
                handle_type::from_promise(*this).destroy();
            }
        }

        Task get_return_object() {
            return Task(*this);
        }

        auto initial_suspend() {
            // Optimize by only suspending initially in "defer" context
            return std::suspend_always();
        }
        auto final_suspend() noexcept { return std::suspend_always(); }
        auto return_void() {}
        void unhandled_exception() {}

        ~promise_type() {}
    };

    Task()
    {}

    Task(handle_type _handle)
        : handle(_handle)
    {
        handle.promise().acquire();
    }

    Task(promise_type& promise)
    {
        handle = handle_type::from_promise(promise);
        promise.acquire();
    }

    Task(const Task& other)
    {
        handle = other.handle;
        handle.promise().acquire();
    }

    Task& operator=(const Task& other)
    {
        if (this != &other) {
            this->~Task();
            new(this) Task(other);
        }
        return *this;
    }

    Task(Task&& other)
    {
        handle = other.handle;
        other.handle = {};
    }

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other) {
            this->~Task();
            new(this) Task(std::move(other));
        }
        return *this;
    }

    ~Task()
    {
        if (handle) {
            handle.promise().release();
        }
    }

    handle_type handle{};

    promise_type* operator->() const noexcept
    {
        return &handle.promise();
    }

    promise_type& promise() const noexcept
    {
        return handle.promise();
    }

    void wait()
    {
        std::atomic_bool done = false;

        {
            std::scoped_lock lock{ promise().mutex };
            if (promise().complete) return;

            auto task = [](auto& done) -> Task {
                done = true;
                done.notify_all();

                co_return;
            }(done);

            task->remainingDependencies = 1;
            promise().dependents.emplace_back(std::move(task));
        }

        done.wait(false);
    }

    // Make every Task a valid functor that returns itself
    decltype(auto) operator()() const noexcept
    {
        return *this;
    }
};

// -----------------------------------------------------------------------------

struct Executor
{
    std::vector<std::jthread> workers;
    std::deque<Task> queue;
    Mutex mutex;
    CV cv;
    bool stop = false;

    Executor(uint32_t num_workers)
    {
        for (uint32_t i = 0; i < num_workers; ++i) {
            workers.emplace_back([this, i] { worker(i); });
        }
    }

    ~Executor()
    {
        {
            std::scoped_lock lock{ mutex };
            stop = true;
        }
        cv.notify_all();
        std::println("Clearing out workers");
        workers.clear();
        std::println("Complete");
    }

    void enqueue(Task task)
    {
        std::println("Enqueing: {}", task->id);

        {
            std::scoped_lock lock{ mutex };
            queue.push_back(std::move(task));
        }
        cv.notify_one();
    }

    void worker(uint32_t id)
    {
        for (;;) {
            Task task;

            {
                std::unique_lock queue_lock{ mutex };

                cv.wait(queue_lock, [&]{
                    return !queue.empty() || stop;
                });

                if (stop) {
                    std::println("Stop signal received, worker [{}] shutting down", id);
                    return;
                }

                task = std::move(queue.front());
                queue.pop_front();
            }

            std::println("[{}] Resuming task: {}", id, task->id);

            task.handle.resume();

            if (task.handle.done()) {
                std::scoped_lock task_lock{ task->mutex };
                task->complete = true;

                for (auto& dep : task->dependents) {
                    if (!--dep->remainingDependencies) {
                        enqueue(std::move(dep));
                    }
                }
            }
        }
    }
} exec(num_threads);

// -----------------------------------------------------------------------------

template<typename ...Fns>
struct AllOf
{
    std::tuple<Fns&&...> task_fns;

    AllOf(Fns&& ...fns)
        : task_fns{std::make_tuple(fns...)}
    {}

    void await_resume() noexcept {}
    bool await_ready() { return false; }
    void await_suspend(Task cur_task)
    {
        cur_task->remainingDependencies = uint32_t(std::tuple_size_v<decltype(task_fns)>);
        std::apply([&](auto&& ...fns) {
            ([&](auto&& fn) {
                auto task = fn();
                task->dependents.emplace_back(cur_task);
                exec.enqueue(std::move(task));
            }(fns), ...);
        }, task_fns);
    }
};

// -----------------------------------------------------------------------------

Task bar() { std::println("B.3"); co_return; }

auto run() -> Task
{
    std::println("A");

    co_await AllOf {
        []() -> Task {
                std::println("B");
                co_await AllOf {
                    []() -> Task { std::println("B.1"); std::this_thread::sleep_for(200ms); co_return; },
                    []() -> Task { std::println("B.2"); std::this_thread::sleep_for(200ms); co_return; },
                    bar(),
                };
                std::println("B Done");
        },
        []() -> Task { std::println("C"); std::this_thread::sleep_for(100ms); co_return; },
    };

    std::println("D");

    co_await AllOf {
        []() -> Task { std::println("E"); std::this_thread::sleep_for(100ms); co_return; },
        []() -> Task { std::println("F"); std::this_thread::sleep_for(100ms); co_return; },
    };

    std::println("G");
}

int main()
{
    auto task = run();
    exec.enqueue(task);
    task.wait();

    std::println("All Complete");
}