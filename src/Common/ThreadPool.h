#pragma once

#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <list>
#include <optional>
#include <atomic>

#include <boost/heap/priority_queue.hpp>

#include <Poco/Event.h>
#include <Common/ThreadStatus.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <base/scope_guard.h>

/** Very simple thread pool similar to boost::threadpool.
  * Advantages:
  * - catches exceptions and rethrows on wait.
  *
  * This thread pool can be used as a task queue.
  * For example, you can create a thread pool with 10 threads (and queue of size 10) and schedule 1000 tasks
  * - in this case you will be blocked to keep 10 tasks in fly.
  *
  * Thread: std::thread or something with identical interface.
  */
template <typename Thread>
class ThreadPoolImpl
{
public:
    using Job = std::function<void()>;

    /// Maximum number of threads is based on the number of physical cores.
    ThreadPoolImpl();

    /// Size is constant. Up to num_threads are created on demand and then run until shutdown.
    explicit ThreadPoolImpl(size_t max_threads_);

    /// queue_size - maximum number of running plus scheduled jobs. It can be greater than max_threads. Zero means unlimited.
    ThreadPoolImpl(size_t max_threads_, size_t max_free_threads_, size_t queue_size_, bool shutdown_on_exception_ = true);

    /// Add new job. Locks until number of scheduled jobs is less than maximum or exception in one of threads was thrown.
    /// If any thread was throw an exception, first exception will be rethrown from this method,
    ///  and exception will be cleared.
    /// Also throws an exception if cannot create thread.
    /// Priority: greater is higher.
    /// NOTE: Probably you should call wait() if exception was thrown. If some previously scheduled jobs are using some objects,
    /// located on stack of current thread, the stack must not be unwinded until all jobs finished. However,
    /// if ThreadPool is a local object, it will wait for all scheduled jobs in own destructor.
    void scheduleOrThrowOnError(Job job, ssize_t priority = 0);

    /// Similar to scheduleOrThrowOnError(...). Wait for specified amount of time and schedule a job or return false.
    bool trySchedule(Job job, ssize_t priority = 0, uint64_t wait_microseconds = 0) noexcept;

    /// Similar to scheduleOrThrowOnError(...). Wait for specified amount of time and schedule a job or throw an exception.
    void scheduleOrThrow(Job job, ssize_t priority = 0, uint64_t wait_microseconds = 0, bool propagate_opentelemetry_tracing_context = true);

    /// Wait for all currently active jobs to be done.
    /// You may call schedule and wait many times in arbitrary order.
    /// If any thread was throw an exception, first exception will be rethrown from this method,
    ///  and exception will be cleared.
    void wait();

    /// Waits for all threads. Doesn't rethrow exceptions (use 'wait' method to rethrow exceptions).
    /// You should not destroy object while calling schedule or wait methods from another threads.
    ~ThreadPoolImpl();

    /// Returns number of running and scheduled jobs.
    size_t active() const;

    /// Returns true if the pool already terminated
    /// (and any further scheduling will produce CANNOT_SCHEDULE_TASK exception)
    bool finished() const;

    void setMaxThreads(size_t value);
    void setMaxFreeThreads(size_t value);
    void setQueueSize(size_t value);
    size_t getMaxThreads() const;

private:
    mutable std::mutex mutex;
    std::condition_variable job_finished;
    std::condition_variable new_job_or_shutdown;

    size_t max_threads;
    size_t max_free_threads;
    size_t queue_size;

    size_t scheduled_jobs = 0;
    bool shutdown = false;
    const bool shutdown_on_exception = true;

    struct JobWithPriority
    {
        Job job;
        ssize_t priority;
        DB::OpenTelemetry::TracingContextOnThread thread_trace_context;

        JobWithPriority(Job job_, ssize_t priority_, const DB::OpenTelemetry::TracingContextOnThread& thread_trace_context_)
            : job(job_), priority(priority_), thread_trace_context(thread_trace_context_) {}

        bool operator< (const JobWithPriority & rhs) const
        {
            return priority < rhs.priority;
        }
    };

    boost::heap::priority_queue<JobWithPriority> jobs;
    std::list<Thread> threads;
    std::exception_ptr first_exception;

    template <typename ReturnType>
    ReturnType scheduleImpl(Job job, ssize_t priority, std::optional<uint64_t> wait_microseconds, bool propagate_opentelemetry_tracing_context = true);

    void worker(typename std::list<Thread>::iterator thread_it);

    void finalize();
};


/// ThreadPool with std::thread for threads.
using FreeThreadPool = ThreadPoolImpl<std::thread>;


/** Global ThreadPool that can be used as a singleton.
  * Why it is needed?
  *
  * Linux can create and destroy about 100 000 threads per second (quite good).
  * With simple ThreadPool (based on mutex and condvar) you can assign about 200 000 tasks per second
  * - not much difference comparing to not using a thread pool at all.
  *
  * But if you reuse OS threads instead of creating and destroying them, several benefits exist:
  * - allocator performance will usually be better due to reuse of thread local caches, especially for jemalloc:
  *   https://github.com/jemalloc/jemalloc/issues/1347
  * - address sanitizer and thread sanitizer will not fail due to global limit on number of created threads.
  * - program will work faster in gdb;
  */
