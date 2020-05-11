//
// Copyright 2020 - present Isotropix SAS. See License.txt for license information
//

#include <module_group.h>

#include <of_app.h>
#include <of_object.h>
#include <of_object_factory.h>
#include <module_scene_object.h>
#include <module_scene_object_tree.h>
#include <module_geometry.h>
#include <geometry_object.h>
#include <app_object.h>

#include "r2c_instancer.h"
#include "r2c_render_delegate.h"

#include "r2c_scene_delegate.h"


// some general helpers

// used to cache OfClasses supported by the render delegate
void
populate_supported_classes(CoreVector<const OfClass *>& supported_classes, const R2cRenderDelegate& delegate, const CoreBasicArray<CoreString>& class_names, OfApp& application)
{
    supported_classes.remove_all();
    for (auto class_name : class_names) {
        OfClass *cclass = application.get_factory().get_classes().exists(class_name);
        if (cclass != nullptr) {
            supported_classes.add(cclass);
        } else { // if it doesn't exist then it's probably a typo. Let's warn about it
            LOG_WARNING("R2cSceneDelegate: The speficied class '" << class_name << "' defined by the render delegate '" << delegate.get_class_info_name() << "' is not supported\n");
        }
    }
}

// used to return if speficied item class is supported by the render delegate
bool
is_class_supported(OfObject *item, const CoreVector<const OfClass *>& supported_classes)
{
    if (item != nullptr) {
        for (auto cls : supported_classes) if (item->is_kindof(cls->get_name())) return true;
    }
    return false;
}

// used to resolve the dependencies of instancers and add them to the dependency index
const CoreBasicArray<OfObject *>&
resolve_instancer_prototypes(const ModuleSceneObjectTree& scene_object_tree, InstancerPrototypesMap& all_dependencies)
{
    // check if we already computed the dependencies of the current scene object tree
    OfObject *item = scene_object_tree.get_object();
    CoreArray<OfObject *> **item_dependencies = all_dependencies.is_key_exists(item);
    if (item_dependencies == nullptr) {
        // we didn't so let's gather them
        CoreSet<OfObject *> current_deps;

        for (unsigned int i = 0; i < scene_object_tree.get_base_objects().get_count(); i++) {
            const ModuleSceneObject *sobj = scene_object_tree.get_base_objects()[i];
            if (sobj->is_kindof(ModuleSceneObjectTree::class_info())) { // if it's a scene object tree then we need to gather its deps
                const CoreBasicArray<OfObject *>& deps = resolve_instancer_prototypes(*static_cast<const ModuleSceneObjectTree *>(sobj), all_dependencies);
                for (unsigned int j = 0; j < deps.get_count(); j++) {
                    current_deps.add(deps[j]);
                }
            } else { // add deps
                current_deps.add(sobj->get_object()); // make sure we don't add duplicates
            }
        }
        // we've gathered all dependencies
        CoreArray<OfObject *> *entry_deps = new CoreArray<OfObject *>(current_deps.get_count());
        for (unsigned int i = 0; i < current_deps.get_count(); i++) {
            (*entry_deps)[i] = current_deps[i];
        }
        all_dependencies.add(item, entry_deps);
        return *entry_deps;
    } else {
        return **item_dependencies;
    }
}

IMPLEMENT_CLASS(R2cSceneDelegate, EventObject)

R2cSceneDelegate::R2cSceneDelegate() : EventObject(),
                                                 m_render_delegate(nullptr),
                                                 m_camera(nullptr),
                                                 m_geometries(nullptr),
                                                 m_lights(nullptr),
                                                 m_shading_layer(nullptr)
{
}

R2cSceneDelegate::~R2cSceneDelegate()
{
    disconnect_all();
    clear();
}


/*! \brief Helper to set class members in a generic way to avoid copying/pasting.
    \param m_input pointer to the class member
    \param new_input pointer to the new input
    \param class_name defines the expected OfClass name of the target input eg: Camera, Group...
    \note When new_input doesn't match the expected class_name m_input is set to nullptr */
