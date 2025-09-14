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
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <tuple>
#include <csignal>
#include <cstdlib>

//---------------------------------------------
// Error handling
//---------------------------------------------

#if defined(_MSC_VER)
    #define DEBUG_BREAK() __debugbreak()
#elif defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
    #define DEBUG_BREAK() __builtin_debugtrap()
#elif defined(__has_builtin) && __has_builtin(__builtin_trap)
    #define DEBUG_BREAK() __builtin_trap()
#else
    #ifdef SIGTRAP
        #define DEBUG_BREAK() std::raise(SIGTRAP)
    #else
        #define DEBUG_BREAK() std::abort()
    #endif
#endif

// logging

#define LOG_INFO(msg) do    { LOG("Info",    msg, __FILE__, __LINE__); } while (0)
#define LOG_WARNING(msg) do { LOG("Warning", msg, __FILE__, __LINE__); } while (0)
#define LOG_ERROR(msg) do   { LOG("Error",   msg, __FILE__, __LINE__); } while (0)

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
    #define ASSERT(x)          (void)0
#endif

//---------------------------------------------
// Type definitions
//---------------------------------------------

using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8 =  std::uint8_t;

namespace spire::ecs
{
    using Index =       u32;
    using EntityID =    u32;
    using ComponentID = u32;
    using Signature =   u64;

    // constant values

    inline constexpr Index       INVALID_INDEX =  std::numeric_limits<Index>::max();
    inline constexpr EntityID    NONE =           std::numeric_limits<EntityID>::max();
    inline constexpr EntityID    MAX_ENTITIES =   1'000'000;
    inline constexpr ComponentID MAX_COMPONENTS = 64;

    static_assert(MAX_COMPONENTS <= 64, "MAX_COMPONENTS can't be more than signature allows!");

    //---------------------------------------------
    // Utility functions
    //---------------------------------------------

    inline std::atomic<ComponentID> nextComponentID{0};

