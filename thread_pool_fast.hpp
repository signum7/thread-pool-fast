#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                            Подключаемые заголовки                         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include <cstdio>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include <cassert>
#include <chrono>
#include <future>
#include <tuple>
#include <utility>


// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║   thread_pool_fast.hpp  —  быстрый пул потоков C++23 для рендер-движков   ║
// ║                                                                           ║
// ║   Режимы работы:                                                          ║
// ║     SingleMutex   — один мьютекс на все очереди, просто и надёжно         ║
// ║     PerQueueMutex — отдельный мьютекс на каждый приоритет                 ║
// ║     LockFree      — кольцевой буфер Вьюкова MPMC, без блокировок          ║
// ║                                                                           ║
// ║   Требования: Windows x64, MSVC 2022+, C++23                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#ifdef POOL_STANDALONE
#  ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#      define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#      define NOMINMAX
#    endif
#    include <windows.h>
#  else
#    error "CRITICAL: Windows only."
#  endif

#  if defined(_MSC_VER)
#    if _MSC_VER < 1930
#      error "CRITICAL: Visual Studio 2022 or newer required."
#    endif
#    define POOL_CPP_VER _MSVC_LANG
#  else
#    define POOL_CPP_VER __cplusplus
#  endif
#  if POOL_CPP_VER < 202302L
#    error "CRITICAL: C++23 required."
#  endif

#  ifndef POOL_ASSUME         
#    if defined(_MSC_VER)
#      define POOL_ASSUME(x) __assume(x)
#    elif defined(__has_cpp_attribute) && __has_cpp_attribute(assume)
#      define POOL_ASSUME(x) [[assume(x)]]
#    else
#      define POOL_ASSUME(x)
#    endif
#  endif

// Выравнивание по кэш-линии — двойное, чтобы работало и в MSVC, и в GCC/Clang
#ifndef POOL_ALIGN_AS
#  if defined(_MSC_VER)
#    define POOL_ALIGN_AS(n) alignas(n) __declspec(align(n))
#  else
#    define POOL_ALIGN_AS(n) alignas(n)
#  endif
#endif

#  ifndef POOL_FORCEINLINE
#    if defined(_MSC_VER)
#      define POOL_FORCEINLINE __forceinline
#    elif defined(__GNUC__) || defined(__clang__)
#      define POOL_FORCEINLINE inline __attribute__((always_inline))
#    else
#      define POOL_FORCEINLINE inline
#    endif
#  endif

#  ifndef POOL_NOINLINE
#    if defined(_MSC_VER)
#      define POOL_NOINLINE __declspec(noinline)
#    elif defined(__GNUC__) || defined(__clang__)
#      define POOL_NOINLINE __attribute__((noinline))
#    else
#      define POOL_NOINLINE
#    endif
#  endif

#else  // встроенный режим: подключаем макросы из движка ──────────────────────

#  include "../common.hpp"
#  ifndef COM_ASSUME
#    error "CRITICAL: common.hpp must define COM_ASSUME(x) and other definitions, for compiler hints."
#  endif
#  ifndef POOL_ASSUME
#  define POOL_ASSUME	COM_ASSUME
#  endif
#  ifndef POOL_FORCEINLINE
#  define POOL_FORCEINLINE	FAST_FORCEINLINE
#  endif
#  ifndef POOL_NOINLINE
#  define POOL_NOINLINE		COM_NOINLINE
#  endif
#  ifndef POOL_ALIGN_AS
#  define POOL_ALIGN_AS		COM_ALIGN_AS
#  endif
#endif // POOL_STANDALONE
// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ ВЕРСИОНИРОВАНИЕ (Version API)                                             ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#  ifndef POOL_VERSION_MAJOR
#define POOL_VERSION_MAJOR 2
#define POOL_VERSION_MINOR 0
#define POOL_VERSION_PATCH 0
#  endif
// Вспомогательные макросы для превращения цифр в строку на этапе компиляции
#define POOL_STR_HELPER(x) #x
#define POOL_STR(x) POOL_STR_HELPER(x)

// Строковое представление версии: "1.0.0"
#define POOL_VERSION_STRING POOL_STR(POOL_VERSION_MAJOR) "." POOL_STR(POOL_VERSION_MINOR) "." POOL_STR(POOL_VERSION_PATCH)

// Единое число для проверок препроцессора (1.0.0 превратится в 10000)
#define POOL_VERSION_INT (POOL_VERSION_MAJOR * 10000 + POOL_VERSION_MINOR * 100 + POOL_VERSION_PATCH)

namespace PoolVersion {
	struct Version {
		uint32_t major;
		uint32_t minor;
		uint32_t patch;
	};

	// Возвращает версию в виде структуры (удобно для логики)
	[[nodiscard]] constexpr Version get() noexcept
	{
		return { POOL_VERSION_MAJOR, POOL_VERSION_MINOR, POOL_VERSION_PATCH };
	}

	// Возвращает версию в виде готовой C-строки (удобно для логов/ImGui)
	[[nodiscard]] constexpr const char* as_string() noexcept
	{
		return POOL_VERSION_STRING;
	}
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ СИСТЕМА ЛОГИРОВАНИЯ (PoolLog)                                             ║
// ║ Zero-overhead коллбеки для вывода предупреждений                          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
namespace PoolLog
{
	// Пользователь может установить свою функцию логирования из основного движка
	inline void (*warn_fn)(const char*) noexcept = nullptr;
	inline void (*info_fn)(const char*) noexcept = nullptr;

	inline void warn(const char* msg) noexcept
	{
		if (!msg) return;
		if (warn_fn) warn_fn(msg);
		else         std::printf("[WARN] %s\n", msg);
	}
	inline void info(const char* msg) noexcept
	{
		if (!msg) return;
		if (info_fn) info_fn(msg);
		else         std::printf("[INFO] %s\n", msg);
	}
}
// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ КОНФИГУРАЦИЯ (PoolConfig)                                                 ║
// ║ Телеметрия и исключения включаются в Debug, отключаются в Release         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// Конфигурация  
namespace PoolConfig {
#ifndef NDEBUG
	inline constexpr bool	EnableTelemetry  = true;
	// В Debug исключения удобнее — падение сразу с сообщением, а не тихий краш.
	// В Release — нулевой оверхед: все проверки выкидываются оптимизатором.
	inline constexpr bool	EnableExceptions = true;
#else
	inline constexpr bool	EnableTelemetry  = false;  // zero-cost в Release
	inline constexpr bool	EnableExceptions = false;
#endif