void
R2cSceneDelegate::set_input(OfObject **m_input, OfObject *new_input, const CoreString& class_name)
{
    if (*m_input != new_input) {
        if (*m_input != nullptr) {
            disconnect_all(**m_input);
            *m_input = nullptr;
            // make sure to dirty our index
            if (*m_input == m_geometries) {
                dirty_geometry_index();
            } else if (*m_input == m_lights) {
                dirty_light_index();
            }
        }

        if (new_input != nullptr && new_input->is_kindof(class_name)) {
            *m_input = new_input;
            connect(*new_input, EVT_ID_DESTROY, EVENT_INFO_METHOD(R2cSceneDelegate::on_input_destroyed), m_input);
            if (m_render_delegate != nullptr) {
                // no need to connect if we don't have a render delegate attached
                if (*m_input == m_geometries) {
                    connect(*new_input->get_module(), EVT_ID_MODULE_GROUP_VISIBILITY, EVENT_INFO_METHOD(R2cSceneDelegate::on_geometries_group_update));
                } else if (*m_input == m_lights) {
                    connect(*new_input->get_module(), EVT_ID_MODULE_GROUP_VISIBILITY, EVENT_INFO_METHOD(R2cSceneDelegate::on_lights_group_update));
                }
            }
        }
    }
}

/*! \brief Track the destruction of the input to automatically set the class_member to nullptr instead of a dangling pointer.*/
void
R2cSceneDelegate::on_input_destroyed(EventObject& sender, const EventInfo& evtid, void *data)
{
    OfObject **m_class_member = static_cast<OfObject **>(data);
    // Note that we don't need to disconnect since we are automatically
    // disconnected from destroy event objects. We just need to mark
    // our corresponding index as dirty
    if (*m_class_member == m_geometries) {
        dirty_geometry_index();
    } else if (*m_class_member == m_lights) {
        dirty_light_index();
    }
    // set the pointer to null so we don't have a dangling pointer
    *m_class_member = nullptr;
}

/*! \brief Track the dirtiness of the input geometries group to update visibility.*/
void
R2cSceneDelegate::on_geometries_group_update(EventObject& sender, const EventInfo& evtid, void *data)
{
    // should resync geometries
    dirty_geometry_index();
}

/*! \brief Track the dirtiness of the input lights group to update visibility.*/
void
R2cSceneDelegate::on_lights_group_update(EventObject& sender, const EventInfo& evtid, void *data)
{
    // should resync lights
    dirty_light_index();
}

/*! \brief Cleanup things when item is destroyed. */
void
R2cSceneDelegate::on_dependency_destroyed(EventObject& sender, const EventInfo& evtid, void *data)
{
    // force disconnection
    disconnect_all(sender);

    OfObjectIndex& visibility_map = *static_cast<OfObjectIndex *>(data);
    // check if the destroyed item was in one of our visibility maps
    bool *value = visibility_map.is_key_exists(static_cast<OfObject *>(&sender));
    // in that case we set it to destroyed state
    if (value != nullptr) *value = false;
    // it was necessarily in our render items index if not that's a bug
    R2cItemDescriptor& item = m_render_item_dependencies.get_value(&sender);
    item.set_refcount(-1); // mark as destroyed
    if (&visibility_map == &m_geometry_index) { // that was a geometry
        remove_geometry(&sender);
    } else { // that was a light
        remove_light(&sender);
    }
}

