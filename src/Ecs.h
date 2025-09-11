#pragma once

//---------------------------------------------
// Includes
//---------------------------------------------

#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

//---------------------------------------------
// Error handling
//---------------------------------------------

#if defined(_MSC_VER)
    #define DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
    #include <csignal>
    #define DEBUG_BREAK() raise(SIGTRAP)
#else
    #define DEBUG_BREAK() ((void)0)
#endif

// logging

#define LOG_INFO(msg)    do { LOG("Info",    msg, __FILE__, __LINE__); } while (0)
#define LOG_WARNING(msg) do { LOG("Warning", msg, __FILE__, __LINE__); } while (0)
#define LOG_ERROR(msg)   do { LOG("Error",   msg, __FILE__, __LINE__); DEBUG_BREAK(); } while (0)

inline void LOG(const std::string& level, const std::string& msg, const char* file, const int line)
{
    std::cout << level << ": " << "File: " << file << "\nLine: " << line << "\n" << msg << "\n";
}

// assertions

#ifndef NDEBUG
    #define ASSERT_MSG(x, msg) do { \
        if (!(x)) { \
            LOG_ERROR(std::string("assertion failed: ") + std::string(msg)); \
            DEBUG_BREAK(); } \
        } while (0) 

    #define ASSERT(x) do { \
        if (!(x)) { \
            LOG_ERROR("assertion failed!");  \
            DEBUG_BREAK(); } \
        } while (0) 
#else
    #define ASSERT_MSG(x, msg) (void)0
    #define ASSERT(x) (void)0
#endif

// define common types

using u32 = unsigned int;
using u16 = unsigned short int;
using u8  = unsigned char;

namespace spire::ecs
{
    class ComponentManager;
    class Registry;

    //---------------------------------------------
    // Define entity and constant values
    //---------------------------------------------

    using Index = u32;
    using Entity = u32;
    using Component = size_t;

    inline constexpr Index INVALID_INDEX = std::numeric_limits<Index>::max();
    inline constexpr Entity NONE = std::numeric_limits<Entity>::max();
    inline constexpr Entity MAX_ENTITIES = 1'000'000;

    //---------------------------------------------
    // ComponentIDs
    //---------------------------------------------

    inline std::atomic_size_t next{0};