	inline constexpr int   LowStarveSec = 3;       // SingleMutex: порог starvation (сек)
	static_assert(sizeof(void*) == 8, "64-bit build required");
}

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ ПУБЛИЧНЫЕ ТИПЫ                                                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

enum class LockMode : uint8_t
{
	SingleMutex,    // Один мьютекс на все очереди — просто, надёжно
	PerQueueMutex,  // Отдельный мьютекс на каждую очередь приоритета
	LockFree,       // Кольцевой буфер Вьюкова MPMC — без мьютексов на горячем пути
};

enum class Priority : int { Low = 0, Normal = 1, High = 2, Critical = 3 };

struct FastTask
{
	void (*fn)(void* ctx) = nullptr;
	void* ctx = nullptr;
};

struct TaskOptions
{
	Priority priority = Priority::Normal;
};

// ─── Проверки на этапе компиляции ────────────────────────────────────────────


static_assert(sizeof(FastTask) == 2 * sizeof(void*), "POOL: FastTask must be exactly 2 pointers.");
static_assert(alignof(FastTask) == alignof(void*), "POOL: FastTask must be pointer-aligned.");
static_assert(std::is_trivially_copyable_v<FastTask>, "POOL: FastTask must be trivially copyable.");
static_assert(std::is_trivially_destructible_v<FastTask>, "POOL: FastTask must be trivially destructible.");
static_assert(std::is_trivially_copyable_v<TaskOptions>, "POOL: TaskOptions must be trivially copyable.");

static_assert(static_cast<int>(Priority::Low) == 0, "POOL: Priority::Low must be 0.");
static_assert(static_cast<int>(Priority::Normal) == 1, "POOL: Priority::Normal must be 1.");
static_assert(static_cast<int>(Priority::High) == 2, "POOL: Priority::High must be 2.");
static_assert(static_cast<int>(Priority::Critical) == 3, "POOL: Priority::Critical must be 3.");

static_assert(std::atomic<uint64_t>::is_always_lock_free, "POOL: atomic<uint64_t> must be lock-free.");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "POOL: atomic<uint32_t> must be lock-free.");

// =============================================================================
//  thread_pool<Mode, RingCap>  —  ядро пула потоков
// =============================================================================

template<LockMode Mode = LockMode::SingleMutex, size_t RingCap = 4096>
class thread_pool {
	struct ctor_token {};
	enum class PoolState : uint8_t { running, stopped };

	static_assert(std::atomic<PoolState>::is_always_lock_free, "POOL: atomic<PoolState> must be lock-free.");
	static_assert(Mode != LockMode::LockFree || RingCap >= 4, "POOL: RingCap must be >= 4 for LockFree.");
	static_assert(Mode != LockMode::LockFree || (RingCap & (RingCap - 1)) == 0, "POOL: RingCap must be power of 2.");


	// ─── Очередь для режимов с мьютексом ─────────────────────────────────────
	//  std::vector + индекс головы — дешёвый pop без перекладывания элементов
	struct TaskQueue {
		std::vector<FastTask> tasks;
		size_t head = 0;

		POOL_FORCEINLINE void push_back(FastTask t)
		{
			if (head > 0 && head == tasks.size()) [[unlikely]]
			{
				tasks.clear();
				head = 0;
			}
			tasks.push_back(t);
		}

		[[nodiscard]] POOL_FORCEINLINE bool pop_front(FastTask& out) noexcept
		{
			if (head < tasks.size()) [[likely]]
			{
				out = tasks[head++];
				return true;
			}
			tasks.clear();
			head = 0;
			return false;
		}

		[[nodiscard]] POOL_FORCEINLINE bool empty() const noexcept
		{
			return head >= tasks.size();
		}
	};

	// ─── Очередь для LockFree-режима ─────────────────────────────────────────
	//  Кольцевой буфер Вьюкова MPMC: несколько продюсеров, несколько консьюмеров,
	//  без блокировок. Каждый слот — ровно одна кэш-линия (64 байта).
	struct MPMCQueue {
		static constexpr size_t MASK = RingCap - 1;

		struct POOL_ALIGN_AS(64) Slot
		{
			std::atomic<size_t> seq{ 0 };
			FastTask            task{};
			char _pad[64 - sizeof(std::atomic<size_t>) - sizeof(FastTask)]{};
		};
		static_assert(sizeof(Slot) == 64, "Slot must be exactly one cache line (64 bytes).");
		static_assert(alignof(Slot) == 64, "Slot must be 64-byte aligned.");

		POOL_ALIGN_AS(64) std::atomic<size_t>	head_{ 0 };
		POOL_ALIGN_AS(64) std::atomic<size_t>	tail_{ 0 };
		std::array<Slot, RingCap>				slots_;

		MPMCQueue() noexcept
		{
			for (size_t i = 0; i < RingCap; ++i)
			{
				slots_[i].seq.store(i, std::memory_order_relaxed);
			}
		}

		[[nodiscard]] POOL_FORCEINLINE bool push(FastTask ft) noexcept
		{
			size_t tail = tail_.load(std::memory_order_relaxed);
			for (;;)
			{
				Slot& s = slots_[tail & MASK];
				const size_t   seq = s.seq.load(std::memory_order_acquire);
				const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail);

				if (dif == 0) [[likely]]
				{
					if (tail_.compare_exchange_weak(tail, tail + 1, std::memory_order_relaxed, std::memory_order_relaxed)) [[likely]]
					{
						s.task = ft;
						s.seq.store(tail + 1, std::memory_order_release);
						return true;
					}
				}
				else if (dif < 0) [[unlikely]]
				{
					return false;
				}
				else {
					tail = tail_.load(std::memory_order_relaxed);
				}
			}
		}

		[[nodiscard]] POOL_FORCEINLINE bool pop(FastTask& out) noexcept
		{
			size_t head = head_.load(std::memory_order_relaxed);
			for (;;)
			{
				Slot& s = slots_[head & MASK];
				const size_t   seq = s.seq.load(std::memory_order_acquire);
				const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(head + 1);

				if (dif == 0) [[likely]]
				{
					if (head_.compare_exchange_weak(head, head + 1, std::memory_order_relaxed, std::memory_order_relaxed)) [[likely]]
					{
						out = s.task;
						s.seq.store(head + RingCap, std::memory_order_release);
						return true;
					}
				}
				else if (dif < 0) [[unlikely]]
				{
					return false;
				}
				else {
					head = head_.load(std::memory_order_relaxed);
				}
			}
		}

