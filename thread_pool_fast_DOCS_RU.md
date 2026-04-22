# thread_pool_fast.hpp — Руководство разработчика

**Версия:** 2.0.0
**Стандарт:** C++23
**Платформа:** Windows x64, MSVC 2022+
**Файл:** `thread_pool_fast.hpp` (header-only)

---

## Содержание

1. [Что такое thread pool и зачем он нужен](#что-такое-thread-pool-и-зачем-он-нужен)
2. [Требования и подключение](#требования-и-подключение)
3. [Архитектура библиотеки](#архитектура-библиотеки)
4. [Режимы работы (LockMode)](#режимы-работы-lockmode)
5. [Система приоритетов](#система-приоритетов)
6. [Быстрый старт — первые шаги](#быстрый-старт--первые-шаги)
7. [engine_pool — главный API для повседневной работы](#engine_pool--главный-api-для-повседневной-работы)
8. [Низкоуровневый API (thread_pool)](#низкоуровневый-api-thread_pool)
9. [Профили пулов (pool_profiles)](#профили-пулов-pool_profiles)
10. [Телеметрия](#телеметрия)
11. [Логирование (PoolLog)](#логирование-poollog)
12. [Конфигурация (PoolConfig)](#конфигурация-poolconfig)
13. [Версионирование](#версионирование)
14. [Режимы интеграции](#режимы-интеграции)
15. [Макросы компилятора](#макросы-компилятора)
16. [Как работает пул изнутри](#как-работает-пул-изнутри)
17. [Ограничения и важные правила](#ограничения-и-важные-правила)
18. [Выбор нужной функции — шпаргалка](#выбор-нужной-функции--шпаргалка)

---

## Что такое thread pool и зачем он нужен

Представьте, что у вас есть рендер-движок и каждый кадр нужно обработать тысячи мелких задач: загрузить текстуру, обновить физику частицы, перестроить меш. Создавать и удалять отдельный поток (`std::thread`) под каждую задачу — очень дорого: ОС тратит время на выделение стека, планирование и уничтожение потока.

**Thread pool** решает эту проблему: вы создаёте фиксированный набор рабочих потоков (воркеров) один раз при старте, а потом просто кидаете задачи в очередь. Воркеры сами забирают задачи и выполняют их. Никаких лишних аллокаций — только работа.

`thread_pool_fast.hpp` — это однофайловая реализация такого пула, заточенная под рендер-движки. Её главные свойства:

- **Три режима** с разным балансом простоты и скорости
- **Приоритеты задач** — срочные задачи выполняются раньше
- **Нулевые аллокации** на горячем пути в LockFree режиме
- **Header-only** — просто включить один `.hpp` файл

---

## Требования и подключение

Для использования библиотеки необходимо:

- **ОС:** Windows (используются `YieldProcessor`, `SetThreadPriority` из WinAPI)
- **Компилятор:** MSVC 2022+ (`_MSC_VER >= 1930`)
- **Стандарт:** C++23 (`/std:c++latest` или `/std:c++23`)
- **Разрядность:** 64-bit (`x64`)

### Standalone-режим (отдельный проект)

Добавьте файл в проект и подключите с макросом `POOL_STANDALONE`:

```cpp
#define POOL_STANDALONE
#include "thread_pool_fast.hpp"
```

Этот макрос говорит библиотеке самостоятельно подключить `<windows.h>` и определить все нужные макросы компилятора. Использовать в проектах вне движка.

### Embedded-режим (внутри движка)

Без `POOL_STANDALONE` библиотека ожидает файл `../common.hpp` от движка, который должен определять макросы `COM_ASSUME`, `FAST_FORCEINLINE`, `COM_NOINLINE`, `COM_ALIGN_AS`. Подробнее в разделе [Режимы интеграции](#режимы-интеграции).

---

## Архитектура библиотеки

Библиотека устроена как три уровня — от самого удобного до самого быстрого:

```
┌─────────────────────────────────────────────────────────────┐
│  engine_pool  — синтаксический сахар, удобство, безопасность │  ← начинайте здесь
├─────────────────────────────────────────────────────────────┤
│  pool_profiles — профили (Background / Render / Hot)         │  ← выбор сценария
├─────────────────────────────────────────────────────────────┤
│  thread_pool<Mode, Cap> — ядро, горячий путь без аллокаций   │  ← максимальная скорость
└─────────────────────────────────────────────────────────────┘
```

**Для большинства задач** используйте только `engine_pool` — он скрывает детали реализации и предоставляет удобный синтаксис. К нижним уровням обращайтесь только когда нужна максимальная производительность или тонкая настройка.

---

## Режимы работы (LockMode)

Режим определяет, как воркеры получают задачи из очереди. Это первый шаблонный параметр `thread_pool<LockMode, RingCap>`.

### `LockMode::SingleMutex` — один мьютекс

Самый простой режим. Все четыре очереди приоритетов защищены одним `std::mutex`. Воркеры ждут задач через `std::condition_variable`.

| Параметр | Значение |
|---|---|
| Скорость | ~1–5 мкс на задачу (ОС-планировщик) |
| Конкуренция | Низкая–средняя |
| Приоритеты | 4 полноценных уровня (`Critical → High → Normal → Low`) |
| Rich API (`add_task` / `submit`) | ✅ доступно |
| Обнаружение голодания (Low/Normal/High) | ✅ предупреждение через `PoolLog::warn` |
| Приоритет потока (Windows) | `THREAD_PRIORITY_BELOW_NORMAL` |
| Псевдоним | `ThreadPoolSingle` |
| **Когда использовать** | Фоновая загрузка ресурсов, I/O, стриминг |

### `LockMode::PerQueueMutex` — мьютекс на каждую очередь

Каждый из четырёх приоритетных уровней имеет свой `std::mutex`. Это снижает конкуренцию, когда задачи разных приоритетов поступают одновременно.

| Параметр | Значение |
|---|---|
| Скорость | ~1–5 мкс на задачу |
| Конкуренция | Ниже, чем у SingleMutex |
| Приоритеты | 4 независимых уровня, полная гранулярность |
| Rich API | ❌ (compile-time ошибка) |
| Псевдоним | `ThreadPoolPerQueue` |
| **Когда использовать** | Рендер-пайплайн со смешанными приоритетами |

### `LockMode::LockFree` — без блокировок

Самый быстрый режим. Использует кольцевой буфер Vyukov MPMC — никаких `mutex` и `condition_variable` на горячем пути. Вместо этого — атомарные операции (`CAS`). Воркеры сначала «крутятся» (`YieldProcessor`), и только потом уходят в OS-сон.

| Параметр | Значение |
|---|---|
| Скорость | < 100 нс на горячем пути |
| Конкуренция | Минимальная (только CAS) |
| Приоритеты | **2 бина** (подробнее ниже) |
| Ёмкость очереди | Шаблонный параметр `RingCap` (по умолчанию 4096) |
| Rich API | ❌ |
| Приоритет потока (Windows) | `THREAD_PRIORITY_ABOVE_NORMAL` |
| Псевдоним | `ThreadPoolFast<RingCap>` |
| **Когда использовать** | Горячий рендер-цикл, частицы, жёсткий бюджет кадра |

> ⚠️ **Важно про приоритеты в LockFree:** из-за особенностей кольцевого буфера 4 приоритета «схлопываются» в 2 физических очереди:
> - `Critical` и `High` → bin 1 (обрабатывается первым)
> - `Normal` и `Low` → bin 0
>
> Порядок задач внутри одного бина не гарантирован. Если вам нужна полная 4-уровневая гранулярность — используйте `SingleMutex` или `PerQueueMutex`.

---

## Система приоритетов

Каждая задача отправляется с приоритетом. Воркеры всегда сначала опустошают очереди с более высоким приоритетом.

```cpp
enum class Priority : int {
    Low      = 0,  // фоновая работа (стриминг, подготовка данных на будущее)
    Normal   = 1,  // по умолчанию — большинство задач
    High     = 2,  // срочные задачи текущего кадра
    Critical = 3,  // максимальный приоритет, выполнить немедленно
};
```

Приоритет передаётся через `TaskOptions`:

```cpp
struct TaskOptions {
    Priority priority = Priority::Normal;
};
```

Порядок обработки в `SingleMutex` и `PerQueueMutex`: `Critical → High → Normal → Low`.

### Обнаружение и устранение голодания (только SingleMutex)

Каждый раз после выполнения задачи воркер проверяет очереди 0 (Low), 1 (Normal),
2 (High) снизу вверх. Если найдена непустая очередь и задачи в ней ждут дольше
`PoolConfig::LowStarveSec` секунд (по умолчанию 3):

1. Через `PoolLog::warn` выдаётся предупреждение
2. Первая задача из голодающей очереди **немедленно перемещается в `Critical` (queue[3])**
3. На следующей итерации воркер заберёт её первой и выполнит


### Обнаружение и устранение голодания (только SingleMutex)

Каждый раз после пробуждения воркер проверяет очереди 0 (Low), 1 (Normal),
2 (High) снизу вверх. Если найдена непустая очередь и задачи в ней ждут дольше
`PoolConfig::LowStarveSec` секунд (по умолчанию 3):

1. Через `PoolLog::warn` выдаётся предупреждение с указанием реального порога и номера очереди
2. Первая задача из голодающей очереди **немедленно перемещается в `Critical` (queue[3])**
3. На следующей итерации воркер заберёт её первой и выполнит

Таким образом голодание не просто фиксируется — оно устраняется. За один проход
продвигается ровно одна задача из **самой низкоприоритетной** голодающей очереди
(алгоритм останавливается на первой найденной через `break`). Если одновременно
голодают несколько очередей — следующие будут обработаны на следующих итерациях.
Таймер каждой очереди сбрасывается независимо, как только она опустевает.

---

## Быстрый старт — первые шаги

Вот минимальный рабочий пример, который покрывает 90% типичного использования:

```cpp
#define POOL_STANDALONE
#include "thread_pool_fast.hpp"

int main()
{
    // 1. Создать пул — 4 рабочих потока, режим Background (SingleMutex)
    auto pool = engine_pool::background(4);

    // 2. Отправить задачи — fire and forget
    engine_pool::post(*pool, [] {
        // эта лямбда выполнится в одном из рабочих потоков
        load_texture("sky.png");
    });

    // 3. Отправить задачу с приоритетом
    engine_pool::post(*pool, TaskOptions{Priority::High}, [] {
        render_shadow_pass();
    });

    // 4. Дождаться, пока все задачи выполнятся
    pool->wait_all();

    // 5. При уничтожении shared_ptr деструктор сам вызовет wait_all() + join()
    return 0;
}
```

Вот и всё — три строки кода для реального многопоточного выполнения. Разберём каждую возможность подробнее.

---

## engine_pool — главный API для повседневной работы

Пространство имён `engine_pool` — это главный уровень, с которым вы будете работать чаще всего. Он предоставляет удобный синтаксис и скрывает детали реализации.

### Создание пула

Три фабричные функции для трёх сценариев использования:

```cpp
// Background — для фоновой загрузки, I/O, стриминга (SingleMutex)
auto bg = engine_pool::background(4);   // 4 рабочих потока

// Render — для рендер-пайплайна (PerQueueMutex)
auto rnd = engine_pool::render(6);      // 6 рабочих потоков

// Hot — для горячего цикла, максимальная скорость (LockFree)
auto hot = engine_pool::hot(4);         // ring = 4096 (по умолчанию)
auto hot2 = engine_pool::hot<8192>(4);  // ring = 8192 (явный размер)
```

Все три функции возвращают `std::shared_ptr` на соответствующий тип пула. При уничтожении `shared_ptr` деструктор пула автоматически вызывает `wait_all()` и `join()` для всех воркеров — утечек потоков не будет.

---

### `post()` — отправить задачу (fire-and-forget)

Самая используемая функция. Принимает любой callable (лямбду, функцию, метод объекта) и опционально аргументы. Под капотом упаковывает всё через `new` и отправляет в очередь.

```cpp
// Синтаксис:
engine_pool::post(pool, callable, args...);
engine_pool::post(pool, TaskOptions{priority}, callable, args...);
```

**Примеры:**

```cpp
auto pool = engine_pool::background(4);

// Простая лямбда без аргументов
engine_pool::post(*pool, [] { load_sound("music.ogg"); });

// Лямбда с явным приоритетом
engine_pool::post(*pool, TaskOptions{Priority::Critical}, [] { flush_render_queue(); });

// Лямбда с захватом переменной
std::string path = "textures/hero.png";
engine_pool::post(*pool, [path]() mutable {
    load_texture(std::move(path));
});

// Свободная функция с аргументами
engine_pool::post(*pool, compute_lod, mesh_ptr, camera_ptr);

// Метод объекта
engine_pool::post(*pool, &Renderer::draw_pass, &renderer, frame_id);

// Приоритет + аргументы
engine_pool::post(*pool, TaskOptions{Priority::High}, update_particles, &particles, dt);
```

> ⚠️ **Hot-пул и `post()`:** каждый вызов делает `new`. Если вы отправляете 100 000+ задач за кадр через Hot-пул, аллокации полностью уничтожат выигрыш от LockFree режима. В этом случае используйте `post_bulk()` или `post_arena()`.

---

### `post_bulk()` — отправить пакет задач без аллокаций

Когда нужно отправить много задач сразу. Принимает `std::span<const FastTask>` — массив уже готовых задач. Нулевые аллокации.

```cpp
// Синтаксис:
engine_pool::post_bulk(pool, tasks_span);
engine_pool::post_bulk(pool, TaskOptions{priority}, tasks_span);
```

**Что такое `FastTask`?**

```cpp
struct FastTask {
    void (*fn)(void* ctx) = nullptr;  // указатель на функцию-обработчик
    void* ctx = nullptr;              // пользовательские данные
};
```

`FastTask` — это просто два указателя. Никаких виртуальных функций, никаких аллокаций. Данные (`ctx`) должны оставаться живыми до завершения задачи.

**Пример:**

```cpp
auto pool = engine_pool::hot(4);

// Подготовить пакет задач заранее
std::array<FastTask, 64> batch;
for (int i = 0; i < 64; ++i) {
    batch[i] = { process_chunk_fn, &chunks[i] };
    // chunks[i] должен прожить до wait_all()
}

// Один вызов — весь пакет в очередь
engine_pool::post_bulk(*pool, batch);
pool->wait_all();
```

`post_bulk` — идеальный выбор для Hot-пула: один вызов `add_task_fast_bulk` отправляет всю партию.

---

### `post_arena()` — отправить задачу без heap вообще

Для ситуаций, когда нужен удобный синтаксис (`post` с лямбдой), но без единой аллокации. Память под задачу берётся из пользовательской арены через placement new.

```cpp
// Синтаксис:
engine_pool::post_arena(pool, arena, callable, args...);
```

**Требования к арене:** тип арены должен предоставлять метод:
```cpp
void* arena.alloc_raw(size_t size, size_t alignment);
// возвращает nullptr при нехватке памяти
```

> ⚠️ **Зависимость от реализации арены:** `post_arena` использует `alloc_raw(size, align)` — низкоуровневый метод без проверок на тип. Убедитесь, что ваша реализация `ArenaAllocator` этот метод предоставляет. Стандартные методы `alloc<T>()` и `alloc_uninit<T>()` для этого **не подходят** — они шаблонные и требуют тривиального типа. Если `alloc_raw` отсутствует — `post_arena` не скомпилируется.

**Пример:**

```cpp
#include "arena_allocator.hpp"  // ваш аллокатор

FrameArena arena(1 * 1024 * 1024);  // 1 МБ под задачи кадра
auto pool = engine_pool::hot(4);

for (const auto& chunk : world_chunks) {
    engine_pool::post_arena(*pool, arena, [&chunk] {
        chunk.rebuild_mesh();
    });
}

pool->wait_all();
arena.reset();  // ТОЛЬКО после wait_all()!
```

> ⚠️ **Критически важный порядок:** `arena.reset()` или уничтожение арены допускается **строго после** `pool->wait_all()`. Если сбросить арену раньше — воркеры обратятся к уже освобождённой памяти, и программа упадёт.

> ⚠️ **`post_arena` не поддерживает `TaskOptions`** — всегда использует `Priority::Normal`. Для задач с приоритетом вызывайте `pool->add_task_fast()` напрямую.

---

### `submit()` — отправить задачу и получить результат

Если задача должна что-то вернуть или вам нужно дождаться конкретной задачи (а не всех), используйте `submit()`. Возвращает `std::future<T>`.

**Доступно только для `BackgroundPool` (SingleMutex).** Попытка вызвать на Render или Hot пуле — compile-time ошибка.

```cpp
// Синтаксис:
auto fut = engine_pool::submit(pool, callable, args...);
auto fut = engine_pool::submit(pool, TaskOptions{priority}, callable, args...);
T result = fut.get();  // блокируем до результата
```

**Примеры:**

```cpp
auto pool = engine_pool::background(4);

// Задача, которая возвращает значение
auto fut = engine_pool::submit(*pool, [] {
    return compute_checksum(data);
});
uint64_t hash = fut.get();  // ждём результат

// С приоритетом
auto fut2 = engine_pool::submit(*pool, TaskOptions{Priority::High}, [] {
    return build_aabb(mesh);
});
BoundingBox bb = fut2.get();

// Задача без возвращаемого значения
auto fut3 = engine_pool::submit(*pool, [&] { rebuild_bvh(scene); });
fut3.get();  // просто ждём завершения

// С аргументами
auto fut4 = engine_pool::submit(*pool, TaskOptions{Priority::Normal}, load_asset, "hero.fbx", 2);
AssetHandle handle = fut4.get();
```

Если лямбда бросает исключение, оно безопасно сохраняется в `std::promise` и перебрасывается при вызове `fut.get()`.

---

### Сводная таблица engine_pool API

| Функция | Аллокация | Приоритет | Future | Пулы |
|---|---|---|---|---|
| `background(n)` | `shared_ptr` | — | — | Создаёт Background |
| `render(n)` | `shared_ptr` | — | — | Создаёт Render |
| `hot<Cap>(n)` | `shared_ptr` | — | — | Создаёт Hot |
| `post(pool, fn)` | `new` | Normal | ❌ | Все |
| `post(pool, opts, fn)` | `new` | Явный | ❌ | Все |
| `post_bulk(pool, tasks)` | ❌ нет | Normal | ❌ | Все |
| `post_bulk(pool, opts, tasks)` | ❌ нет | Явный | ❌ | Все |
| `post_arena(pool, arena, fn)` | Арена | Normal | ❌ | Все |
| `submit(pool, fn)` | `new` | Normal | ✅ | **SingleMutex** |
| `submit(pool, opts, fn)` | `new` | Явный | ✅ | **SingleMutex** |

---

## Низкоуровневый API (thread_pool)

Прямая работа с классом `thread_pool<Mode, RingCap>`. Используйте этот уровень, когда `engine_pool` недостаточно гибок или нужен максимальный контроль.

### Создание

```cpp
// Единственный правильный способ — через статический метод create():
auto pool = thread_pool<LockMode::LockFree, 4096>::create(8);

// Прямой вызов конструктора намеренно заблокирован извне (ctor_token).
```

Метод `create(n)` бросает `std::invalid_argument` если `n == 0` (в Debug). При ошибке создания потоков выполняет cleanup и перебрасывает исключение — утечек не будет.

### Горячий путь — быстрая отправка

```cpp
void add_task_fast(TaskOptions opts, FastTask ft);
void add_task_fast_bulk(TaskOptions opts, std::span<const FastTask> tasks);
```

Нулевые аллокации. Работает во всех трёх режимах. Это и есть «горячий путь», который `engine_pool` вызывает внутри себя.

**Пример прямого использования:**

```cpp
struct DrawData { Mesh* mesh; Camera* cam; };

void draw_fn(void* ctx) {
    auto* d = static_cast<DrawData*>(ctx);
    render(d->mesh, d->cam);
}

DrawData data{ mesh, cam };  // должен пережить wait_all()
pool->add_task_fast(TaskOptions{Priority::High}, FastTask{ draw_fn, &data });
pool->wait_all();
```

### Rich API (только SingleMutex)

Удобные обёртки с аллокацией. Когда нужен `std::future` или захват произвольных аргументов:

```cpp
// Отправить задачу без результата
pool->add_task([] { do_work(); });
pool->add_task(TaskOptions{Priority::High}, process, arg1, arg2);

// Отправить задачу и получить future
auto fut = pool->submit([] { return compute(); });
auto result = fut.get();
```

> Эти методы доступны только для `thread_pool<LockMode::SingleMutex>`. На других режимах — compile-time ошибка с понятным сообщением.

### Синхронизация

```cpp
pool->wait_all();  // блокирует вызывающий поток до завершения ВСЕХ задач
```

Поведение зависит от режима:
- **LockFree:** активный spin-wait (800 × `YieldProcessor()`), затем `std::this_thread::yield()` — минимальный jitter, хорошо для Hot-пула.
- **SingleMutex / PerQueueMutex:** futex-сон через `std::atomic::wait` — экономит CPU пока задачи выполняются.

Деструктор пула автоматически вызывает `wait_all()` перед остановкой воркеров.

### Прочие методы

```cpp
uint32_t thread_count() const noexcept;  // сколько воркеров в пуле
static constexpr LockMode lock_mode;     // режим пула (удобно в шаблонах)
```

---

## Профили пулов (pool_profiles)

Пространство имён `pool_profiles` связывает смысловые сценарии с конкретными режимами. `engine_pool` построен поверх него.

```cpp
namespace pool_profiles {
    enum class Use { Background, Render, Hot };
}
```

| Профиль | Режим | Сценарий |
|---|---|---|
| `Use::Background` | `SingleMutex` | Загрузка ресурсов, стриминг, I/O |
| `Use::Render` | `PerQueueMutex` | Рендер-пайплайн, средняя конкуренция |
| `Use::Hot` | `LockFree` | Горячий цикл, частицы, жёсткий кадровый бюджет |

### Типы, псевдонимы и фабрика

```cpp
// Получить тип пула по профилю:
using MyPool = pool_profiles::Pool<pool_profiles::Use::Hot, 8192>;

// Готовые псевдонимы:
pool_profiles::BackgroundPool   // = thread_pool<SingleMutex>
pool_profiles::RenderPool       // = thread_pool<PerQueueMutex>
pool_profiles::HotPool          // = thread_pool<LockFree, 4096>

// Фабрика (возвращает shared_ptr):
auto pool = pool_profiles::make_pool<pool_profiles::Use::Render>(8);

// Имя профиля как строка (compile-time):
constexpr auto name = pool_profiles::name<pool_profiles::Use::Hot>(); // "Hot"
```

---

## Телеметрия

Библиотека умеет собирать статистику работы. Все счётчики имеют **нулевую стоимость в Release** — ветка `if constexpr (EnableTelemetry)` полностью исчезает при `-DNDEBUG`. В Debug — полностью активны.

```cpp
struct PoolTelemetry {
    uint64_t total_submitted;          // задач отправлено в пул
    uint64_t total_executed;           // задач выполнено
    uint64_t sleep_cycles;             // сколько раз воркеры уходили в OS-сон
    uint64_t push_spins;               // итераций ожидания свободного слота (LockFree)
    uint64_t wait_all_calls;           // вызовов wait_all()
    uint64_t low_starvation_warnings;  // эпизодов голодания (Low/Normal/High) — только SingleMutex
};

// Использование:
PoolTelemetry t = pool->telemetry();
printf("Submitted: %llu, Executed: %llu\n", t.total_submitted, t.total_executed);
```

В Release `telemetry()` возвращает нулевую структуру без каких-либо атомарных чтений.

### Как читать показатели

| Метрика | Что означает |
|---|---|
| `push_spins > 0` | Ring buffer переполнился — увеличьте `RingCap` или снизьте burst |
| `sleep_cycles / total_executed` велик | Воркеры часто простаивают — пул избыточен для нагрузки |
| `total_submitted != total_executed` после `wait_all()` | Счётчик рассинхронизирован — это баг |
| `low_starvation_warnings > 0` | Зафиксированы эпизоды голодания очередей Low/Normal/High. Задачи автоматически подняты до Critical и выполнены. Если счётчик растёт — снижайте нагрузку высокоприоритетных очередей или добавляйте воркеров |

---

## Логирование (PoolLog)

По умолчанию сообщения пула выводятся через `std::printf`. Чтобы перенаправить их в логгер вашего движка, установите коллбеки до создания пулов:

```cpp
// Установить один раз при старте движка:
PoolLog::warn_fn = [](const char* msg) noexcept {
    engine::log::warn("[Pool] {}", msg);
};
PoolLog::info_fn = [](const char* msg) noexcept {
    engine::log::info("[Pool] {}", msg);
};
```

**Требования к коллбекам:**
- Должны быть `noexcept` — они вызываются из рабочих потоков
- Должны быть потокобезопасны

Если коллбек не установлен, сообщения уходят в `std::printf("[WARN] ...")` / `std::printf("[INFO] ...")`.

---

## Конфигурация (PoolConfig)

```cpp
namespace PoolConfig {
    inline constexpr bool EnableTelemetry  = /* Debug: true,  Release: false */;
    inline constexpr bool EnableExceptions = /* Debug: true,  Release: false */;
    inline constexpr int  LowStarveSec     = 3;  // порог голодания очередей Low/Normal/High (секунды)
}
```

`EnableTelemetry` и `EnableExceptions` выставляются **автоматически** на основе `NDEBUG` — вам не нужно трогать эти флаги вручную:

- **Debug** (без `NDEBUG`): оба `true` — ошибки бросают исключения с описанием, телеметрия активна
- **Release** (с `NDEBUG`): оба `false` — нулевой оверхед, ошибки вызывают `std::abort()`

`LowStarveSec` можно изменить, отредактировав заголовок. Это порог в секундах, после которого `SingleMutex` предупреждает о голодании очередей Low, Normal и High.

### Ожидаемая просадка производительности в Debug

В Debug-сборке пул работает значительно медленнее — это штатное поведение:

| Режим | Release | Debug | Падение |
|---|---|---|---|
| SingleMutex | ~2.0 Mops/s | ~0.29 Mops/s | ~7× |
| PerQueueMutex | ~3.3 Mops/s | ~0.83 Mops/s | ~4× |
| LockFree | ~14.7 Mops/s | ~3.25 Mops/s | ~4.5× |

Основные причины просадки:
- `POOL_FORCEINLINE` не работает при `/Od` — все горячие функции становятся реальными вызовами
- `std::vector` в Debug включает `_ITERATOR_DEBUG_LEVEL=2` — bounds-check на каждой операции
- `std::mutex` в Debug содержит дополнительную валидацию состояния
- `assert()` в горячем пути активен и компилируется
- Все атомарные операции телеметрии активны и идут через библиотечный вызов
- Полный механизм исключений в каждой функции с `check_running()`

Чтобы уменьшить просадку в Debug без перехода в Release, добавьте перед включением заголовка:
```cpp
#define _ITERATOR_DEBUG_LEVEL 0
```

Также в конфигурации стоит compile-time гарантия 64-битной сборки:
```cpp
static_assert(sizeof(void*) == 8, "64-bit build required");
```

---

## Версионирование

```cpp
namespace PoolVersion {
    constexpr Version get() noexcept;           // { major=2, minor=0, patch=0 }
    constexpr const char* as_string() noexcept; // "2.0.0"
}

// Препроцессорные макросы:
POOL_VERSION_MAJOR   // 2
POOL_VERSION_MINOR   // 0
POOL_VERSION_PATCH   // 0
POOL_VERSION_STRING  // "2.0.0"
POOL_VERSION_INT     // 20000  (major*10000 + minor*100 + patch)
```

Проверка совместимости во время компиляции — полезно, если несколько модулей проекта используют библиотеку:

```cpp
#if POOL_VERSION_INT < 20000
#  error "thread_pool_fast >= 2.0.0 required"
#endif
```

---

## Режимы интеграции

### Standalone (`POOL_STANDALONE`)

Для любых проектов вне движка. Все макросы определяются прямо в заголовке.

```cpp
#define POOL_STANDALONE
#include "thread_pool_fast.hpp"
```

### Embedded (внутри движка)

Без `POOL_STANDALONE` заголовок подключает `../common.hpp` движка и маппит макросы:

```cpp
// Без POOL_STANDALONE — embedded режим
#include "thread_pool_fast.hpp"
```

`common.hpp` должен определять (иначе `#error` при компиляции):

| Макрос пула | Макрос движка | Что делает |
|---|---|---|
| `POOL_ASSUME` | `COM_ASSUME` | Подсказка компилятору (UB hint) |
| `POOL_FORCEINLINE` | `FAST_FORCEINLINE` | Принудительный инлайнинг |
| `POOL_NOINLINE` | `COM_NOINLINE` | Запрет инлайнинга |
| `POOL_ALIGN_AS` | `COM_ALIGN_AS` | Выравнивание по кэш-линии |

---

## Макросы компилятора

Библиотека использует четыре внутренних макроса для максимальной производительности:

| Макрос | MSVC | GCC/Clang | Иное |
|---|---|---|---|
| `POOL_ASSUME(x)` | `__assume(x)` | `[[assume(x)]]` (C++23) | пусто |
| `POOL_FORCEINLINE` | `__forceinline` | `__attribute__((always_inline))` | `inline` |
| `POOL_NOINLINE` | `__declspec(noinline)` | `__attribute__((noinline))` | пусто |
| `POOL_ALIGN_AS(n)` | `alignas(n) __declspec(align(n))` | `alignas(n)` | `alignas(n)` |

Вы можете переопределить любой из них до включения заголовка — они используют `#ifndef`.

---

## Как работает пул изнутри

Этот раздел не нужен для использования библиотеки, но поможет понять, почему те или иные правила важны.

### Жизненный цикл задачи

```
1. Вы вызываете post() / add_task_fast()
2. Задача помещается в очередь нужного приоритета
3. unfinished_tasks_++ (атомарно)
4. Один из воркеров забирает задачу из очереди
5. Воркер выполняет fn(ctx)
6. local_completed++ (локальный счётчик воркера)
7. Каждые 64 задачи: unfinished_tasks_ -= local_completed (один атомарный op)
8. Если unfinished_tasks_ стал 0 — будим wait_all()
```

### Batch-commit: зачем воркеры не сбрасывают счётчик сразу

Вместо того чтобы делать `fetch_sub(1)` после каждой задачи, воркеры накапливают локальный счётчик `local_completed` и сбрасывают его пачкой каждые 64 задачи. Это снижает атомарный трафик на шину памяти в 64 раза.

### Shutdown-последовательность

```
~thread_pool():
  1. wait_all()     ← ждём, пока все задачи завершатся
  2. signal_stop()  ← устанавливаем state = stopped, будим всех воркеров
  3. join all       ← ждём, пока каждый воркер завершит свой цикл
```

Порядок критичен: нельзя сначала `signal_stop()`, а потом `wait_all()` — иначе воркеры завершатся раньше, чем успеют доделать задачи.

### Цикл воркера в LockFree

```
1. pop_best_ready() — сначала bin[1] (High|Critical), потом bin[0] (Normal|Low)
2. Нашли задачу    → выполнить, local_completed++
                     если local_completed >= 64 → batch-commit
3. Не нашли + stopped → commit, выход
4. Не нашли        → спин 2000 × YieldProcessor(), снова pop
5. Всё равно пусто → sleeping_threads++,
                     double-check any_ready/stopped,
                     wake_signal.wait() (OS futex),
                     sleeping_threads--
```

### Цикл воркера в SingleMutex

```
1. unique_lock(mtx)
2. cv.wait([](){ return any_ready || stopped; })
3. pop_best_ready() под мьютексом
4. unlock, выполнить, local_completed++
5. если local_completed >= 64 → batch-commit
```

---

## Ограничения и важные правила

### Только Windows

Используются `YieldProcessor()` (x86 `PAUSE`) и `SetThreadPriority()` из `<windows.h>`. Linux и macOS не поддерживаются.

### `RingCap` должен быть степенью двойки

```cpp
// Правильно:
engine_pool::hot<4096>(4);
engine_pool::hot<8192>(4);
engine_pool::hot<16384>(4);

// Неправильно — compile-time ошибка:
engine_pool::hot<5000>(4);  // static_assert: RingCap must be power of 2
```

Допустимые значения: 4, 8, 16, 32, ..., 4096, 8192, 16384, ...

### Размер очереди в LockFree ограничен

Если все `RingCap` слотов заняты, `push_one` и `push_bulk` будут спинить в ожидании свободного места. При `push_spins > 0` в телеметрии — увеличьте `RingCap`.

### Время жизни контекста FastTask

Пул **не управляет** временем жизни `ctx`. Вы обязаны обеспечить, чтобы объект по `ctx` был жив до момента выполнения задачи.

**Паттерн: передача владения через unique_ptr**

```cpp
// Создаём данные на куче и передаём владение задаче
auto* data = new MyData{ ... };
pool->add_task_fast({}, FastTask{
    [](void* ctx) {
        // unique_ptr удалит data после выполнения
        std::unique_ptr<MyData> d(static_cast<MyData*>(ctx));
        process(*d);
    },
    data
});
```

**Паттерн: стековые данные (только если wait_all до уничтожения)**

```cpp
DrawData data{ mesh, cam };           // стек
pool->add_task_fast(TaskOptions{Priority::High}, FastTask{ draw_fn, &data });
pool->wait_all();                     // гарантирует, что data ещё жива
// data уничтожается здесь — безопасно
```

---

## Выбор нужной функции — шпаргалка

```
Нужен результат / future?
  └─► YES → engine_pool::submit()       [только BackgroundPool / SingleMutex]
  └─► NO  → одиночная задача или пакет?
              └─► Пакет (много задач сразу)
                    └─► engine_pool::post_bulk()  [нулевые аллокации]
              └─► Одиночная задача
                    └─► Hot-пул + много задач/кадр (100k+)?
                          └─► YES → engine_pool::post_arena()  [нулевые аллокации]
                          └─► NO  → engine_pool::post()        [удобно, heap]
```

**Выбор типа пула:**

```
Что делает задача?
  └─► Загрузка файлов, I/O, стриминг    → engine_pool::background()  (SingleMutex)
  └─► Рендер, смешанные приоритеты      → engine_pool::render()      (PerQueueMutex)
  └─► Горячий цикл, частицы, <100 нс    → engine_pool::hot()         (LockFree)
```

---

*Документация составлена по исходному коду `thread_pool_fast.hpp` v2.0.0*
