#include "Ecs.h"
#include <iostream>

int main()
{
    // component (simple struct with constructor)

    ECS_COMPONENT(Name)
    {
        std::string name;

        Name(std::string name) : name(name) {}
    };
    
    // create registry wich handles entities and components

    ecs::Registry reg;

    // register the youre component, needs the wanted registry and the component struct

    ECS_REGISTER(reg, Name);

    // create entitiy (simple uint32_t)

    ecs::Entity e = reg.create();

    // add the wanted component to the entity 
    // reg.addComponent<component>(entity, constructor values...);

    reg.addComponent<Name>(e, "Tom");

    // lambda to iterate over each entity with given components

    reg.each<Name>([&reg](ecs::Entity e)
    {
        // get component ptr

        Name* n = reg.getComponent<Name>(e);

        std::cout << n->name << std::endl;
    });

    // iterate over every entity

    for (auto e : reg.alive())
    {   
        // get if entity has wanted component

        if (reg.hasComponent<Name>(e))
        {
            Name* n = reg.getComponent<Name>(e);

            std::cout << n->name << std::endl;

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