		[[nodiscard]] POOL_FORCEINLINE bool empty() const noexcept
		{
			return head_.load(std::memory_order_relaxed) >= tail_.load(std::memory_order_relaxed);
		}
	};

	// ─── Данные, специфичные для каждого режима ──────────────────────────────
	struct SingleMutexData {
		std::mutex               mtx;
		std::condition_variable  cv;
		std::array<TaskQueue, 4> queues;
		// Таймеры голодания для очередей 0–2 (Low/Normal/High) — под мьютексом
		std::array<std::chrono::steady_clock::time_point, 3> starve_since{};
		std::array<bool, 3>                                  starve_warned{ false, false, false };
	};

	struct PerQueueData {
		std::mutex                notify_mtx;
		std::condition_variable   cv;
		std::array<std::mutex, 4> queue_mtx;
		std::array<TaskQueue, 4>  queues;

		// Битовая маска готовности: бит p = 1 означает, что queue[p] не пуста.
		// Атомарный флаг — не требует захвата очередного мьютекса для проверки.
		std::atomic<uint32_t>     ready_mask{ 0 };
	};

	struct LockFreeData {
		POOL_ALIGN_AS(64)std::atomic<uint32_t>    wake_signal{ 0 };
		POOL_ALIGN_AS(64)std::atomic<uint32_t>    sleeping_threads{ 0 };

		// Примечание по архитектуре: 4 уровня приоритета схлопываются в 2 физические
		// очереди, чтобы снизить атомарную конкуренцию и промахи кэша при сканировании.
		//   bin[1] = Critical (3) и High (2)  — обрабатывается первым
		//   bin[0] = Normal (1)  и Low  (0)   — обрабатывается вторым
		std::array<MPMCQueue, 2> queues;

		LockFreeData() = default;
		~LockFreeData() = default;
		LockFreeData(const LockFreeData&) = delete;
		LockFreeData& operator=(const LockFreeData&) = delete;
	};

	using Data = std::conditional_t<Mode == LockMode::SingleMutex, SingleMutexData, std::conditional_t <Mode == LockMode::PerQueueMutex, PerQueueData, LockFreeData>>;

	// ─── Поля класса ──────────────────────────────────────────────────────────
	std::vector<std::thread>           threads_;
	std::atomic<PoolState>             state_{ PoolState::running };
	Data                               data_;
	POOL_ALIGN_AS(64) std::atomic<uint64_t> unfinished_tasks_{ 0 };

	// Телеметрия (используется только если PoolConfig::EnableTelemetry == true)
	POOL_ALIGN_AS(64) std::atomic<uint64_t> tel_submitted_{ 0 };
	POOL_ALIGN_AS(64) std::atomic<uint64_t> tel_executed_{ 0 };
	POOL_ALIGN_AS(64) std::atomic<uint64_t> tel_sleeps_{ 0 };
	POOL_ALIGN_AS(64) std::atomic<uint64_t> tel_push_spins_{ 0 };
	POOL_ALIGN_AS(64) std::atomic<uint64_t> tel_wait_calls_{ 0 };
	POOL_ALIGN_AS(64) std::atomic<uint64_t> tel_low_starve_{ 0 };