class GlobalThreadPool : public FreeThreadPool, private boost::noncopyable
{
    static std::unique_ptr<GlobalThreadPool> the_instance;

    GlobalThreadPool(size_t max_threads_, size_t max_free_threads_,
            size_t queue_size_, const bool shutdown_on_exception_)
        : FreeThreadPool(max_threads_, max_free_threads_, queue_size_,
            shutdown_on_exception_)
    {}

public:
    static void initialize(size_t max_threads = 10000, size_t max_free_threads = 1000, size_t queue_size = 10000);
    static GlobalThreadPool & instance();
};


/** Looks like std::thread but allocates threads in GlobalThreadPool.
  * Also holds ThreadStatus for ClickHouse.
  *
  * NOTE: User code should use 'ThreadFromGlobalPool' declared below instead of directly using this class.
  *
  */
template <bool propagate_opentelemetry_context = true>
class ThreadFromGlobalPoolImpl : boost::noncopyable
{
public:
    ThreadFromGlobalPoolImpl() = default;

    template <typename Function, typename... Args>
    explicit ThreadFromGlobalPoolImpl(Function && func, Args &&... args)
        : state(std::make_shared<State>())
    {
        /// NOTE:
        /// - If this will throw an exception, the destructor won't be called
        /// - this pointer cannot be passed in the lambda, since after detach() it will not be valid
        GlobalThreadPool::instance().scheduleOrThrow([
            state = state,
            func = std::forward<Function>(func),
            args = std::make_tuple(std::forward<Args>(args)...)]() mutable /// mutable is needed to destroy capture
        {
            SCOPE_EXIT(
                state->thread_id = std::thread::id();
                state->event.set();
            );

            state->thread_id = std::this_thread::get_id();

            /// This moves are needed to destroy function and arguments before exit.
            /// It will guarantee that after ThreadFromGlobalPool::join all captured params are destroyed.
            auto function = std::move(func);
            auto arguments = std::move(args);

            /// Thread status holds raw pointer on query context, thus it always must be destroyed
            /// before sending signal that permits to join this thread.
            DB::ThreadStatus thread_status;
            std::apply(function, arguments);
        },
        0, // default priority
        0, // default wait_microseconds
        propagate_opentelemetry_context
        );
    }

    ThreadFromGlobalPoolImpl(ThreadFromGlobalPoolImpl && rhs) noexcept
    {
        *this = std::move(rhs);
    }

    ThreadFromGlobalPoolImpl & operator=(ThreadFromGlobalPoolImpl && rhs) noexcept
    {
        if (initialized())
            abort();
        state = std::move(rhs.state);
        return *this;
    }

    ~ThreadFromGlobalPoolImpl()
    {
        if (initialized())
            abort();
    }

    void join()
    {
        if (!initialized())
            abort();

        state->event.wait();
        state.reset();
    }

    void detach()
    {
        if (!initialized())
            abort();
        state.reset();
    }

    bool joinable() const
    {
        if (!state)
            return false;
        /// Thread cannot join itself.
        if (state->thread_id == std::this_thread::get_id())
            return false;
        return true;
    }

protected:
    struct State
    {
        /// Should be atomic() because of possible concurrent access between
        /// assignment and joinable() check.
        std::atomic<std::thread::id> thread_id;

        /// The state used in this object and inside the thread job.
        Poco::Event event;
    };
    std::shared_ptr<State> state;

    /// Internally initialized() should be used over joinable(),
    /// since it is enough to know that the thread is initialized,
    /// and ignore that fact that thread cannot join itself.
    bool initialized() const
    {
        return static_cast<bool>(state);
    }
};

/// Schedule jobs/tasks on global thread pool without implicit passing tracing context on current thread to underlying worker as parent tracing context.
///
/// If you implement your own job/task scheduling upon global thread pool or schedules a long time running job in a infinite loop way,
/// you need to use class, or you need to use ThreadFromGlobalPool below.
///
/// See the comments of ThreadPool below to know how it works.
using ThreadFromGlobalPoolNoTracingContextPropagation = ThreadFromGlobalPoolImpl<false>;

/// An alias of thread that execute jobs/tasks on global thread pool by implicit passing tracing context on current thread to underlying worker as parent tracing context.
/// If jobs/tasks are directly scheduled by using APIs of this class, you need to use this class or you need to use class above.
using ThreadFromGlobalPool = ThreadFromGlobalPoolImpl<true>;

/// Recommended thread pool for the case when multiple thread pools are created and destroyed.
///
/// The template parameter of ThreadFromGlobalPool is set to false to disable tracing context propagation to underlying worker.
/// Because ThreadFromGlobalPool schedules a job upon GlobalThreadPool, this means there will be two workers to schedule a job in 'ThreadPool',
/// one is at GlobalThreadPool level, the other is at ThreadPool level, so tracing context will be initialized on the same thread twice.
///
/// Once the worker on ThreadPool gains the control of execution, it won't return until it's shutdown,
/// which means the tracing context initialized at underlying worker level won't be delete for a very long time.
/// This would cause wrong context for further jobs scheduled in ThreadPool.
///
/// To make sure the tracing context is correctly propagated, we explicitly disable context propagation(including initialization and de-initialization) at underlying worker level.
///
using ThreadPool = ThreadPoolImpl<ThreadFromGlobalPoolNoTracingContextPropagation>;