    inline ComponentID nextComponent() noexcept
    {
        return nextComponentID.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename C>
    struct ComponentType
    {
        inline static const ComponentID id = nextComponent();
    };

    template <typename C>
    inline ComponentID getComponentID() noexcept
    {
        return ComponentType<C>::id;
    }

    template <typename... Cs>
    inline static Signature getSignature()
    {
        Signature s = 0;
        ((s |= (Signature{1} << getComponentID<Cs>())), ...);
        return s;
    }

    struct ViewKey 
    {
        Signature signature;
        std::vector<ComponentID> order;

        bool operator==(const ViewKey& other) const noexcept 
        {
            return signature == other.signature && order == other.order;
        }
    };

    struct ViewKeyHash 
    {
        size_t operator()(const ViewKey& key) const noexcept 
        {
            size_t hash = std::hash<u64>{}(key.signature);

            for (auto id : key.order) 
            {
                hash ^= std::hash<size_t>{}(id) + 0x9e3779b97f4a7c15ULL + 
                    (hash<<6) + (hash>>2);
            }
            return hash;
        }
    };

    //---------------------------------------------
    // Component pool
    //---------------------------------------------

    class Registry;
    class ComponentManager;

    class ComponentPoolBase
    {
    public:
        virtual ~ComponentPoolBase() = default;
        virtual void remove(EntityID e) = 0;
    };

    template <typename C>
    class ComponentPool : public ComponentPoolBase
    {
    private:
        friend class ComponentManager;
        friend class Registry;

        u64 m_version{0};

        static constexpr u16 PAGE_SIZE = 4096;
        static constexpr u32 MAX_PAGES = (MAX_ENTITIES + PAGE_SIZE - 1) / PAGE_SIZE;

        using Page = std::array<Index, PAGE_SIZE>;

        std::vector<C>               m_components;
        std::vector<EntityID>        m_entities;
        std::array<Page*, MAX_PAGES> m_indices{};

        static_assert((PAGE_SIZE & (PAGE_SIZE - 1)) == 0, "PAGE_SIZE must be power of two");

        ComponentPool() = default;

        ~ComponentPool()
        {
            for (auto& p : m_indices) delete p;
        }

        void add(EntityID e, C component)
        {
            const size_t page =  (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);

            if (!m_indices[page])
            {
                m_indices[page] = new Page;
                m_indices[page]->fill(INVALID_INDEX);
            }

            if ((*m_indices[page])[index] == INVALID_INDEX)
            {
                Index i = (Index)m_components.size();
                m_components.push_back(std::move(component));
                m_entities.push_back(e);
                (*m_indices[page])[index] = i;

                ++m_version;
            }
            else 
            {
                m_components[(*m_indices[page])[index]] = std::move(component);				
            }
        }

        void remove(EntityID e) override
        {
            const size_t page =  (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);		
            
            if (!m_indices[page] || (*m_indices[page])[index] == INVALID_INDEX) return;
            
            ASSERT(!m_components.empty());

            Index i = (Index)m_components.size() - 1;

            if ((*m_indices[page])[index] != i)
            {
                EntityID moved = m_entities[i];
                m_components[(*m_indices[page])[index]] = std::move(m_components[i]);
                m_entities[(*m_indices[page])[index]] = moved;

                const size_t movedPage =  (size_t)moved / PAGE_SIZE;
                const size_t movedIndex = (size_t)moved & (PAGE_SIZE - 1);

                (*m_indices[movedPage])[movedIndex] = (*m_indices[page])[index];
            }

            m_components.pop_back();
            m_entities.pop_back();
            (*m_indices[page])[index] = INVALID_INDEX;

            ++m_version;
        }

        void clear() noexcept 
        {
            for (EntityID e : m_entities) 
            {
                const size_t page =  (size_t)e / PAGE_SIZE;
                const size_t index = (size_t)e & (PAGE_SIZE - 1);
                if (m_indices[page]) (*m_indices[page])[index] = INVALID_INDEX;
            }

            m_components.clear();
            m_entities.clear();

            ++m_version;
        }

        C* get(EntityID e) noexcept
        {
            const size_t page =  (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);	

            Page* p = m_indices[page];
            if (!p) return nullptr;

            auto i = (*p)[index];
            if (i == INVALID_INDEX) return nullptr;

            return &m_components[i];
        }

        const C* get(EntityID e) const noexcept
        {
            const size_t page =  (size_t)e / PAGE_SIZE;
            const size_t index = (size_t)e & (PAGE_SIZE - 1);	

            Page* p = m_indices[page];
            if (!p) return nullptr;

            auto i = (*p)[index];
            if (i == INVALID_INDEX) return nullptr;

            return &m_components[i];
        }

        u64 version() const noexcept {return m_version;}
        const std::vector<C>& components() const noexcept { return m_components; }
        const std::vector<EntityID>& entities() const noexcept { return  m_entities; }
    };

    //---------------------------------------------
    // ComponentManager
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
            const ComponentID i = getComponentID<C>();

            if (i >= MAX_COMPONENTS)
            {   
                LOG_ERROR("Component limit reached!");
                std::terminate();
            }

            if (i >= m_pools.size()) m_pools.resize(i + 1, nullptr);
            if (!m_pools[i]) m_pools[i] = new ComponentPool<C>();
        }

        template <typename C>
        void add(EntityID e, C component)
        {
            pool<C>().add(e, std::move(component));
        }

        template <typename C>
        void remove(EntityID e)
        {
            pool<C>().remove(e);
        }

        void destroy(EntityID e)
        {
            for (auto& pool : m_pools) 
            {
                if (pool) pool->remove(e);               
            }
        }

        template <typename C>
        void clear() {pool<C>().clear();}

        template <typename C>
        C* get(EntityID e) noexcept
        {
            return pool<C>().get(e);
        }

        template <typename C>
        const C* get(EntityID e) const noexcept
        {
            return pool<C>().get(e);
        }

        template <typename C> 
        ComponentPool<C>& pool()
        {
            const ComponentID i = getComponentID<C>();

            if (i >= m_pools.size() || !m_pools[i])
            {
                LOG_ERROR("Component not found, use registerComponent<C>() first!");
                std::terminate();
            }

            return *static_cast<ComponentPool<C>*>(m_pools[i]);
        }

        template <typename C> 
        const ComponentPool<C>& pool() const
        {
            const ComponentID i = getComponentID<C>();

            if (i >= m_pools.size() || !m_pools[i])
            {
                LOG_ERROR("Component not found, use registerComponent<C>() first!");
                std::terminate();
            }

            return *static_cast<const ComponentPool<C>*>(m_pools[i]);
        }