public:
	// ─── Создание и уничтожение ───────────────────────────────────────────────

	[[nodiscard]] static std::shared_ptr<thread_pool> create(uint32_t n)
	{
		if (n == 0) [[unlikely]]
		{
			if constexpr (PoolConfig::EnableExceptions) { throw std::invalid_argument("thread_pool::create: n must be > 0"); }
			else { std::abort(); }
		}
		return std::make_shared<thread_pool>(ctor_token{}, n);
	}

	thread_pool(ctor_token, uint32_t n)
	{
		threads_.reserve(n);
		if constexpr (Mode != LockMode::LockFree)
		{
			for (auto& q : data_.queues)
			{
				q.tasks.reserve(1024);
			}
		}
		if constexpr (PoolConfig::EnableExceptions)
		{
			try
			{
				for (uint32_t i = 0; i < n; ++i)
				{
					threads_.emplace_back(&thread_pool::run, this);
				}
			}
			catch (...)
			{
				signal_stop();
				for (auto& t : threads_) if (t.joinable()) t.join();
				throw;
			}
		}
		else
		{
			for (uint32_t i = 0; i < n; ++i)
			{
				threads_.emplace_back(&thread_pool::run, this);
			}
		}
	}

	thread_pool(const thread_pool&) = delete;
	thread_pool& operator=(const thread_pool&) = delete;
	thread_pool(thread_pool&&) = delete;
	thread_pool& operator=(thread_pool&&) = delete;

	~thread_pool()
	{
		wait_all();
		signal_stop();
		for (auto& t : threads_) if (t.joinable()) t.join();
	}

	// ─── Публичный API ────────────────────────────────────────────────────────

	POOL_FORCEINLINE void add_task_fast(TaskOptions opts, FastTask ft)
	{
		const int p = static_cast<int>(opts.priority) & 3;
		POOL_ASSUME(p >= 0 && p <= 3);
		push_one(p, ft);
		notify_one_work();
	}

	POOL_FORCEINLINE void add_task_fast_bulk(TaskOptions opts, std::span<const FastTask> tasks)
	{
		if (tasks.empty()) [[unlikely]] return;
		const int p = static_cast<int>(opts.priority) & 3;
		POOL_ASSUME(p >= 0 && p <= 3);
		push_bulk(p, tasks);
		notify_many_work(tasks.size());
	}
	// ─── Расширенный API: add_task / submit ──────────────────────────────────
	//  Горячий путь не затрагивается. Удобно для сложных задач и future-результатов.

	template<class F, class... Args>
	void add_task(F&& f, Args&&... args)
	{
		add_task(TaskOptions{}, std::forward<F>(f), std::forward<Args>(args)...);
	}

	template<class F, class... Args>
	void add_task(TaskOptions opts, F&& f, Args&&... args)
	{
		static_assert(Mode == LockMode::SingleMutex, "Rich add_task() is intended for SingleMutex mode.");
		using Box = RichTask<std::decay_t<F>, std::decay_t<Args>...>;

		auto holder = std::unique_ptr<RichTaskBase>(new Box(std::forward<F>(f), std::forward<Args>(args)...));
		add_task_fast(opts, FastTask{ &rich_task_trampoline, holder.get() });
		holder.release();
	}

	template<class F, class... Args>
	[[nodiscard]] auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
	{
		return submit(TaskOptions{}, std::forward<F>(f), std::forward<Args>(args)...);
	}

	template<class F, class... Args>
	[[nodiscard]] auto submit(TaskOptions opts, F&& f, Args&&... args) -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
	{
		static_assert(Mode == LockMode::SingleMutex, "Rich submit() is intended for SingleMutex mode.");
		using Fn = std::decay_t<F>;
		using R = std::invoke_result_t<Fn, std::decay_t<Args>...>;

		auto bound = [func = Fn(std::forward<F>(f)), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R
			{
				if constexpr (std::is_void_v<R>)
				{
					std::apply(func, tup);
				}
				else {
					return std::apply(func, tup);
				}
			};

		auto task = std::unique_ptr<std::packaged_task<R()>>(new std::packaged_task<R()>(std::move(bound)));
		auto fut = task->get_future();

		add_task_fast(opts, FastTask{ &packaged_task_trampoline<R>, task.get() });
		task.release();

		return fut;
	}

	// Ждёт, пока счётчик незавершённых задач не упадёт до нуля.
	POOL_FORCEINLINE void wait_all() noexcept
	{
		if constexpr (PoolConfig::EnableTelemetry)
		{
			tel_wait_calls_.fetch_add(1, std::memory_order_relaxed);
		}
		if constexpr (Mode == LockMode::LockFree)
		{
			// Spin-wait: избегаем jitter OS-планировщика.
			// Безопасно: unfinished_tasks_ — атомарный счётчик,
			// воркеры декрементируют его после каждой задачи.
			uint32_t spin = 0;
			while (unfinished_tasks_.load(std::memory_order_acquire) > 0)
			{
				if (spin < 800)
				{
					YieldProcessor(); // _mm_pause() — hint CPU что мы в spin-wait
					++spin;
				}
				else {
					spin = 0;
					std::this_thread::yield(); // не жжём ядро вхолостую бесконечно
				}
			}
		}
		else {
			// Mutex-режимы: futex-sleep нормален, они и так медленнее
			uint64_t count = unfinished_tasks_.load(std::memory_order_acquire);
			while (count > 0)
			{
				unfinished_tasks_.wait(count, std::memory_order_relaxed);
				count = unfinished_tasks_.load(std::memory_order_acquire);
			}
		}
	}

	// Заглушка для обратной совместимости — параметр cleanup зарезервирован, не используется.
	POOL_FORCEINLINE void wait_all(bool /*cleanup*/) noexcept { wait_all(); }

	[[nodiscard]] POOL_FORCEINLINE uint32_t thread_count() const noexcept
	{
		return static_cast<uint32_t>(threads_.size());
	}
	static constexpr LockMode lock_mode = Mode;
	// ─── Телеметрия ───────────────────────────────────────────────────────────
	struct PoolTelemetry
	{
		uint64_t total_submitted         = 0; // задач отправлено в пул
		uint64_t total_executed          = 0; // задач выполнено воркерами
		uint64_t sleep_cycles            = 0; // раз воркеры уходили в OS-сон
		uint64_t push_spins              = 0; // итераций ожидания слота (LockFree ring full)
		uint64_t wait_all_calls          = 0; // вызовов wait_all() главным потоком
		uint64_t low_starvation_warnings = 0; // эпизодов голодания (Low/Normal/High), только SingleMutex
	};

	[[nodiscard]] POOL_FORCEINLINE PoolTelemetry telemetry() const noexcept
	{
		PoolTelemetry t{};
		if constexpr (PoolConfig::EnableTelemetry)
		{
			t.total_submitted = tel_submitted_.load(std::memory_order_relaxed);
			t.total_executed = tel_executed_.load(std::memory_order_relaxed);
			t.sleep_cycles = tel_sleeps_.load(std::memory_order_relaxed);
			t.push_spins = tel_push_spins_.load(std::memory_order_relaxed);
			t.wait_all_calls = tel_wait_calls_.load(std::memory_order_relaxed);
			t.low_starvation_warnings = tel_low_starve_.load(std::memory_order_relaxed);
		}
		return t;
	}
private:
	// ─── Внутренние вспомогательные структуры ────────────────────────────────
	// ─── Расширенный API: вспомогательные типы (только SingleMutex) ──────────
	struct RichTaskBase {
		virtual void run() = 0;
		virtual ~RichTaskBase() = default;
	};

	template<class Fn, class... Args>
	struct RichTask final : RichTaskBase
	{
		Fn fn;
		std::tuple<Args...> args;
		template<class F, class... As>
		RichTask(F&& f, As&&... as) : fn(std::forward<F>(f)), args(std::forward<As>(as)...) {}
		void run() override { std::apply(fn, args); }
	};

	static void rich_task_trampoline(void* ctx) noexcept(!PoolConfig::EnableExceptions)
	{
		std::unique_ptr<RichTaskBase> box(static_cast<RichTaskBase*>(ctx));
		box->run();
	}

	template<class R>
	static void packaged_task_trampoline(void* ctx) noexcept(!PoolConfig::EnableExceptions)
	{
		std::unique_ptr<std::packaged_task<R()>> task(static_cast<std::packaged_task<R()>*>(ctx));
		(*task)();
	}

	POOL_FORCEINLINE void check_running() const
	{
		if (state_.load(std::memory_order_relaxed) != PoolState::running) [[unlikely]]
		{
			if constexpr (PoolConfig::EnableExceptions) { throw std::runtime_error("thread_pool: pool is stopping"); }
			else { std::abort(); }
		}
	}

	[[nodiscard]] POOL_FORCEINLINE bool stopped() const noexcept
	{
		return state_.load(std::memory_order_relaxed) == PoolState::stopped;
	}
	static POOL_FORCEINLINE int highest_ready_priority(uint32_t mask) noexcept
	{
		if (mask & (1u << 3)) return 3;
		if (mask & (1u << 2)) return 2;
		if (mask & (1u << 1)) return 1;
		if (mask & (1u << 0)) return 0;
		return -1;
	}
	[[nodiscard]] POOL_FORCEINLINE bool any_ready_locked() const noexcept
	{
		if constexpr (Mode == LockMode::SingleMutex)
		{
			return !data_.queues[3].empty() || !data_.queues[2].empty() || !data_.queues[1].empty() || !data_.queues[0].empty();

		}
		else if constexpr (Mode == LockMode::PerQueueMutex)
		{
			return data_.ready_mask.load(std::memory_order_relaxed) != 0;
		}
		else {
			return !data_.queues[1].empty() || !data_.queues[0].empty();
		}
	}

	[[nodiscard]] POOL_FORCEINLINE bool pop_best_ready(FastTask& out) noexcept(Mode != LockMode::PerQueueMutex)
	{
		if constexpr (Mode == LockMode::SingleMutex)
		{
			if (data_.queues[3].pop_front(out))  return true;
			if (data_.queues[2].pop_front(out))  return true;
			if (data_.queues[1].pop_front(out)) [[likely]] return true;
			if (data_.queues[0].pop_front(out)) [[unlikely]] return true;
			return false;
		}
		else if constexpr (Mode == LockMode::PerQueueMutex)
		{
			const uint32_t mask = data_.ready_mask.load(std::memory_order_acquire);
			if (mask == 0) [[unlikely]]
			{
				return false;
			}

			const int p = highest_ready_priority(mask);
			POOL_ASSUME(p >= 0 && p <= 3);

			std::lock_guard<std::mutex> qlk(data_.queue_mtx[p]);

			if (data_.queues[p].pop_front(out))
			{
				if (data_.queues[p].empty())
				{
					data_.ready_mask.fetch_and(~(1u << p), std::memory_order_release);
				}
				return true;
			}

			// Устаревший бит: очередь казалась готовой, но при захвате мьютекса оказалась пустой
			data_.ready_mask.fetch_and(~(1u << p), std::memory_order_release);
			return false;
		}
		else { // LockFree
			// Сначала проверяем высокоприоритетный бин — Critical и High
			if (data_.queues[1].pop(out)) return true;
			// Если пуст — берём из низкоприоритетного бина — Normal и Low
			if (data_.queues[0].pop(out)) return true;
			return false;
		}
	}

	// ─── Добавление задач в очереди ──────────────────────────────────────────

	POOL_FORCEINLINE void push_one(int p, FastTask ft)
	{
		assert(ft.fn && "FastTask::fn must not be null");
		POOL_ASSUME(ft.fn != nullptr);

		if constexpr (PoolConfig::EnableTelemetry)
		{
			tel_submitted_.fetch_add(1, std::memory_order_relaxed);
		}
		if constexpr (Mode == LockMode::SingleMutex)
		{
			std::lock_guard<std::mutex> lk(data_.mtx);
			check_running();
			unfinished_tasks_.fetch_add(1, std::memory_order_relaxed);
			data_.queues[p].push_back(ft);
			if (p <= 2 && data_.starve_since[p] == std::chrono::steady_clock::time_point{})
			{
				data_.starve_since[p] = std::chrono::steady_clock::now();
				data_.starve_warned[p] = false;
			}
		}
		else if constexpr (Mode == LockMode::PerQueueMutex)
		{
			std::lock_guard<std::mutex> qlk(data_.queue_mtx[p]);
			check_running();

			const bool was_empty = data_.queues[p].empty();

			unfinished_tasks_.fetch_add(1, std::memory_order_relaxed);
			data_.queues[p].push_back(ft);

			if (was_empty)
			{
				data_.ready_mask.fetch_or(1u << p, std::memory_order_release);
			}
		}
		else { // LockFree
			check_running();
			unfinished_tasks_.fetch_add(1, std::memory_order_relaxed);

			// Отображаем 4 уровня приоритета в 2 бина: 3,2 → bin[1]; 1,0 → bin[0]
			const int bin = p >> 1;
			int spin_cnt = 0;
			while (!data_.queues[bin].push(ft)) [[unlikely]]
			{
				if (++spin_cnt < 1000) YieldProcessor();
				else std::this_thread::yield();
			}
			// ТЕЛЕМЕТРИЯ: Фиксируем, если кольцевой буфер был переполнен
			if constexpr (PoolConfig::EnableTelemetry)
			{
				if (spin_cnt > 0) tel_push_spins_.fetch_add(spin_cnt, std::memory_order_relaxed);
			}
		}
	}

	POOL_FORCEINLINE void push_bulk(int p, std::span<const FastTask> tasks)
	{
		const auto n = static_cast<uint64_t>(tasks.size());

		if constexpr (PoolConfig::EnableTelemetry)
		{
			tel_submitted_.fetch_add(n, std::memory_order_relaxed);
		}
		for (const auto& ft : tasks)
		{
			assert(ft.fn && "FastTask::fn must not be null");
			POOL_ASSUME(ft.fn != nullptr);
		}
		if constexpr (Mode == LockMode::SingleMutex)
		{
			std::lock_guard<std::mutex> lk(data_.mtx);
			check_running();
			unfinished_tasks_.fetch_add(n, std::memory_order_relaxed);
			for (const auto& ft : tasks) data_.queues[p].push_back(ft);
			if (p <= 2 && data_.starve_since[p] == std::chrono::steady_clock::time_point{})
			{
				data_.starve_since[p] = std::chrono::steady_clock::now();
				data_.starve_warned[p] = false;
			}
		}
		else if constexpr (Mode == LockMode::PerQueueMutex)
		{
			std::lock_guard<std::mutex> qlk(data_.queue_mtx[p]);
			check_running();

			const bool was_empty = data_.queues[p].empty();

			unfinished_tasks_.fetch_add(n, std::memory_order_relaxed);
			for (const auto& ft : tasks)
			{
				data_.queues[p].push_back(ft);
			}

			if (was_empty)
			{
				data_.ready_mask.fetch_or(1u << p, std::memory_order_release);
			}
		}
		else { // LockFree
			check_running();
			unfinished_tasks_.fetch_add(n, std::memory_order_relaxed);
			const int bin = p >> 1;
			if constexpr (PoolConfig::EnableTelemetry)
			{
				int total_spins = 0;
				for (const auto& ft : tasks) {
					int spin_cnt = 0;
					while (!data_.queues[bin].push(ft)) [[unlikely]]
					{
						if (++spin_cnt < 1000) YieldProcessor();
						else std::this_thread::yield();
					}
					total_spins += spin_cnt;
				}
				if (total_spins > 0) tel_push_spins_.fetch_add(total_spins, std::memory_order_relaxed);
			}
			else {
				for (const auto& ft : tasks) {
					int spin_cnt = 0;
					while (!data_.queues[bin].push(ft)) [[unlikely]]
					{
						if (++spin_cnt < 1000) YieldProcessor();
						else std::this_thread::yield();
					}
				}
			}
		}
	}


	// ─── Уведомление воркеров о новых задачах ────────────────────────────────

	POOL_FORCEINLINE void notify_one_work() noexcept
	{
		if constexpr (Mode == LockMode::LockFree)
		{
			if (data_.sleeping_threads.load(std::memory_order_acquire) > 0)
			{
				data_.wake_signal.fetch_add(1, std::memory_order_release);
				data_.wake_signal.notify_one();
			}
		}
		else {
			data_.cv.notify_one();
		}
	}

	POOL_FORCEINLINE void notify_many_work(size_t n) noexcept
	{
		if constexpr (Mode == LockMode::LockFree)
		{
			if (data_.sleeping_threads.load(std::memory_order_acquire) > 0)
			{
				data_.wake_signal.fetch_add(1, std::memory_order_release);
				data_.wake_signal.notify_all();
			}
		}
		else {
			size_t to_wake = std::min<size_t>(n, threads_.size());
			if (to_wake >= threads_.size())
			{
				data_.cv.notify_all();
			}
			else {
				for (size_t i = 0; i < to_wake; ++i)
				{
					data_.cv.notify_one();
				}
			}
		}
	}

	// ─── Остановка пула ───────────────────────────────────────────────────────

	POOL_NOINLINE void signal_stop() noexcept(Mode == LockMode::LockFree)
	{
		if constexpr (Mode == LockMode::SingleMutex)
		{
			{
				std::lock_guard<std::mutex> lk(data_.mtx);
				state_.store(PoolState::stopped, std::memory_order_relaxed);
			}
			data_.cv.notify_all();
		}
		else if constexpr (Mode == LockMode::PerQueueMutex)
		{
			{
				std::lock_guard<std::mutex> lk(data_.notify_mtx);
				state_.store(PoolState::stopped, std::memory_order_relaxed);
			}
			data_.cv.notify_all();
		}
		else { // LockFree
			state_.store(PoolState::stopped, std::memory_order_seq_cst);
			data_.wake_signal.fetch_add(1, std::memory_order_release);
			data_.wake_signal.notify_all();
		}
	}

	// ─── Функции рабочих потоков ──────────────────────────────────────────────

	POOL_NOINLINE void run()
	{
		// Установка приоритета в зависимости от Mode/Profile
		if constexpr (Mode == LockMode::LockFree)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		}
		else if constexpr (Mode == LockMode::SingleMutex)
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		}

		if constexpr (Mode == LockMode::LockFree)
		{
			run_lockfree();
		}
		else {
			run_mutex();
		}
	}

	POOL_NOINLINE void run_lockfree()
	{
		uint32_t local_completed = 0;

		// Пакетный сброс счётчика выполненных задач: одна атомарная операция вместо N.
		auto commit_tasks = [&]()
			{
				if (local_completed > 0) [[likely]]
				{
					// ТЕЛЕМЕТРИЯ: обновляем счетчик выполненных задач
					if constexpr (PoolConfig::EnableTelemetry)
					{
						tel_executed_.fetch_add(local_completed, std::memory_order_relaxed);
					}

					if (unfinished_tasks_.fetch_sub(local_completed, std::memory_order_acq_rel) == local_completed) [[unlikely]]
					{
						unfinished_tasks_.notify_all();
					}
					local_completed = 0;
				}
			};

		while (true)
		{
			FastTask task;

			if (pop_best_ready(task))
			{
				assert(task.fn && "worker got null task.fn");
				POOL_ASSUME(task.fn != nullptr);
				task.fn(task.ctx);
				local_completed++;
				if (local_completed >= 64) commit_tasks();
				continue;
			}

			if (state_.load(std::memory_order_acquire) == PoolState::stopped) [[unlikely]]
			{
				commit_tasks();
				break;
			}

			commit_tasks();

			// Активное ожидание перед переходом в сон — избегаем дорогих OS-переключений
			int  spin_cnt = 0;
			bool found = false;
			while (spin_cnt < 2000)
			{
				YieldProcessor();
				if (pop_best_ready(task)) { found = true; break; }
				spin_cnt++;
			}

			if (found)
			{
				assert(task.fn && "worker got null task.fn");
				POOL_ASSUME(task.fn != nullptr);
				task.fn(task.ctx);
				local_completed++;
				continue;
			}

			// Задач нет — уходим в OS-сон до следующего wake_signal
			if constexpr (PoolConfig::EnableTelemetry)
			{
				tel_sleeps_.fetch_add(1, std::memory_order_relaxed);
			}
			data_.sleeping_threads.fetch_add(1, std::memory_order_release);
			uint32_t val = data_.wake_signal.load(std::memory_order_acquire);

			if (!any_ready_locked() && state_.load(std::memory_order_acquire) != PoolState::stopped)
			{
				data_.wake_signal.wait(val, std::memory_order_relaxed);
			}

			data_.sleeping_threads.fetch_sub(1, std::memory_order_release);
		}
	}

	POOL_NOINLINE void run_mutex()
	{
		uint32_t local_completed = 0;

		auto commit_tasks = [&]() {
			if (local_completed > 0) [[likely]]
			{
				// ТЕЛЕМЕТРИЯ: обновляем счетчик выполненных задач
				if constexpr (PoolConfig::EnableTelemetry)
				{
					tel_executed_.fetch_add(local_completed, std::memory_order_relaxed);
				}

				if (unfinished_tasks_.fetch_sub(local_completed, std::memory_order_acq_rel) == local_completed) [[unlikely]]
				{
					unfinished_tasks_.notify_all();
				}
				local_completed = 0;
			}
			};

		while (true)
		{
			FastTask task;
			bool     got = false;
			// SingleMutex: захватываем единственный мьютекс, ждём задачи через cv.
			// Перед уходом в сон сбрасываем накопленный счётчик выполненных задач,
			// чтобы wait_all() не завис в ожидании несброшенных completions.
			if constexpr (Mode == LockMode::SingleMutex)
			{
				std::unique_lock<std::mutex> lk(data_.mtx);
				if (!any_ready_locked() && !stopped()) commit_tasks();

				if constexpr (PoolConfig::EnableTelemetry)
				{
					if (!any_ready_locked() && !stopped()) tel_sleeps_.fetch_add(1, std::memory_order_relaxed);
				}

				data_.cv.wait(lk, [this] { return any_ready_locked() || stopped(); });
				got = pop_best_ready(task);

				// Проверка голодания: таймер и флаг на каждую очередь 0=Low, 1=Normal, 2=High
				const auto now = std::chrono::steady_clock::now();
				for (int starve_p = 0; starve_p <= 2; ++starve_p)
				{
					if (data_.queues[starve_p].empty())
					{
						data_.starve_since[starve_p] = {};
						data_.starve_warned[starve_p] = false;
						continue;
					}
					if (data_.starve_since[starve_p] == std::chrono::steady_clock::time_point{})
					{
						continue;
					}
					auto age = now - data_.starve_since[starve_p];
					if (age >= std::chrono::seconds(PoolConfig::LowStarveSec))
					{
						if (!data_.starve_warned[starve_p])
						{
							char warn_buf[128];
							std::snprintf(warn_buf, sizeof(warn_buf),
								"SingleMutex: Priority tasks starving (>%ds). Queue=%d. Reduce higher-priority load or add workers.",
								PoolConfig::LowStarveSec, starve_p);
							PoolLog::warn(warn_buf);
							if constexpr (PoolConfig::EnableTelemetry)
							{
								tel_low_starve_.fetch_add(1, std::memory_order_relaxed);
							}
							data_.starve_warned[starve_p] = true;
						}
						FastTask ft;
						if (data_.queues[starve_p].pop_front(ft))
						{
							data_.queues[3].push_back(ft);
						}
						break;
					}
				}

				if (!got) [[unlikely]]
				{
					if (stopped()) [[unlikely]] { commit_tasks(); break; }
					continue;
				}
			}
			else { // PerQueueMutex
				got = pop_best_ready(task);

				if (!got) {
					commit_tasks(); // сбрасываем счётчик перед уходом в сон
					{
						std::unique_lock<std::mutex> nlk(data_.notify_mtx);
						if (!any_ready_locked() && !stopped())
						{
							if constexpr (PoolConfig::EnableTelemetry)
							{
								tel_sleeps_.fetch_add(1, std::memory_order_relaxed);
							}
							data_.cv.wait(nlk, [this] {return any_ready_locked() || stopped(); });
						}
					}

					if (stopped() && !any_ready_locked()) [[unlikely]]
					{
						commit_tasks();
						break;
					}

					got = pop_best_ready(task);
					if (!got) [[unlikely]] continue;
				}
			}
			assert(task.fn && "worker got null task.fn");
			POOL_ASSUME(task.fn != nullptr);
			task.fn(task.ctx);
			local_completed++;
			if (local_completed >= 64) commit_tasks();
		}
	}
};