/*! \brief converting Clarisse dirtiness to scene object (geometry or instancer) dirtiness for the render delegate */
void
R2cSceneDelegate::dispatch_scene_object_dirtiness(EventObject& sender, const EventInfo& evtid, void *data)
{
    OfObject *item = static_cast<OfObject *>(&sender);
    const OfAttr *attr = item->get_changing_attr();
    R2cItemDescriptor descriptor = get_item_descriptor(item);
    int transmitted_dirtiness = DIRTINESS_NONE;

    // converting Clarisse dirtiness to render delegate one
    int dirtiness = attr->get_output_dirtiness();
    if (attr->get_event_info().type != OfAttrEvent::TYPE_PROPAGATE) {
        if (dirtiness & OfAttr::DIRTINESS_SHADING_MATERIAL) transmitted_dirtiness |=  DIRTINESS_SHADING_GROUP;
        //if (attr->get_name() == "override_material") transmitted_dirtiness |=  DIRTINESS_SHADING_GROUP;
    }
    if (dirtiness & OfAttr::DIRTINESS_MOTION) transmitted_dirtiness |=  DIRTINESS_KINEMATIC;
    if (dirtiness & OfAttr::DIRTINESS_SHADING_GROUP) transmitted_dirtiness |=  DIRTINESS_SHADING_GROUP;
    if (dirtiness & OfAttr::DIRTINESS_GEOMETRY) transmitted_dirtiness |=  DIRTINESS_GEOMETRY;

    if (descriptor.is_geometry()) { // most cases
        m_render_delegate->dirty_geometry(descriptor, transmitted_dirtiness);
    } else { // it's an instancer we have potentially quite some work to do
        if (transmitted_dirtiness & DIRTINESS_GEOMETRY) {
            CoreArray<OfObject *> **item_prototypes = m_instancer_prototypes.is_key_exists(item);
            CoreVector<OfObject *> inserted, removed;
            const CoreBasicArray<OfObject *> *final_prototypes = nullptr;
            if (item_prototypes != nullptr) {
                // we have quite a lot to do here to notify render delegates about the prototypes
                // creating a hash table of the old prototype for faster item existence checks
                CoreSet<OfObject *> old_prototypes_set;
                for (auto prototype : **item_prototypes) old_prototypes_set.add(prototype);

                // we must delete here if done later it will delete the new prototypes!
                // remember it's a ** and has the same key...
                delete *item_prototypes;
                // then we must the key from the hash table since resolving new prototypes will add them back
                m_instancer_prototypes.remove(item);
                // finally we can resolve the new prototypes
                 const CoreBasicArray<OfObject *>& new_prototypes = resolve_instancer_prototypes(*static_cast<ModuleSceneObjectTree *>(item->get_module()), m_instancer_prototypes);

                // let's now compare old and new ones to create new arrays of removed and new inserted ones...
                // in order to notify the render delegate
                unsigned int idx;
                for (unsigned int i = 0; i < new_prototypes.get_count(); i++) {
                    if (old_prototypes_set.exists(new_prototypes[i], idx)) {
                        // we remove existing prototypes to only keep removed ones
                        old_prototypes_set.remove(idx);
                    } else { // it's a new prototype
                        inserted.add(new_prototypes[i]);
                    }
                    // test for early exit since nothing can be left
                    if (old_prototypes_set.get_count() == 0) break;
                }
                // populating removed items
                removed.resize(old_prototypes_set.get_count());
                for (unsigned int i = 0; i < removed.get_count(); i++) removed[i] = old_prototypes_set[i];
                final_prototypes = &inserted;
            } else { // no old prototypes defined so we can just get the resolved prototypes
                final_prototypes = &resolve_instancer_prototypes(*static_cast<ModuleSceneObjectTree *>(item->get_module()), m_instancer_prototypes);
            }

            // notifying the render delegate
            for (auto prototype : removed) remove_geometry(prototype);
            for (auto prototype : *final_prototypes) insert_geometry(prototype);
        }
        // propagating the dirtiness on the instancer
        m_render_delegate->dirty_instancer(descriptor, transmitted_dirtiness);
    }
}

/*! \brief converting Clarisse dirtiness to light dirtiness for the render delegate */
void
R2cSceneDelegate::dispatch_light_dirtiness(EventObject& sender, const EventInfo& evtid, void *data)
{
    OfObject * item = static_cast<OfObject *>(&sender);
    R2cItemDescriptor descriptor = get_item_descriptor(item);
    int dirtiness = item->get_dirtiness();
    int transmitted_dirtiness = DIRTINESS_NONE;

    // converting clarisse dirtiness to the render delegate

    // FIXME: CLARISSEAPI
    // We always send DIRTINESS_SHADING_LIGHT for lights because of the output dirtiness
    // We need to change the bevahior since we can track it at the layer level...
    transmitted_dirtiness = (dirtiness <= OfAttr::DIRTINESS_MOTION) ? DIRTINESS_KINEMATIC : (DIRTINESS_KINEMATIC | DIRTINESS_LIGHT);
    m_render_delegate->dirty_light(descriptor, transmitted_dirtiness);
}

R2cItemDescriptor
R2cSceneDelegate::get_render_item(R2cItemId id) const
{
    R2cItemDescriptor *item = m_render_item_dependencies.is_key_exists(id);
    return item != nullptr ? *item : R2cItemDescriptor();
}

