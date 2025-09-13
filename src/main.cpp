#include "ecs.h"
#include <chrono>

// component (simple struct with constructor)

struct Name
{
    std::string name;

    Name(std::string name) : name(name) {}
};

int main()
{
    spire::ecs::Registry reg;

    // register the youre component, needs the wanted registry and the component struct

    reg.registerComponent<Name>();

    // create entitiy (simple uint32_t)

    for (int i = 0; i < spire::ecs::MAX_ENTITIES; ++i) 
    {
        spire::ecs::Entity e = reg.create();

        // add the wanted component to the entity 
        // reg.addComponent<component>(entity, constructor values...);

        reg.addComponent<Name>(e, "Tom");
    
    }

    // lambda to iterate over each entity with given components

    auto time1 = std::chrono::steady_clock::now();

    for (int i = 0; i < 1000; ++i)
    {

        //iterate through each entity with given components

        reg.view<Name>()->each([&reg](spire::ecs::Entity e, Name& n)
        {
            // get component ptr

            // std::cout << n->name << std::endl;
        });
    }

    auto time2 = std::chrono::steady_clock::now();

    auto time = time2 - time1; 

    std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(time).count() << '\n';

    // iterate over every entity

    for (auto e : reg.alive())
    {   
        // get if entity has wanted component

        if (reg.hasComponent<Name>(e))
        {
            Name* n = reg.getComponent<Name>(e);

            // std::cout << n->name << std::endl;

            // remove component from entity

            reg.removeComponent<Name>(e);
        }

        //delete entity
         
        reg.destroy(e);
    }

    // remove given component from every entity

    reg.clear<Name>();

    // destroy every entity

    reg.reset();
}