// =============================================================================
//  Псевдонимы для удобного использования
// =============================================================================

using ThreadPoolSingle = thread_pool<LockMode::SingleMutex>;
using ThreadPoolPerQueue = thread_pool<LockMode::PerQueueMutex>;

template<size_t Cap = 4096>
using ThreadPoolFast = thread_pool<LockMode::LockFree, Cap>;

// ─── Финальные проверки на этапе компиляции ──────────────────────────────────

static_assert(sizeof(ThreadPoolSingle) > 0, "POOL: SingleMutex must instantiate.");
static_assert(sizeof(ThreadPoolPerQueue) > 0, "POOL: PerQueueMutex must instantiate.");
static_assert(sizeof(ThreadPoolFast<>) > 0, "POOL: LockFree must instantiate.");
static_assert(sizeof(ThreadPoolFast<>) > sizeof(ThreadPoolSingle), "POOL: LockFree must be larger (ring buffer).");
static_assert(sizeof(ThreadPoolFast<>) > sizeof(ThreadPoolPerQueue), "POOL: LockFree must be larger (ring buffer).");


namespace pool_profiles
{
	enum class Use {
		Background,  // загрузка ресурсов, I/O, стриминг
		Render,      // рендер-пайплайн, средняя конкуренция
		Hot          // самый горячий цикл, максимум скорости
	};
	template<Use U>
	constexpr const char* name()
	{
		if constexpr (U == Use::Background) return "Background";
		else if constexpr (U == Use::Render) return "Render";
		else if constexpr (U == Use::Hot) return "Hot";
		else std::unreachable(); // C++23: хинт компилятору, что сюда мы никогда не попадем
	}
	template<Use U, size_t RingCap = 4096>
	struct Select;