/*! \brief Returns the global transform of the specified item */
const GMathMatrix4x4d&
R2cSceneDelegate::get_transform(R2cItemId id) const
{
    static GMathMatrix4x4d identity(true);
    R2cItemDescriptor *item = m_render_item_dependencies.is_key_exists(id);
    if (item != nullptr && !item->is_destroyed() && item->has_transform()) {
        ModuleSceneItem *scene_item = static_cast<ModuleSceneItem *>(item->get_item()->get_module());
        return scene_item->get_global_matrix();
    }
    return identity;
}

bool
R2cSceneDelegate::get_visible(R2cItemId id) const
{
    R2cItemDescriptor item = get_render_item(id);
    if (!item.is_destroyed()) {
        OfObject *ptr = item.get_item();
        bool *exists = nullptr;
        exists = item.is_light() ? m_light_index.is_key_exists(ptr) : m_geometry_index.is_key_exists(ptr);
        return exists != nullptr;
    }
    return false;
}

R2cGeometryResource
R2cSceneDelegate::get_geometry_resource(R2cItemId id) const
{
    if (id != nullptr) {
        OfObject *item = static_cast<OfObject *>(id);
        if (item->get_module()->is_kindof(ModuleGeometry::class_info())) { // it's indeed a geometry
            ModuleGeometry *module = static_cast<ModuleGeometry *>(item->get_module());
            R2cGeometryResource resource;
            resource.set_id(const_cast<ResourceData *>(module->get_resource(ModuleGeometry::RESOURCE_ID_TESSELLATION_DEFORMED)));
            return resource;
        }
    }
    return R2cGeometryResource();
}

R2cItemDescriptor
R2cSceneDelegate::get_material(R2cItemId scene_object_id, const unsigned int& shading_group_id) const
{
    // FIXME: Should be extended to support shading layer and the shading table instead
    R2cItemDescriptor *item = m_render_item_dependencies.is_key_exists(scene_object_id);
    R2cItemDescriptor assigned_material;
    assigned_material.set_type(R2cItemDescriptor::TYPE_MATERIAL);
    if (item != nullptr) {
        ModuleSceneObject *scene_object = static_cast<ModuleSceneObject *>(item->get_item()->get_module());
        ModuleMaterial *material_module = scene_object->get_override_material();
        OfObject *material = material_module != nullptr ? material_module->get_object() : nullptr;
        if (is_class_supported(material, m_supported_classes.materials)) {
            assigned_material.set_id(material);
            assigned_material.set_full_name(material->get_full_name());
            assigned_material.set_refcount(1);
        } else { // no valid material override then we need to look at the shading groups
            if (shading_group_id < scene_object->get_shading_groups().get_count()) {
                material_module = scene_object->get_material(shading_group_id);
                // making sure a supported material is assigned
                if (material_module != nullptr && is_class_supported(material_module->get_object(), m_supported_classes.materials)) {
                    material = material_module->get_object();
                    assigned_material.set_id(material);
                    assigned_material.set_full_name(material->get_full_name());
                    assigned_material.set_refcount(1);
                }
            }
        }
    }
    return assigned_material;
}

void
R2cSceneDelegate::remove_geometry(R2cItemId id)
{
    // since the item might be already destroyed we retreive the description
    // we stored in the render index
    R2cItemDescriptor *item = m_render_item_dependencies.is_key_exists(id);
    if (item != nullptr) {
        if (item->get_refcount() <= 1) {
            if (!item->is_destroyed()) {
                disconnect_all(*item->get_item());
            }
            // make a copy before removal
            R2cItemDescriptor cpy(*item);
            m_render_item_dependencies.remove(id);

            if (cpy.is_geometry()) {
                m_render_delegate->remove_geometry(cpy);
            } else {
                m_render_delegate->remove_instancer(cpy);
            }
        } else {
            item->set_refcount(item->get_refcount() - 1);
        }
    } // else should assert
}