    Component nextComponentID()
    {
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename C>
    struct ComponentID
    {
        inline static const Component id = nextComponentID();
    };

    template <typename C>
    inline Component getComponentID() noexcept
    {
        return ComponentID<C>::id;
    }

    //---------------------------------------------
    // Component pool
    //---------------------------------------------

    class ComponentPoolBase
    {
    public:
        virtual ~ComponentPoolBase() = default;
        virtual void remove(Entity e) = 0;
    };

    template <typename C>
    class ComponentPool : public ComponentPoolBase
    {
    private:
        friend class ComponentManager;
        friend class Registry;
        
        static constexpr u16 CHUNK_SIZE = 16384;
        static constexpr u16 PAGE_SIZE = 4096;
        static constexpr u32 MAX_PAGES = (MAX_ENTITIES + PAGE_SIZE - 1) / PAGE_SIZE;

        using Page = std::array<Index, PAGE_SIZE>;

        std::vector<C> m_components;
        std::vector<Entity> m_entities;
        Page* m_indices[MAX_PAGES]{};
    
        ComponentPool()
        {
            if ((PAGE_SIZE & (PAGE_SIZE - 1)) != 0)
            {
                LOG_ERROR("PAGE_SIZE must be power of two!");
                std::terminate();
            }
        }

        ~ComponentPool()
        {
            for (auto& p : m_indices) delete p;
        }

        void reserveChunk()
        {
            auto newCap = m_components.size() + CHUNK_SIZE;
            if (newCap <= m_components.capacity() || newCap + CHUNK_SIZE > (size_t)MAX_ENTITIES) return;
            m_components.reserve(m_components.capacity() + CHUNK_SIZE);
            m_entities.reserve(m_components.capacity() + CHUNK_SIZE);
        }

        void add(Entity e, C component)
        {
            reserveChunk();

            const size_t page = (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);

            if (!m_indices[page])
            {
                m_indices[page] = new Page;
                m_indices[page]->fill(INVALID_INDEX);
            }

            if ((*m_indices[page])[index] == INVALID_INDEX)
            {
                Index i = m_components.size();
                m_components.push_back(std::move(component));
                m_entities.push_back(e);
                (*m_indices[page])[index] = i;
            }
            else 
            {
                m_components[(*m_indices[page])[index]] = std::move(component);				
            }
        }

        void remove(Entity e) override
        {
            const size_t page = (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);		
            
            if (!m_indices[page] || (*m_indices[page])[index] == INVALID_INDEX) return;
            
            Index i = m_components.size() - 1;

            if ((*m_indices[page])[index] != i)
            {
                Entity moved = m_entities[i];
                m_components[(*m_indices[page])[index]] = std::move(m_components[i]);
                m_entities[(*m_indices[page])[index]] = moved;

                const size_t movedPage = (size_t)moved / PAGE_SIZE;
                const size_t movedIndex = (size_t)moved & (PAGE_SIZE - 1);

                (*m_indices[movedPage])[movedIndex] = (*m_indices[page])[index];
            }

            m_components.pop_back();
            m_entities.pop_back();
            (*m_indices[page])[index] = INVALID_INDEX;
        }

        C* get(Entity e)
        {
            const size_t page = (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);	

            if (!m_indices[page]) return nullptr;
            if ((*m_indices[page])[index] == INVALID_INDEX) return nullptr;

            return &m_components[(*m_indices[page])[index]];
        }

        const C* get(Entity e) const
        {
            const size_t page = (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);	

            if (!m_indices[page]) return nullptr;
            if ((*m_indices[page])[index] == INVALID_INDEX) return nullptr;

            return &m_components[(*m_indices[page])[index]];
        }

        const std::vector<C> &components() const { return m_components; }
        const std::vector<Entity> &entities() const { return  m_entities; }
    };

    //---------------------------------------------
    // ComponentManager manages all component pools
    //---------------------------------------------

    class ComponentManager
    {
    private:
        friend class Registry;

        std::vector<ComponentPoolBase*> m_pools;

        ComponentManager() = default;

        ~ComponentManager()
        {
            for (auto& pool : m_pools) delete pool;
        }
        
        template <typename C>
        void registerComponent()
        {
            const Component i = getComponentID<C>();
            if (i >= m_pools.size()) m_pools.resize(i + 1, nullptr);
            if (!m_pools[i]) m_pools[i] = new ComponentPool<C>();
        }

        template <typename C>
        void add(Entity e, C component)
        {
            pool<C>().add(e, std::move(component));
        }

        template <typename C>
        void remove(Entity e)
        {
            pool<C>().remove(e);
        }

        void destroy(Entity e)
        {
            for (auto& pool : m_pools) 
            {
                if (pool) pool->remove(e);               
            }
        }

        template <typename C>
        C *get(Entity e)
        {
            return pool<C>().get(e);
        }

        template <typename C>
        const C *get(Entity e) const
        {
            return pool<C>().get(e);
        }
        
        template <typename C> 
        const std::vector<C> &components() const { return pool<C>().components(); }

        template <typename C> 
        const std::vector<Entity> &entities() const { return pool<C>().entities(); }

        template <typename C> 
        ComponentPool<C>& pool()
        {
            const Component i = getComponentID<C>();

            auto* pool = static_cast<ComponentPool<C>*>(m_pools[i]);
            if (!pool)
            {
                LOG_ERROR("Component not found, use registerComponent<C>() first!");
                std::terminate();
            }

            return *pool;
        }

        template <typename C> 
        const ComponentPool<C>& pool() const
        {
            const Component i = getComponentID<C>();

            auto* pool = static_cast<const ComponentPool<C>*>(m_pools[i]);
            if (!pool)
            {
                LOG_ERROR("Component not found, use registerComponent<C>() first!");
                std::terminate();
            }

            return *pool;
        }
    };

    //---------------------------------------------
    // Registry	 manages entities and its components
    //---------------------------------------------

    class Registry
    {
    private:
        ComponentManager m_componentManager;

        std::queue<Entity> m_available{};
        std::vector<Entity> m_destroy;

        std::vector<Entity> m_alive;
        std::vector<Index> m_indices;

    public:
        Registry() : m_indices(MAX_ENTITIES, INVALID_INDEX)
        {
            for (Entity e = 0; e < MAX_ENTITIES; e++) m_available.push(e);
        }

        ~Registry() = default;

        void update()
        {
            for (auto e : m_destroy)
            {
                if (m_indices[e] == INVALID_INDEX) continue;

                Index i = m_alive.size() - 1;
                Entity moved = m_alive[i];
                m_alive[m_indices[e]] = moved;
                m_indices[moved] = m_indices[e];

                m_alive.pop_back();
                m_indices[e] = INVALID_INDEX;

                m_componentManager.destroy(e);
                m_available.push(e);
            }
            m_destroy.clear();			
        }

        // manage component pools

        template<typename C>
        void registerComponent()
        {
            m_componentManager.registerComponent<C>();
        }

        Entity create()
        {
            if (m_available.empty()) 
            {
                LOG_WARNING("Entity limit reached!");
                return NONE;
            }

            Entity e = m_available.front();
            m_available.pop();

            m_indices[e] = m_alive.size();
            m_alive.push_back(e);

            return e;
        }

        template<typename C>
        void clear()
        {
            const auto entities = m_componentManager.entities<C>();
            for (auto e : entities) m_componentManager.remove<C>(e);
        }

        // reset registry

        void reset()
        {
            for (auto e : m_alive) destroy(e);	
            update();
        }

        // manage entities and its components

        template<typename C, typename... Args>
        void addComponent(Entity e, Args&&... args)
        {
            if (!valid(e)) return;
            C component = C(std::forward<Args>(args)...);
            m_componentManager.add<C>(e, component);
        }

        template<typename C>
        void removeComponent(Entity e)
        {	
            if (!valid(e)) return;
            m_componentManager.remove<C>(e);
        }

        void destroy(Entity e)
        {
            if (!valid(e)) return;
            m_destroy.push_back(e);
        }

        template<typename C>
        C *getComponent(Entity e)
        {
            if (!valid(e)) return nullptr;
            return m_componentManager.get<C>(e);
        }

        template<typename C>
        const C *getComponent(Entity e) const
        {
            if (!valid(e)) return nullptr;
            return m_componentManager.get<C>(e);
        }
        
        template<typename C>
        bool hasComponent(Entity e) const
        {
            if (!valid(e)) return false;
            return m_componentManager.get<C>(e) != nullptr;
        }

        bool valid(Entity e) const
        {
            if (e >= MAX_ENTITIES) 
            {
                LOG_WARNING("Entity not available!");
                return false;
            }

            if (!isAlive(e)) return false;
            return true;
        }
        
        bool isAlive(Entity e) const { return m_indices[e] != INVALID_INDEX; }

        // get all alive entities

        const std::vector<Entity>& alive() const { return m_alive; }

        // iterate through entities with set components

        template<typename... Cs, typename Fn>
        void each(Fn&& fn)
        {
            if(!(sizeof...(Cs) > 0))
            {
                LOG_ERROR("Fuction needs at least one component!");
                std::terminate();
            }

            std::array<const std::vector<Entity>*, sizeof...(Cs)> lists{
                &m_componentManager.pool<Cs>().entities()...
            };

            const std::vector<Entity>* smallest = lists[0];
            for (const auto* entities : lists)
            {
                if (smallest->size() > entities->size()) smallest = entities;
            } 

            for (Entity e : *smallest)
            {	
                bool hasComponents = ((m_componentManager.pool<Cs>().get(e) != nullptr) && ...);
                if (hasComponents) fn(e);
            }
        }
    };
}