	template<size_t RingCap>
	struct Select<Use::Background, RingCap>
	{
		using type = thread_pool<LockMode::SingleMutex, RingCap>;
	};

	template<size_t RingCap>
	struct Select<Use::Render, RingCap>
	{
		using type = thread_pool<LockMode::PerQueueMutex, RingCap>;
	};

	template<size_t RingCap>
	struct Select<Use::Hot, RingCap>
	{
		using type = thread_pool<LockMode::LockFree, RingCap>;
	};

	template<Use U, size_t RingCap = 4096>
	using Pool = typename Select<U, RingCap>::type;

	// Удобные алиасы
	using BackgroundPool = Pool<Use::Background>;
	using RenderPool = Pool<Use::Render>;
	using HotPool = Pool<Use::Hot>;

	// Фабрика
	template<Use U, size_t RingCap = 4096>
	[[nodiscard]] POOL_FORCEINLINE std::shared_ptr<Pool<U, RingCap>> make_pool(uint32_t threads)
	{
		return Pool<U, RingCap>::create(threads);
	}
}
namespace engine_pool
{
	using pool_profiles::Use;
	using pool_profiles::BackgroundPool;
	using pool_profiles::RenderPool;
	using pool_profiles::HotPool;
	using pool_profiles::Pool;
	using pool_profiles::make_pool;