void
R2cSceneDelegate::insert_geometry(OfObject *geometry)
{
    R2cItemDescriptor *item = m_render_item_dependencies.is_key_exists(geometry);
    if (item == nullptr) { // it's a new item
        R2cItemDescriptor new_item = get_item_descriptor(geometry);
        new_item.set_refcount(1);
        connect(*geometry, EVT_ID_DESTROY, EVENT_INFO_METHOD(R2cSceneDelegate::on_dependency_destroyed), &m_geometry_index);
        connect(*geometry, EVT_ID_OF_OBJECT_ATTR_CHANGE, EVENT_INFO_METHOD(R2cSceneDelegate::dispatch_scene_object_dirtiness));
        if (new_item.is_instancer()) {
            m_render_delegate->insert_instancer(new_item);
        } else {
            m_render_delegate->insert_geometry(new_item);
        }
        m_render_item_dependencies.add(geometry, new_item);
    } else { // we just increment the refcount
        item->set_refcount(item->get_refcount() + 1);
    }
}

void
R2cSceneDelegate::remove_light(R2cItemId id)
{
    // since the item might be already destroyed we retreive the description
    // we stored in the render index
    R2cItemDescriptor *item = m_render_item_dependencies.is_key_exists(id);
    if (item != nullptr) {
        if (!item->is_destroyed()) {
            disconnect_all(*item->get_item());
        }
        R2cItemDescriptor cpy(*item);
        m_render_item_dependencies.remove(id);
        m_render_delegate->remove_light(cpy);
    } // else should assert
}

void
R2cSceneDelegate::insert_light(OfObject *light)
{
    R2cItemDescriptor item = get_item_descriptor(light);
    item.set_refcount(1);
    connect(*light, EVT_ID_DESTROY, EVENT_INFO_METHOD(R2cSceneDelegate::on_dependency_destroyed), &m_light_index);
    connect(*light, EVT_ID_OF_OBJECT_DIRTINESS, EVENT_INFO_METHOD(R2cSceneDelegate::dispatch_light_dirtiness));
    m_render_item_dependencies.add(light, item);
    m_render_delegate->insert_light(item);
}

/*!\brief Internal helper to synchronize previous index from a group.*/
void
R2cSceneDelegate::sync_index(OfObject *item,
                                    OfObjectIndex& index,
                                    const CoreVector<const OfClass *>& supported_classes,
                                    CoreVector<OfObject *>& added_items,
                                    CoreVector<OfObject *>& removed_items)
{
    // make sure we initialize the incoming arrays
    added_items.remove_all();
    removed_items.remove_all();

    ModuleGroup *group = static_cast<ModuleGroup *>(item->get_module());
    CoreArray<OfObject *> new_items;
    group->get_updated_objects(new_items);

    for (auto item : new_items) {
        if (is_class_supported(item, supported_classes)) {
            // value is true is the item is still alive!
            bool *value = index.is_key_exists(item);
            if (value == nullptr) { // this is a new item that wasn't in our index
                added_items.add(item);
                if (item->get_module()->is_kindof(ModuleSceneObjectTree::class_info())) {
                    // instancers needs some extra work since they have prototypes that may not be
                    // directly part of the visibility group. This happens a lot of times when prototypes are located outside
                    // the geometry group. We need to gather these prototypes and then add them as render geometry to notify
                    // the render delegate. These geometries will be considered as invisible to avoid displaying actual
                    // prototypes in the render scene.
                    // since it's a new instancer, we need to resolve its prototypes and add them as render items
                    const CoreBasicArray<OfObject *>& prototypes = resolve_instancer_prototypes(*static_cast<ModuleSceneObjectTree *>(item->get_module()), m_instancer_prototypes);
                    for (auto prototype : prototypes) insert_geometry(prototype);
                }
            } else { // we remove existing item to only keep the removed items
                index.remove(item);
            }
        }
    }

    // remaining items are the ones that have been removed from the input group
    removed_items.resize(index.get_count());
    unsigned int i = 0;
    for (auto removed_item : index) {
        removed_items[i] = removed_item.get_key();
        i++;
    }
    // clear the index to rebuild it with the list of items
    index.remove_all();
    // create the new index
    for (auto item : new_items) index.add(item, true);
}

