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

struct Task;

struct Step
{
    std::string name;
    std::function<void()> run;

    std::atomic_int remainingDependencies;
    std::vector<std::shared_ptr<Step>> dependents;

    bool complete = false;
    std::mutex mutex;

    bool await_ready();
    void await_resume() noexcept;
    void await_suspend(Task task);

    void Wait();
};

std::shared_ptr<Step>& CurrentStep()
{
    thread_local std::shared_ptr<Step> current_step;
    return current_step;
}

void Step::Wait()
    {
        if (CurrentStep()) {
            std::println("Cannot call blocking Step::Wait from step running on worker thread! Use co_await or schedule a new task");
            std::terminate();
        }

        std::scoped_lock lock{ mutex };
        if (complete) return;

        std::atomic_bool done = false;

        auto step = std::shared_ptr<Step>(new Step {
            .run = [&] {
                done = true;
                done.notify_all();
            },
            .remainingDependencies = 1,
        });

        dependents.emplace_back(step);

        done.wait(false);
    }

struct Executor
{
    std::vector<std::jthread> workers;
    std::deque<std::shared_ptr<Step>> queue;
    std::mutex mutex;
    std::condition_variable cv;

    bool stop = false;

    Executor(uint32_t num_threads)
    {
        for (uint32_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this, i] { Worker(i); });
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

    void Enqueue(std::shared_ptr<Step> step)
    {
        std::println("Enqueing: {}", step->name);

        {
            std::scoped_lock lock{ mutex };
            queue.push_back(std::move(step));
        }
        cv.notify_one();
    }

    void Worker(uint32_t id)
    {
        for (;;) {
            std::shared_ptr<Step> step;

            {
                std::unique_lock lock{ mutex };

                cv.wait(lock, [&]{
                    return !queue.empty() || stop;
                });

                if (stop) {
                    std::println("Stop signal received, worker [{}] shutting down", id);
                    return;
                }

                step = std::move(queue.front());
                queue.pop_front();
            }

            std::println("[{}] Running step: {}", id, step->name);

            CurrentStep() = step;
            step->run();

            std::scoped_lock step_lock{ step->mutex };
            step->complete = true;

            for (auto& dep : step->dependents) {
                if (!--dep->remainingDependencies) {
                    Enqueue(dep);
                }
            }
        }
    }
} exec(4);

std::atomic_uint64_t id = 0;
std::string StepName() { return std::format("Step-{}", ++id); }

std::shared_ptr<Step> Do(std::function<void()> func)
{
    auto task = std::shared_ptr<Step>(new Step{
        .name = StepName(),
        .run = std::move(func),
    });
    exec.Enqueue(task);
    return task;
}

template<typename T>
std::shared_ptr<Step> AfterAll(std::span<std::function<T()>> fn, std::shared_ptr<Step> after)
{
    std::vector<std::shared_ptr<Step>> prequisites;

    for (auto& f : fn) {
        prequisites.emplace_back(new Step {
            .name = StepName(),
            .run = std::move(f),
            .dependents = { after }
        });
    }

    after->remainingDependencies = uint32_t(prequisites.size());

    for (auto& preq : prequisites) {
        exec.Enqueue(std::move(preq));
    }

    return after;
}

// -----------------------------------------------------------------------------

struct Task
{
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type
    {
        std::atomic_uint32_t ref_count = 0;

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

        auto initial_suspend() { return std::suspend_never(); }
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
};

std::shared_ptr<Step> Coro(std::function<Task()> coro)
{
    return Do([coro = std::move(coro)]() mutable {
        coro();
    });
}

bool Step::await_ready()
{
    return false;
}

void Step::await_resume() noexcept {}

void Step::await_suspend(Task task)
{
    auto cur_step = CurrentStep();
    auto step = std::shared_ptr<Step>(new Step {
        .run = [task = std::move(task)] {
            task.handle.resume();
        },
        .dependents = std::move(cur_step->dependents),
    });

    std::scoped_lock lock{ mutex };
    if (complete) {
        exec.Enqueue(std::move(step));
    } else {
        step->remainingDependencies = 1;
        dependents.emplace_back(step);
    }
}

struct AllOfHelper
{
    std::span<std::function<Task()>> tasks;

    void await_resume() noexcept {}
    bool await_ready() { return false; }
    void await_suspend(Task task)
    {
        auto cur_step = CurrentStep();
        auto step = std::shared_ptr<Step>(new Step {
            .run = [task = std::move(task)] {
                task.handle.resume();
            },
            .dependents = std::move(cur_step->dependents),
        });

        AfterAll<Task>(tasks, step);
    }
};

AllOfHelper AllOf(std::vector<std::function<Task()>>&& fn)
{
    return AllOfHelper(fn);
}

// -----------------------------------------------------------------------------

int main()
{
    // Do([] {
    //     std::println("A");

    //     AfterAll(
    //         {
    //             [] { std::println("B"); },
    //             [] { std::println("C"); },
    //         },
    //         [] { std::println("D"); });
    // });

    Coro([]() -> Task {
        std::println("A");

        co_await AllOf({
            []() -> Task {
                std::println("B");
                co_await AllOf({
                    []() -> Task { std::println("B.1"); std::this_thread::sleep_for(200ms); co_return; },
                    []() -> Task { std::println("B.2"); std::this_thread::sleep_for(200ms); co_return; },
                });
                std::println("B Done");
            },
            []() -> Task { std::println("C"); std::this_thread::sleep_for(100ms); co_return; },
        });

        std::println("D");

        co_await AllOf({
            []() -> Task { std::println("E"); std::this_thread::sleep_for(100ms); co_return; },
            []() -> Task { std::println("F"); std::this_thread::sleep_for(100ms); co_return; },
        });

        std::println("G");
    })->Wait();

    std::println("All Complete");
}