	// Концепт-предохранитель: проверяет, что тип T — это НЕ TaskOptions
	template<typename T>
	concept NotTaskOptions = !std::is_same_v<std::remove_cvref_t<T>, TaskOptions>;

	// ─── Готовые фабрики ──────────────────────────────────────────────────────

	[[nodiscard]] POOL_FORCEINLINE std::shared_ptr<BackgroundPool> background(uint32_t threads)
	{
		return pool_profiles::make_pool<Use::Background>(threads);
	}

	[[nodiscard]] POOL_FORCEINLINE std::shared_ptr<RenderPool> render(uint32_t threads)
	{
		return pool_profiles::make_pool<Use::Render>(threads);
	}

	template<size_t Cap = 4096>
	[[nodiscard]] POOL_FORCEINLINE std::shared_ptr<thread_pool<LockMode::LockFree, Cap>> hot(uint32_t threads)
	{
		return pool_profiles::make_pool<Use::Hot, Cap>(threads);
	}

	// ─── Обёртка для произвольного callable ──────────────────────────────────

	struct JobBase
	{
		virtual void run() = 0;
		virtual ~JobBase() = default;
	};

	template<class Fn, class... Args>
	struct Job final : JobBase
	{
		Fn fn;
		std::tuple<Args...> args;
		template<class F, class... As>
		Job(F&& f, As&&... as) : fn(std::forward<F>(f)), args(std::forward<As>(as)...) {}
		void run() override { std::apply(fn, args); }
	};