void
R2cSceneDelegate::sync()
{
    // FIXME: Render delegate could eventually tell which classes it supports.

    CoreVector<OfObject *> inserted;
    CoreVector<OfObject *> removed;
    // check if our geometries list is dirty, otherwise there's nothing to do
    if (m_geometry_index_dirty) {
        sync_index(m_geometries, m_geometry_index, m_supported_classes.geometries, inserted, removed);
        // setting geometries map dirtiness to false since we just synched
        m_geometry_index_dirty = false;

        // removing items from the render scene
        for (auto item : removed) remove_geometry(item);
        // adding new items to the render scene
        for (auto item : inserted) insert_geometry(item);
    }

    // check if our light index is dirty, otherwise there's nothing to do
    if (m_light_index_dirty) {
        sync_index(m_lights, m_light_index, m_supported_classes.lights, inserted, removed);
        // setting lights map dirtiness to false since we just synched
        m_light_index_dirty = false;

        // removing items from the render scene
        for (auto item : removed) remove_light(item);
        // adding new items to the render scene
        for (auto item : inserted) insert_light(item);
    }

}

R2cItemDescriptor
R2cSceneDelegate::get_item_descriptor(OfObject *item) const
{
    static const OfClass *scene_object_cls = get_application().get_factory().get_classes().get("SceneObject");
    static const OfClass *light_cls = get_application().get_factory().get_classes().get("Light");
    static const OfClass *material_cls = get_application().get_factory().get_classes().get("Material");
    static const OfClass *camera_cls = get_application().get_factory().get_classes().get("Camera");

    R2cItemDescriptor result;
    result.set_id(item);
    if (item != nullptr) {
        result.set_refcount(1);
        result.set_full_name(item->get_full_name());
        if (item->is_kindof(*material_cls)) {
            result.set_type(R2cItemDescriptor::TYPE_MATERIAL);
        } else if (item->is_kindof(*light_cls)) {
            result.set_type(R2cItemDescriptor::TYPE_LIGHT);
        } else if (item->is_kindof(*camera_cls)) {
            result.set_type(R2cItemDescriptor::TYPE_CAMERA);
        } else if (item->is_kindof(*scene_object_cls)) {
            result.set_type(item->get_module()->is_kindof(ModuleSceneObjectTree::class_info()) ? R2cItemDescriptor::TYPE_INSTANCER : R2cItemDescriptor::TYPE_GEOMETRY);
        }
    } else {
        result.set_refcount(-1);
    }
    return result;
}

R2cInstancer *
R2cSceneDelegate::create_instancer_description(R2cItemId id, bool flattened) const
{
    R2cItemDescriptor item = get_render_item(id);
    if (item.is_instancer()) {
        if (!flattened) {
            LOG_WARNING("R2cSceneDelegate::create_instancer_description: Non flattened instancers not supported. Returning flattened version\n");
        }
        R2cInstancer *instancer = new R2cInstancer(*this, id, *m_instancer_prototypes.get_value(item.get_item()));
        return instancer;
    } else {
        return nullptr;
    }
}

const CoreArray<R2cItemId>&
R2cSceneDelegate::get_prototypes(R2cItemId id, bool flattened) const
{
    static CoreArray<R2cItemId> empty;
    if (!flattened) {
        LOG_WARNING("R2cSceneDelegate::create_instancer_description: Non flattened instancers not supported. Returning flattened version\n");
    }
    CoreArray<OfObject *> **prototypes = m_instancer_prototypes.is_key_exists(static_cast<OfObject *>(id));
    CoreArray<R2cItemId> *result = reinterpret_cast<CoreArray<R2cItemId> *>(*prototypes);
    return prototypes != nullptr ? *result : empty;
}

void
R2cSceneDelegate::destroy_instancer_description(R2cInstancer *instancer) const
{
    delete instancer;
}

CoreString
R2cSceneDelegate::get_dirtiness_name(const Dirtiness& dirtiness)
{
    switch(dirtiness) {
        case DIRTINESS_NONE:
            return "DIRTINESS_NONE";
        case DIRTINESS_KINEMATIC:
            return "DIRTINESS_KINEMATIC";
        case DIRTINESS_LIGHT:
            return "DIRTINESS_LIGHT";
        case DIRTINESS_SHADING_GROUP:
            return "DIRTINESS_SHADING_GROUP";
        case DIRTINESS_MATERIAL:
            return "DIRTINESS_MATERIAL";
        case DIRTINESS_VISIBILITY:
            return "DIRTINESS_VISIBILITY";
        case DIRTINESS_GEOMETRY:
            return "DIRTINESS_GEOMETRY";
        case DIRTINESS_ALL:
            return "DIRTINESS_ALL";
        default:
            return "DIRTINESS_UNKNOWN";
    }
}

