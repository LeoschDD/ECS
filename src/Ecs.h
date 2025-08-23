#pragma once

#include <cstdint>
#include <vector>
#include <queue>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <stdexcept>
#include <limits>
#include <utility>
#include <execution>
#include <array>
#include <algorithm>

#define ECS_REGISTER(reg, component) reg.registerComponent<component>() 
#define ECS_COMPONENT(name) struct name

namespace ecs
{
	class ComponentManager;
	class Registry;

	//---------------------------------------------
	// Define entity and constant values
	//---------------------------------------------

	using Index = uint32_t;
	using Entity = uint32_t;

	inline constexpr Index INVALID_INDEX = std::numeric_limits<Index>::max();
	inline constexpr Entity NONE = std::numeric_limits<Entity>::max();
	inline constexpr Entity MAX_ENTITIES = 500000;

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
		
		static constexpr uint16_t CHUNK_SIZE = 16384;
		static constexpr uint16_t PAGE_SIZE = 4096; // page size must be power of 2
		static constexpr uint32_t MAX_PAGES = (MAX_ENTITIES + PAGE_SIZE - 1) / PAGE_SIZE;

		using Page = std::array<Index, PAGE_SIZE>;

		std::vector<C> m_components;
		std::vector<Entity> m_entities;
		std::array<Page*, MAX_PAGES> m_indices{};
	
		ComponentPool() = default;

		~ComponentPool()
		{
			for (auto& p : m_indices) delete p;
		}

		void reserveChunk()
		{
            auto newCap = m_components.size() + CHUNK_SIZE;
			if (newCap <= m_components.capacity() || newCap + CHUNK_SIZE > static_cast<size_t>(MAX_ENTITIES)) return;
			m_components.reserve(m_components.capacity() + CHUNK_SIZE);
			m_entities.reserve(m_components.capacity() + CHUNK_SIZE);
		}

		void add(Entity e, C component)
		{
			reserveChunk();

			const size_t page = static_cast<size_t>(e) / PAGE_SIZE;
			const size_t index = static_cast<size_t>(e) & (PAGE_SIZE - 1);

			if (!m_indices[page])
			{
				m_indices[page] = new Page;
				m_indices[page]->fill(INVALID_INDEX);
			}

			if ((*m_indices[page])[index] == INVALID_INDEX)
			{
				Index i = m_components.size();
				m_components.emplace_back(std::move(component));
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
			const size_t page = static_cast<size_t>(e) / PAGE_SIZE;
			const size_t index = static_cast<size_t>(e) & (PAGE_SIZE - 1);		
			
			if (!m_indices[page] || (*m_indices[page])[index] == INVALID_INDEX) return;
			
			Index i = m_components.size() - 1;

			if ((*m_indices[page])[index] != i)
			{
				Entity moved = m_entities[i];
				m_components[(*m_indices[page])[index]] = std::move(m_components[i]);
				m_entities[(*m_indices[page])[index]] = moved;

				const size_t movedPage = static_cast<size_t>(moved) / PAGE_SIZE;
				const size_t movedIndex = static_cast<size_t>(moved) & (PAGE_SIZE - 1);

				(*m_indices[movedPage])[movedIndex] = (*m_indices[page])[index];
			}

			m_components.pop_back();
			m_entities.pop_back();
			(*m_indices[page])[index] = INVALID_INDEX;
		}

		C *get(Entity e)
		{
			const size_t page = static_cast<size_t>(e) / PAGE_SIZE;
			const size_t index = static_cast<size_t>(e) & (PAGE_SIZE - 1);	

			if (!m_indices[page]) return nullptr;
			if ((*m_indices[page])[index] == INVALID_INDEX) return nullptr;

			return &m_components[(*m_indices[page])[index]];
		}

		const C *get(Entity e) const
		{
			const size_t page = static_cast<size_t>(e) / PAGE_SIZE;
			const size_t index = static_cast<size_t>(e) & (PAGE_SIZE - 1);	

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

		std::unordered_map<std::type_index, ComponentPoolBase*> m_pools;

		ComponentManager() = default;

		~ComponentManager()
		{
			for (auto& [key, pool] : m_pools) delete pool;
		}
		
		template <typename C>
		void registerComponent()
		{
			auto key = std::type_index(typeid(C));
			if (m_pools.count(key)) return;
			m_pools[key] = new ComponentPool<C>();
		}

		template <typename C>
		void add(Entity e, C component)
		{
			pool<C>().add(e, component);
		}

		template <typename C>
		void remove(Entity e)
		{
			pool<C>().remove(e);
		}

		void destroy(Entity e)
		{
			for (auto& [type, pool] : m_pools) pool->remove(e);
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
			const auto i = std::type_index(typeid(C));

			auto it = m_pools.find(i);
			if (it == m_pools.end()) throw std::runtime_error("Error: Component not found!");

			return *static_cast<ComponentPool<C>*>(it->second);
		}

		template <typename C> 
		const ComponentPool<C>& pool() const
		{
			const auto i = std::type_index(typeid(C));

			auto it = m_pools.find(i);
			if (it == m_pools.end()) throw std::runtime_error("Error: Component not found!");

			return *static_cast<const ComponentPool<C>*>(it->second);
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
			if (m_available.empty()) throw std::runtime_error("Error: Entity limit reached!");

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
			if (e >= MAX_ENTITIES) throw std::runtime_error("Error: Entity not available!");
			if (!isAlive(e)) return false;
			return true;
		}

		bool isAlive(Entity e) const { return m_indices[e] != INVALID_INDEX; }

		// get all alive entities

		const std::vector<Entity>& alive() const { return m_alive; }

		// iterate through entities with set components -> first component should be the least used one for performance

		template<typename C0, typename... Cs, typename Fn>
		void each(Fn&& fn)
		{
			const auto entities = m_componentManager.entities<C0>();
			auto pools = std::tuple<const ComponentPool<Cs>*...>{ &m_componentManager.pool<Cs>()... };

			for (Entity e : entities)
			{	
				bool hasComponent = true;
				
				std::apply([&](auto... p){hasComponent = ((p->get(e) != nullptr) && ...);}, pools);

				if (hasComponent) fn(e);
			}
		}

		// multithreaded -> read only 

		template<typename C0, typename... Cs, typename Fn>
		void parallelEach(Fn&& fn)
		{
			const auto entities = m_componentManager.entities<C0>();
			auto pools = std::tuple<const ComponentPool<Cs>*...>{ &m_componentManager.pool<Cs>()... };

			std::for_each(std::execution::par, entities.begin(), entities.end(), [&](Entity e)
            {
				bool hasComponent = true;
				
				std::apply([&](auto... p){hasComponent = ((p->get(e) != nullptr) && ...);}, pools);

				if (hasComponent) fn(e);
            });	
		}
	};
}