	// Трамплин для обычной кучи (использует delete через unique_ptr)
	POOL_NOINLINE static void trampoline_heap(void* ctx) noexcept(!PoolConfig::EnableExceptions)
	{
		std::unique_ptr<JobBase> box(static_cast<JobBase*>(ctx));
		box->run();
	}

	// Трамплин для Арены (ТОЛЬКО вызывает деструктор, память не трогает)
	POOL_NOINLINE static void trampoline_arena(void* ctx) noexcept(!PoolConfig::EnableExceptions)
	{
		JobBase* job = static_cast<JobBase*>(ctx);
		job->run();
		std::destroy_at(job); // C++17+: Идиоматичный вызов полиморфного деструктора
	}

	// ─── post_arena: отправка задач без аллокаций на куче ────────────────────
	//
	//  Все объекты Job размещаются в переданной арене через alloc_raw().
	//  Трамплин trampoline_arena вызывает деструктор, но не трогает память —
	//  она живёт до reset() арены.
	//
	//  ВАЖНО: для использования этого шаблона необходимо подключить
	//  "arena_allocator.hpp" до включения этого заголовка или в общем precompiled header.

	template<class PoolT, class ArenaT, class F, class... Args>
	POOL_FORCEINLINE void post_arena(PoolT& pool, ArenaT& arena, F&& f, Args&&... args)
	{
		using Box = Job<std::decay_t<F>, std::decay_t<Args>...>;

		void* mem = arena.alloc_raw(sizeof(Box), alignof(Box));
		if (!mem) [[unlikely]] return;
		POOL_ASSUME(mem != nullptr);

		Box* holder = new (mem) Box(std::forward<F>(f), std::forward<Args>(args)...);

		// Используем специальный трамплин для Арены!
		pool.add_task_fast(TaskOptions{}, FastTask{ &trampoline_arena, holder });
	}

	// ─── post: fire-and-forget с аллокацией на куче ──────────────────────────
	/**
	 * @brief Удобная отправка одиночной задачи (fire-and-forget).
	 *
	 * Упаковывает callable и аргументы в Job через `new`. Подходит для
	 * Background и Render пулов. Для Hot-пула в плотных циклах (100k+ задач/кадр)
	 * используйте post_arena или post_bulk — heap-аллокации уничтожат прирост от LockFree.
	 */
	// Базовая версия — с явным TaskOptions
	template<class PoolT, class F, class... Args>
	POOL_FORCEINLINE void post(PoolT& pool, TaskOptions opts, F&& f, Args&&... args)
	{
		using Box = Job<std::decay_t<F>, std::decay_t<Args>...>;
		auto holder = std::unique_ptr<JobBase>(new Box(std::forward<F>(f), std::forward<Args>(args)...));

		// Используем обычный трамплин для кучи
		pool.add_task_fast(opts, FastTask{ &trampoline_heap, holder.get() });
		holder.release();
	}

	// Сахарная версия — TaskOptions по умолчанию (Normal приоритет)
	template<class PoolT, NotTaskOptions F, class... Args>
	POOL_FORCEINLINE void post(PoolT& pool, F&& f, Args&&... args)
	{
		post(pool, TaskOptions{}, std::forward<F>(f), std::forward<Args>(args)...);
	}

	// ─── post_bulk: пакетная отправка без аллокаций ──────────────────────────

	template<class PoolT>
	POOL_FORCEINLINE void post_bulk(PoolT& pool, TaskOptions opts, std::span<const FastTask> tasks)
	{
		pool.add_task_fast_bulk(opts, tasks);
	}

	template<class PoolT>
	POOL_FORCEINLINE void post_bulk(PoolT& pool, std::span<const FastTask> tasks)
	{
		pool.add_task_fast_bulk(TaskOptions{}, tasks);
	}

	// ─── submit: отправка с получением std::future ───────────────────────────

	// Базовая версия — с явным TaskOptions
	template<class PoolT, class F, class... Args>
	[[nodiscard]] POOL_FORCEINLINE auto submit(PoolT& pool, TaskOptions opts, F&& f, Args&&... args)
	{
		if constexpr (PoolT::lock_mode == LockMode::SingleMutex)
		{
			return pool.submit(opts, std::forward<F>(f), std::forward<Args>(args)...);
		}
		else
		{
			static_assert(PoolT::lock_mode == LockMode::SingleMutex, "engine_pool::submit() is only available for SingleMutex pools.");
			return std::future<void>{};
		}
	}

	// Сахарная версия — TaskOptions по умолчанию (Normal приоритет)
	template<class PoolT, NotTaskOptions F, class... Args>
	[[nodiscard]] POOL_FORCEINLINE  auto submit(PoolT& pool, F&& f, Args&&... args)
	{
		return submit(pool, TaskOptions{}, std::forward<F>(f), std::forward<Args>(args)...);
	}
}