        template <typename C> 
        const std::vector<C>& components() const noexcept { return pool<C>().components(); }

        template <typename C> 
        const std::vector<EntityID>& entities() const noexcept { return pool<C>().entities(); }

        template <typename C> 
        const u64 version() const noexcept { return pool<C>().version(); }
    };

    //---------------------------------------------
    // Entity wrapper
    //--------------------------------------------- 

    class Entity
    {
    private:
        EntityID  m_id;
        Registry* m_reg;

    public:
        Entity(EntityID id, Registry* reg) : m_id(id), m_reg(reg) {}
        ~Entity() = default;

        template<typename C, typename... Args>
        void add(Args&&... args);

        template<typename C>
        void remove();

        void destroy();

        template<typename C>
        [[nodiscard]] C* get() noexcept;

        template<typename C>
        [[nodiscard]] const C* get() const noexcept;

        [[nodiscard]] bool valid() const noexcept;

        EntityID id() const;
    };

    //---------------------------------------------
    // View
    //---------------------------------------------

    class ViewBase
    {
    public:
        virtual ~ViewBase() = default;
    };

    template <typename... Cs>
    class View : public ViewBase
    {
    private:
        std::array<u64, sizeof...(Cs)> m_versions{};
        std::vector<std::tuple<EntityID, Cs*...>> m_cache{};

        Signature m_signature;
        Registry* m_reg;

        void update(); 

    public:
        View(Registry* reg);
        ~View() = default;

        template <typename Fn>
        void each(Fn&& fn);

        std::vector<EntityID> entities();
    };

    //---------------------------------------------
    // Registry
    //---------------------------------------------

    class Registry
    {
    private:
        ComponentManager m_componentManager;

        std::queue<EntityID>  m_available{};
        std::vector<EntityID> m_destroy;

        std::vector<EntityID>  m_alive;
        std::vector<Signature> m_signatures;
        std::vector<Index>     m_indices;

        std::unordered_map<ViewKey, ViewBase*, ViewKeyHash> m_views;

    public:
        Registry() : m_indices(MAX_ENTITIES, INVALID_INDEX), m_signatures(MAX_ENTITIES, 0)
        {
            for (EntityID e = 0; e < MAX_ENTITIES; e++) m_available.push(e);
        }

        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;

        ~Registry()
        {
            for (auto& [key, view] : m_views) delete view;
        }

        void update()
        {
            for (auto e : m_destroy)
            {
                if (m_indices[e] == INVALID_INDEX) continue;

                Index i = (Index)m_alive.size() - 1;
                EntityID moved = m_alive[i];
                m_alive[m_indices[e]] = moved;
                m_indices[moved] = m_indices[e];

                m_alive.pop_back();
                m_indices[e] = INVALID_INDEX;
                m_signatures[e] = 0;

                m_componentManager.destroy(e);
                m_available.push(e);
            }
            m_destroy.clear();			
        }

        template<typename C>
        void registerComponent()
        {
            m_componentManager.registerComponent<C>();
        }

        [[nodiscard]] Entity create()
        {
            if (m_available.empty()) 
            {
                LOG_WARNING("Entity limit reached!");
                return Entity(NONE, this);
            }

            EntityID e = m_available.front();
            m_available.pop();

            m_indices[e] = (Index)m_alive.size();
            m_alive.push_back(e);

            return Entity(e, this);
        }

        [[nodiscard]] Entity getEntity(EntityID e)
        {
            return Entity(e, this);
        }

        void reset()
        {
            for (auto e : m_alive) destroy(e);	
            update();
        }

        template<typename C, typename... Args>
        void addComponent(EntityID e, Args&&... args)
        {
            if (!valid(e)) return;
            C component = C(std::forward<Args>(args)...);
            m_componentManager.add<C>(e, component);

            m_signatures[e] |= (Signature{1} << getComponentID<C>());
        }

        template<typename C>
        void removeComponent(EntityID e)
        {	
            if (!valid(e)) return;
            m_componentManager.remove<C>(e);

            m_signatures[e] &= ~(Signature{1} << getComponentID<C>());
        }

        void destroy(EntityID e)
        {
            if (!valid(e)) return;
            m_destroy.push_back(e);
        }

        template<typename C>
        void clear() {m_componentManager.clear<C>();}

        template<typename C>
        [[nodiscard]] C* getComponent(EntityID e) noexcept
        {
            if (!valid(e)) return nullptr;
            return m_componentManager.get<C>(e);
        }

        template<typename C>
        [[nodiscard]] const C* getComponent(EntityID e) const noexcept
        {
            if (!valid(e)) return nullptr;
            return m_componentManager.get<C>(e);
        }

        [[nodiscard]] bool valid(EntityID e) const noexcept
        {
            if (e >= MAX_ENTITIES) 
            {
                LOG_WARNING("Entity not valid!");
                return false;
            }

            return m_indices[e] != INVALID_INDEX; 
        }

        template <typename... Cs>
        View<Cs...>* view()
        {
            Signature signature = getSignature<Cs...>();
            ViewKey key {signature, std::vector<ComponentID> {getComponentID<Cs>()...}};

            if (auto it = m_views.find(key); it != m_views.end()) 
            {
                return static_cast<View<Cs...>*>(it->second);
            }

            auto view = new View<Cs...>(this);
            m_views.emplace(key, std::move(view));
            return view;
        } 
        
        template <typename C> 
        const std::vector<EntityID>& entities() const {return m_componentManager.entities<C>();}

        const std::vector<Signature>& signatures() const noexcept {return m_signatures;}
        
        const std::vector<EntityID>& alive() const { return m_alive; }

        template <typename C> 
        u64 version() const {return m_componentManager.version<C>();}

    };

    // entity definitions

    template<typename C, typename... Args>
    inline void Entity::add(Args&&... args)
    {
        m_reg->addComponent<C>(m_id, std::forward<Args>(args)...);
    }

    template<typename C>
    inline void Entity::remove()
    {
        m_reg->removeComponent<C>(m_id);
    }

    inline void Entity::destroy()
    {
        m_reg->destroy(m_id);
    }

    template<typename C>
    [[nodiscard]] inline C* Entity::get() noexcept
    {
        return m_reg->getComponent<C>(m_id);
    }

    template<typename C>
    [[nodiscard]] inline const C* Entity::get() const noexcept
    {
        return m_reg->getComponent<C>(m_id);
    }

    [[nodiscard]] inline bool Entity::valid() const noexcept
    {
        return m_reg->valid(m_id);
    }

    inline EntityID Entity::id() const {return m_id;}

    // view definitions

    template <typename... Cs>
    inline void View<Cs...>::update()
    {
        std::array<u64, sizeof...(Cs)> versions {
            m_reg->template version<Cs>()...
        };

        if (m_versions == versions) return;
        m_versions = versions;

        std::array<const std::vector<EntityID>*, sizeof...(Cs)> entitiesArr {
            &m_reg->template entities<Cs>()...
        };

        const std::vector<EntityID>* smallest = entitiesArr[0];
        for (const auto* entities : entitiesArr)
        {
            if (smallest->size() > entities->size()) smallest = entities;
        } 

        m_cache.clear();
        m_cache.reserve(smallest->size());

        const auto& signatures = m_reg->signatures();

        for (auto e : *smallest)
        {	
            if ((signatures[e] & m_signature) != m_signature) continue;
            m_cache.emplace_back(e, m_reg->getComponent<Cs>(e)...);
        }
    } 
    
    template <typename... Cs>
    inline View<Cs...>::View(Registry* reg) 
        : m_reg(reg)
        , m_signature(getSignature<Cs...>()) 
    {
        for (auto& version : m_versions)
        {
            version = std::numeric_limits<u64>::max();
        }
    }

    template <typename... Cs>
    template <typename Fn>
    inline void View<Cs...>::each(Fn&& fn)
    {
        update();

        for (auto& tuple : m_cache)
        {
            std::apply([&](EntityID e, auto*... components){
                std::invoke(std::forward<Fn>(fn), e, (*components)...);
            }, tuple);
        }
    }

    template <typename... Cs>
    inline std::vector<EntityID> View<Cs...>::entities()
    {
        update();

        std::vector<EntityID> entities;
        entities.reserve(m_cache.size());

        for (auto& tuple : m_cache)
        {
            entities.push_back(std::get<0>(tuple));
        }

        return entities;
    }
}