void
R2cSceneDelegate::get_dirtiness_names(CoreVector<CoreString>& names, const int& dirtiness)
{
    names.remove_all();
    if (dirtiness & DIRTINESS_KINEMATIC) names.add("DIRTINESS_KINEMATIC");
    if (dirtiness & DIRTINESS_LIGHT) names.add("DIRTINESS_LIGHT");
    if (dirtiness & DIRTINESS_SHADING_GROUP) names.add("DIRTINESS_SHADING_GROUP");
    if (dirtiness & DIRTINESS_MATERIAL) names.add("DIRTINESS_MATERIAL");
    if (dirtiness & DIRTINESS_VISIBILITY) names.add("DIRTINESS_VISIBILITY");
    if (dirtiness & DIRTINESS_GEOMETRY) names.add("DIRTINESS_GEOMETRY");
    if (dirtiness & DIRTINESS_ALL) names.add("DIRTINESS_ALL");
}

void
R2cSceneDelegate::dirty_geometry_index()
{
    m_geometry_index_dirty = true;
}

void
R2cSceneDelegate::dirty_light_index()
{
    m_light_index_dirty = true;
}

void
R2cSceneDelegate::dirty_all()
{
    dirty_geometry_index();
    dirty_light_index();
}

void
R2cSceneDelegate::clear()
{
    if (m_render_delegate != nullptr) {
        dirty_all();

        // clearing instance prototype cache
        for (auto prototypes : m_instancer_prototypes) {
            delete prototypes.get_value();
        }
        m_instancer_prototypes.remove_all();
        m_render_item_dependencies.remove_all();
        m_geometry_index.remove_all();
        m_light_index.remove_all();
    }
}

void
R2cSceneDelegate::set_render_delegate(R2cRenderDelegate *render_delegate)
{
    if (m_render_delegate != render_delegate) {
        if (this == get_null()) { // early exit if we are the null singleton
            return;
        }

        if (m_render_delegate != nullptr) {
            // clearing our previous render delegate
            m_render_delegate->clear();
            m_render_delegate->set_scene_delegate(get_null());
            m_render_delegate = nullptr;
        }

        if (render_delegate != nullptr) {
            // clearing the previous render delegate
            render_delegate->clear();
            // notify the former scene delegate that it is not attached to it anymore
            const_cast<R2cSceneDelegate *>(render_delegate->get_scene_delegate())->set_render_delegate(nullptr);
            // attach it to us
            render_delegate->set_scene_delegate(this);
            // clear ourselves so we can repopulate ourselfves to the new delegate during sync
            clear();
            // populate supported class with the new render delegate
            populate_supported_classes(m_supported_classes.lights, *render_delegate, render_delegate->get_supported_lights(), get_application());
            populate_supported_classes(m_supported_classes.materials, *render_delegate, render_delegate->get_supported_materials(), get_application());
            populate_supported_classes(m_supported_classes.geometries, *render_delegate, render_delegate->get_supported_geometries(), get_application());
            populate_supported_classes(m_supported_classes.cameras, *render_delegate, render_delegate->get_supported_cameras(), get_application());
        }
        m_render_delegate = render_delegate;
    } // else nothing to do since it's the same
}

static OfApp *__application__ = nullptr;
bool
R2cSceneDelegate::is_initialized() { return __application__  != nullptr; }

void
R2cSceneDelegate::initialize(OfApp& application)
{
    if (!is_initialized()) {
        __application__ = &application;
    }
}
R2cSceneDelegate *
R2cSceneDelegate::create()
{
    if (!is_initialized()) {
        LOG_FATAL("You must first call R2cSceneDelegate::initialize!");
        return nullptr;
    }
    return new R2cSceneDelegate;
}

const R2cSceneDelegate *
R2cSceneDelegate::get_null()
{
    static const R2cSceneDelegate null_scene;
    if (!is_initialized()) {
        LOG_FATAL("You must first call R2cSceneDelegate::initialize!");
        return nullptr;
    }
    return &null_scene;
}

void
R2cSceneDelegate::destroy(R2cSceneDelegate *delegate)
{
    delete delegate;
}

OfApp&
R2cSceneDelegate::get_application() const
{
    return *__application__;
}