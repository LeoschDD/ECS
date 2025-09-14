#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>
#include <atomic>

#include "spire_ecs.h"

using namespace spire::ecs;
using SteadyClock = std::chrono::steady_clock;

struct Position { float x, y; };
struct Velocity { float x, y; };
struct Accel    { float x, y; };
struct Health   { float hp;    };

template <class F>
static long long time_ms(F&& f)
{
    auto t0 = SteadyClock::now();
    f();
    auto t1 = SteadyClock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

static void init_world(Registry& reg, size_t N)
{
    reg.registerComponent<Position>();
    reg.registerComponent<Velocity>();
    reg.registerComponent<Accel>();
    reg.registerComponent<Health>();

    for (size_t i = 0; i < N; ++i)
    {
        Entity e = reg.create();

        float fx = static_cast<float>(i % 1000) * 0.001f;
        float fy = static_cast<float>((i / 1000) % 1000) * 0.001f;

        reg.addComponent<Position>(e.id(), Position{fx, fy});
        reg.addComponent<Velocity>(e.id(), Velocity{fx * 0.5f + 0.01f, fy * 0.5f + 0.02f});
        reg.addComponent<Accel>(e.id(),    Accel{0.0001f + fx * 0.00001f, -0.0002f + fy * 0.00001f});
        reg.addComponent<Health>(e.id(),   Health{100.0f});
    }
}

// -------------------------------
// Single-threaded Systeme (Views)
// -------------------------------

static void sys_apply_accel_ST(Registry& reg, float dt)
{
    auto v = reg.view<Velocity, Accel>();
    v->each([&](EntityID, Velocity& vel, const Accel& a){
        vel.x += a.x * dt;
        vel.y += a.y * dt;
    });
}

static void sys_integrate_ST(Registry& reg, float dt)
{
    auto v = reg.view<Position, Velocity>();
    v->each([&](EntityID, Position& p, const Velocity& vel){
        p.x += vel.x * dt;
        p.y += vel.y * dt;
    });
}

static void sys_health_decay_ST(Registry& reg, float dt)
{
    auto v = reg.view<Health>();
    v->each([&](EntityID, Health& h){
        h.hp -= 0.01f * dt * 1000.0f;
        if (h.hp < 0.0f) h.hp = 0.0f;
    });
}

static long long simulate_singlethreaded(Registry& reg, int steps, float dt)
{
    return time_ms([&]{
        for (int i = 0; i < steps; ++i)
        {
            sys_apply_accel_ST(reg, dt);
            sys_integrate_ST(reg, dt);
            sys_health_decay_ST(reg, dt);
        }
    });
}

// ---------------------------------
// Multi-threaded Systeme (Ranges)
// ---------------------------------

template <typename Fn>
static void parallel_for_indices(size_t begin, size_t end, unsigned threads, Fn&& fn)
{
    threads = std::max(1u, threads);
    size_t total = end - begin;
    size_t chunk = (total + threads - 1) / threads;

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t)
    {
        size_t b = begin + t * chunk;
        if (b >= end) break;
        size_t e = std::min(end, b + chunk);
        pool.emplace_back([=,&fn]{
            fn(b, e);
        });
    }
    for (auto& th : pool) th.join();
}

static void sys_apply_accel_MT(Registry& reg, float dt, unsigned threads)
{
    const auto& alive = reg.alive();
    parallel_for_indices(0, alive.size(), threads, [&](size_t b, size_t e){
        for (size_t i = b; i < e; ++i)
        {
            EntityID id = alive[i];
            auto* v = reg.getComponent<Velocity>(id);
            const auto* a = reg.getComponent<Accel>(id);
            v->x += a->x * dt;
            v->y += a->y * dt;
        }
    });
}

static void sys_integrate_MT(Registry& reg, float dt, unsigned threads)
{
    const auto& alive = reg.alive();
    parallel_for_indices(0, alive.size(), threads, [&](size_t b, size_t e){
        for (size_t i = b; i < e; ++i)
        {
            EntityID id = alive[i];
            auto* p = reg.getComponent<Position>(id);
            const auto* v = reg.getComponent<Velocity>(id);
            p->x += v->x * dt;
            p->y += v->y * dt;
        }
    });
}

static void sys_health_decay_MT(Registry& reg, float dt, unsigned threads)
{
    const auto& alive = reg.alive();
    parallel_for_indices(0, alive.size(), threads, [&](size_t b, size_t e){
        for (size_t i = b; i < e; ++i)
        {
            EntityID id = alive[i];
            auto* h = reg.getComponent<Health>(id);
            h->hp -= 0.01f * dt * 1000.0f;
            if (h->hp < 0.0f) h->hp = 0.0f;
        }
    });
}

static long long simulate_multithreaded(Registry& reg, int steps, float dt, unsigned threads)
{
    return time_ms([&]{
        for (int i = 0; i < steps; ++i)
        {
            sys_apply_accel_MT(reg, dt, threads);
            sys_integrate_MT(reg, dt, threads);
            sys_health_decay_MT(reg, dt, threads);
        }
    });
}

int test()
{
    constexpr size_t N = spire::ecs::MAX_ENTITIES;
    constexpr int    STEPS = 3;
    constexpr float  DT    = 0.016f;

    std::cout << "Init " << N << " entities ...\n";
    Registry reg;

    auto t_init = time_ms([&]{
        init_world(reg, N);
    });

    // Warmup: View bauen, damit ST-Messung fair ist (kein erster Cache-Build)
    auto warm = reg.view<Position, Velocity>();
    warm->each([](EntityID, Position&, Velocity&){});

    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned threads = hw;

    auto t_st = simulate_singlethreaded(reg, STEPS, DT);
    auto t_mt = simulate_multithreaded(reg, STEPS, DT, threads);

    // kleiner Anti-Dead-Code-Check
    double checksum = 0.0;
    {
        const auto& alive = reg.alive();
        if (!alive.empty()) {
            size_t step = std::max<size_t>(1, alive.size() / 1000);
            for (size_t k = 0; k < alive.size() && k < step * 1000; k += step)
            {
                EntityID e = alive[k];
                const auto* p = reg.getComponent<Position>(e);
                const auto* h = reg.getComponent<Health>(e);
                checksum += p->x + p->y + h->hp;
            }
        }
    }

    std::cout << "Setup time:              " << t_init << " ms\n";
    std::cout << "Single-threaded:         " << t_st   << " ms for " << STEPS << " steps\n";
    std::cout << "Multi-threaded (" << threads << "): " << t_mt << " ms for " << STEPS << " steps\n";
    std::cout << "Checksum: " << checksum << "\n";
    return 0;
}
