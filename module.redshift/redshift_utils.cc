//
// Copyright 2020 - present Isotropix SAS. See License.txt for license information
//

#include <core_log.h>
#include <sys_thread_lock.h>

#include <image_canvas.h>
#include <image_map_channel.h>
#include <of_app.h>
#include <module_camera.h>
#include <module_layer.h>
#include <module_geometry.h>
#include <module_scene_object_tree.h>
#include <poly_mesh.h>
#include <poly_mesh_smoothed.h>

#include <gmath_matrix3x3.h>

#include <RS.h>

#include <module_material_redshift.h>
#include <module_light_redshift.h>

#include <r2c_instancer.h>
#include <r2c_render_buffer.h>

#include "redshift_utils.h"

#define CLARISSE_SINK 0

RenderingBlockSink::RenderingBlockSink() : RSBlockSink(), m_render_buffer(nullptr)
{}

RenderingBlockSink::~RenderingBlockSink()
{
    // must remove the block sink when destroyed since it is tied to the layer/canvas
    // and we don't want a dangling pointer right?

    struct RSBlockSinkInfo {
        RSBlockSink *ptr;
        unsigned int layer_id;
    };

    // gathering sink info
    CoreVector<RSBlockSinkInfo> sinks;
    RSBlockSinkInfo sink_info;
    for (unsigned int i = 0; i < RS_RenderChannel_GetMain()->GetNumBlockSinks(); i++) {
        RSBlockSink *sink = RS_RenderChannel_GetMain()->GetBlockSink(i);
        if (sink != this) {
            sink_info.ptr = sink;
            sink_info.layer_id = RS_RenderChannel_GetMain()->GetBlockSinkLayerID(i);
            sinks.add(sink_info);
        }
    }
    // rebuilding sinks
    RS_RenderChannel_GetMain()->SetNumBlockSinks(sinks.get_count());
    for (unsigned int i = 0; i < sinks.get_count(); i++) {
        RS_RenderChannel_GetMain()->SetBlockSink(i, sinks[i].layer_id, sinks[i].ptr);
    }
}

unsigned int
RenderingBlockSink::GetWidth() const
{
    return m_render_buffer != nullptr ? static_cast<unsigned int>(m_render_buffer->get_width()) : 0;
}

unsigned int
RenderingBlockSink::GetHeight() const
{
    return m_render_buffer != nullptr ? static_cast<unsigned int>(m_render_buffer->get_height()) : 0;
}

void
RenderingBlockSink::OutputBlock(unsigned int layer_id, unsigned int denoisePassID, unsigned int offsetX, unsigned int offsetY, unsigned int width, unsigned int height, unsigned int stride, const char *pDataType, const char *pBitDepth, float gamma, bool clamped, const void *data)
{
    if (layer_id == 0 && m_render_buffer!= nullptr && data != nullptr && strcmp(pBitDepth, "FLOAT32") == 0) {
        unsigned int numSourceChannels = 0;
        if (strcmp(pDataType, "RGB") == 0) {
            numSourceChannels = 3;
        } else if (strcmp(pDataType, "RGBA") == 0) {
            numSourceChannels = 4;
        } else if (strcmp(pDataType, "SCALAR") == 0) {
            numSourceChannels = 1;
        } else if (strcmp(pDataType, "POINT") == 0)
            numSourceChannels = 3;
        else {
            // unrecognized!
        }

        if (numSourceChannels == 4) {
            R2cRenderBuffer::Region region(offsetX, offsetY, width, height);
            // we have to lock because of potential conccurent calls when rendering on multiple GPUs
            m_render_buffer->fill_rgba_region(static_cast<const float *>(data), stride, region, true);
        }
    }
}

void
RenderingBlockSink::NotifyWillRenderBlock(unsigned int offsetX, unsigned int offsetY, unsigned int width, unsigned int height)
{
    m_render_buffer->notify_start_render_region(R2cRenderBuffer::Region(offsetX, offsetY, width, height), true);
}

void
RenderingBlockSink::PreRender()
{
    //LOG_INFO("RenderingBlockSink::PreRender()\n");
}

void
RenderingBlockSink::PostRender()
{
    //LOG_INFO("RenderingBlockSink::PostRender()\n");
}

void
ClarisseLogSink::Log(RSLogLevel level, const char *msg, size_t nCharsMSG)
{
    RSString strippedTags;
    switch(level) {
        case Error:
            strippedTags = msg;
            RS_Utility_StripHTMLTags(strippedTags);
            LOG_ERROR(strippedTags);
            break;
        case Warning:
            strippedTags = msg;
            RS_Utility_StripHTMLTags(strippedTags);
            LOG_WARNING(strippedTags);
            break;
        case Info:
            strippedTags = msg;
            RS_Utility_StripHTMLTags(strippedTags);
            LOG_INFO(strippedTags);
            break;
        case Detailed:
        case Debug:
        case DebugVerbose:
        case InternalOnly:
            LOG_DEBUG(strippedTags);
            break;
        default:
            break;
    }
}

RSMeshBase *
RedshiftUtils::CreateGeometry(const R2cSceneDelegate& delegate, R2cItemId geometry, RSMaterial *material, RSResourceInfo::Type& type)
{
    // initializing OfClass for faster access
    // Nitpicking: Classes could potentially be dynamically removed but that never happens.
    // The anal way would be to connect to the right event to update the pointers...
    static const OfClass *GeometryPolymeshClass = delegate.get_application().get_factory().get_classes().get("GeometryPolymesh");
    static const OfClass *GeometrySphereClass = delegate.get_application().get_factory().get_classes().get("GeometrySphere");
    static const OfClass *GeometryBoxClass = delegate.get_application().get_factory().get_classes().get("GeometryBox");

    static const CoreClassInfo *tmesh_type = CoreClassInfo::get_class("TessellationMesh");

    R2cItemDescriptor idesc = delegate.get_render_item(geometry);
    OfObject *item = idesc.get_item();

    ModuleSceneObject *module = static_cast<ModuleSceneObject *>(item->get_module());

    if (item->is_kindof(*GeometryPolymeshClass)) { // test if it's a polymesh
        type = RSResourceInfo::TYPE_MESH;
        R2cGeometryResource resource = delegate.get_geometry_resource(geometry);
        const GeometryObject *geo = resource.get_geometry();
        if (geo != nullptr) { // safety net but shouldn't really happen
            if (geo->is_kindof(PolyMesh::class_info())) { // it's a polymesh
                return CreatePolymesh(*dynamic_cast<const PolyMesh *>(geo), material);
            } else if ((geo->is_kindof(PolyMeshSmoothed::class_info())) ||
                       (tmesh_type != nullptr && geo->is_kindof(*tmesh_type))) { // it's a polymeshsmoothed or tessellation mesh
                return CreateGeometryPolymesh(*geo, material);
            }
        } // else something wrong has happened since we should always have a resource
    } else if (item->is_kindof(*GeometrySphereClass)) {
        type = RSResourceInfo::TYPE_POINT_CLOUD;
        const float radius = static_cast<float>(item->get_attribute("radius")->get_double());
        return CreateSphere(radius, material);
    } else if (item->is_kindof(*GeometryBoxClass)) {
        type = RSResourceInfo::TYPE_MESH;
        return CreateBox(item->get_attribute("size")->get_vec3d(), material);
    }
    type = RSResourceInfo::TYPE_MESH;
    // don't know the geometry type, let's create a bbox
    return CreateBbox(module->get_bbox(), material);
}


template <typename T>
inline void	SetVertexData(void *vtx_data_struct, unsigned int attribute_byte_offset, const T& data)
{
    memcpy((static_cast<char *>(vtx_data_struct)) + attribute_byte_offset, &data, sizeof(T));
}

RSMesh *
RedshiftUtils::CreatePolymesh(const PolyMesh& polymesh, RSMaterial *material)
{
    const unsigned int attribute_count = 2;
    const unsigned int material_count = polymesh.get_shading_group_names().get_count();

    RSMesh *mesh = RS_Mesh_New(get_new_unique_name("GeometrySmoothed").get_data());
    mesh->SetIsTransformationBlurred(false);
    mesh->SetNumMaterials(material_count);

    RSArray<RSMaterial *> materials;
    for (unsigned int i = 0; i < material_count; i++) {
        mesh->SetMaterial(i, material);
        materials.Add(material);
    }

    RSVertexData* pMyVertexFormatData = RS_VertexData_New();
    pMyVertexFormatData->SetNumAttributes(attribute_count);

    const unsigned int positionStreamOriginalIndex = 0;
    const unsigned int normalStreamOriginalIndex = 1;
    //const unsigned int uv0StreamOriginalIndex = 2;

    pMyVertexFormatData->SetAttributeDefinition(positionStreamOriginalIndex, "RS_ATTRIBUTETYPE_FLOAT3", "RS_ATTRIBUTEUSAGETYPE_POSITION", "<<position>>");
    pMyVertexFormatData->SetAttributeDefinition(normalStreamOriginalIndex, "RS_ATTRIBUTETYPE_NORMAL", "RS_ATTRIBUTEUSAGETYPE_NORMAL", "<<normal>>");

    pMyVertexFormatData->FinalizeAttributeFormatAndAllocate(true, 1, false, mesh->GetName(), materials);

    // Cache the re-mapped offsets into the finalized vertex data structure
    unsigned int vtx_attr_offsets[attribute_count];
    for (unsigned int originalAttributeIndex = 0; originalAttributeIndex < attribute_count; originalAttributeIndex++) {
        // Check to see if the remapped attribute has been stripped by the 'RS_GenerateVertexFormatFromMeshMaterials' function. If it has, the remapped index will be 'RS_INVALIDATTRIBUTEINDEX'
        if (pMyVertexFormatData->IsAttributeUsed(originalAttributeIndex))
            vtx_attr_offsets[originalAttributeIndex] = pMyVertexFormatData->GetAttributeOffsetBytes(originalAttributeIndex);
        else
            vtx_attr_offsets[originalAttributeIndex] = 0xFFFF;	// an invalid offset (see AddTrianglesToMesh for usage)
    }

    unsigned int p_offset = vtx_attr_offsets[positionStreamOriginalIndex];
    unsigned int n_offset= vtx_attr_offsets[normalStreamOriginalIndex];

    CoreArray<GMathVec3f> vertices;
    CoreArray<unsigned int> polygon_vertex_ids;
    CoreArray<unsigned int> polygon_vertex_count;
    CoreArray<unsigned int> polygon_shading_groups;

    const GeometryPointCloud *ptc = polymesh.get_point_cloud();

    ptc->get_positions(vertices);
    polymesh.get_polygon_vertex_count(polygon_vertex_count);
    polymesh.get_polygon_vertex_indices(polygon_vertex_ids);
    polymesh.get_polygon_shading_groups(polygon_shading_groups);

    unsigned int offset = 0;
    unsigned int *ids;

    mesh->SetAttributesFormat(pMyVertexFormatData);
    mesh->BeginPrimitives(1);

    char vtx_data[4][1024]; // temporary vertex buffer data
    for (unsigned int i = 0; i < polymesh.get_polygon_count(); i++) {
        ids = &polygon_vertex_ids[offset];

        if (polygon_vertex_count[i] == 3) {
            SetVertexData(vtx_data[0], p_offset, RSVector3(vertices[ids[0]][0], vertices[ids[0]][1], -vertices[ids[0]][2]));
            SetVertexData(vtx_data[1], p_offset, RSVector3(vertices[ids[1]][0], vertices[ids[1]][1], -vertices[ids[1]][2]));
            SetVertexData(vtx_data[2], p_offset, RSVector3(vertices[ids[2]][0], vertices[ids[2]][1], -vertices[ids[2]][2]));

            SetVertexData(vtx_data[0], n_offset, RSNormal(ptc->get_normal(ids[0])[0], ptc->get_normal(ids[0])[1], -ptc->get_normal(ids[0])[2]));
            SetVertexData(vtx_data[1], n_offset, RSNormal(ptc->get_normal(ids[1])[0], ptc->get_normal(ids[1])[1], -ptc->get_normal(ids[1])[2]));
            SetVertexData(vtx_data[2], n_offset, RSNormal(ptc->get_normal(ids[2])[0], ptc->get_normal(ids[2])[1], -ptc->get_normal(ids[2])[2]));

            mesh->AddTri(vtx_data[0], vtx_data[1], vtx_data[2],
                          0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                          ids[0], ids[1], ids[2], static_cast<unsigned short>(polygon_shading_groups[i]));

        } else if (polygon_vertex_count[i] == 4) {
            SetVertexData(vtx_data[0], p_offset, RSVector3(vertices[ids[0]][0], vertices[ids[0]][1], -vertices[ids[0]][2]));
            SetVertexData(vtx_data[1], p_offset, RSVector3(vertices[ids[1]][0], vertices[ids[1]][1], -vertices[ids[1]][2]));
            SetVertexData(vtx_data[2], p_offset, RSVector3(vertices[ids[2]][0], vertices[ids[2]][1], -vertices[ids[2]][2]));
            SetVertexData(vtx_data[3], p_offset, RSVector3(vertices[ids[3]][0], vertices[ids[3]][1], -vertices[ids[3]][2]));

            SetVertexData(vtx_data[0], n_offset, RSNormal(ptc->get_normal(ids[0])[0], ptc->get_normal(ids[0])[1], -ptc->get_normal(ids[0])[2]));
            SetVertexData(vtx_data[1], n_offset, RSNormal(ptc->get_normal(ids[1])[0], ptc->get_normal(ids[1])[1], -ptc->get_normal(ids[1])[2]));
            SetVertexData(vtx_data[2], n_offset, RSNormal(ptc->get_normal(ids[2])[0], ptc->get_normal(ids[2])[1], -ptc->get_normal(ids[2])[2]));
            SetVertexData(vtx_data[3], n_offset, RSNormal(ptc->get_normal(ids[3])[0], ptc->get_normal(ids[3])[1], -ptc->get_normal(ids[3])[2]));

            mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                          0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                          ids[0], ids[1], ids[2], ids[3], static_cast<unsigned short>(polygon_shading_groups[i]));
        }
        offset += polygon_vertex_count[i];
    }
    mesh->CompactDataAndPrepareForRendering();
    RS_VertexData_Delete(pMyVertexFormatData);
    return mesh;
}

RSMesh *
RedshiftUtils::CreateGeometryPolymesh(const GeometryObject& geometry, RSMaterial *material)
{
    const unsigned int attribute_count = 2;
    const unsigned int material_count = geometry.get_shading_group_names().get_count();


    RSMesh *mesh = RS_Mesh_New(get_new_unique_name("GeometrySmoothed").get_data());
    mesh->SetIsTransformationBlurred(false);
    mesh->SetNumMaterials(material_count);

    RSArray<RSMaterial *> materials;
    for (unsigned int i = 0; i < material_count; i++) {
        mesh->SetMaterial(i, material);
        materials.Add(material);
    }

    RSVertexData* pMyVertexFormatData = RS_VertexData_New();
    pMyVertexFormatData->SetNumAttributes(attribute_count);

    const unsigned int positionStreamOriginalIndex = 0;
    const unsigned int normalStreamOriginalIndex = 1;
    //const unsigned int uv0StreamOriginalIndex = 2;

    pMyVertexFormatData->SetAttributeDefinition(positionStreamOriginalIndex, "RS_ATTRIBUTETYPE_FLOAT3", "RS_ATTRIBUTEUSAGETYPE_POSITION", "<<position>>");
    pMyVertexFormatData->SetAttributeDefinition(normalStreamOriginalIndex, "RS_ATTRIBUTETYPE_NORMAL", "RS_ATTRIBUTEUSAGETYPE_NORMAL", "<<normal>>");

    pMyVertexFormatData->FinalizeAttributeFormatAndAllocate(true, 1, false, mesh->GetName(), materials);

    // Cache the re-mapped offsets into the finalized vertex data structure
    unsigned int vtx_attr_offsets[attribute_count];
    for (unsigned int originalAttributeIndex = 0; originalAttributeIndex < attribute_count; originalAttributeIndex++) {
        // Check to see if the remapped attribute has been stripped by the 'RS_GenerateVertexFormatFromMeshMaterials' function. If it has, the remapped index will be 'RS_INVALIDATTRIBUTEINDEX'
        if (pMyVertexFormatData->IsAttributeUsed(originalAttributeIndex))
            vtx_attr_offsets[originalAttributeIndex] = pMyVertexFormatData->GetAttributeOffsetBytes(originalAttributeIndex);
        else
            vtx_attr_offsets[originalAttributeIndex] = 0xFFFF;	// an invalid offset (see AddTrianglesToMesh for usage)
    }

    unsigned int p_offset = vtx_attr_offsets[positionStreamOriginalIndex];
    unsigned int n_offset= vtx_attr_offsets[normalStreamOriginalIndex];

    mesh->SetAttributesFormat(pMyVertexFormatData);
    mesh->BeginPrimitives(1);

    CoreArray<unsigned int> primitive_vertex_ids;
    const GeometryPointCloud *ptc = geometry.get_point_cloud();
    CoreArray<GMathVec3f> vertices;
    ptc->get_positions(vertices);

    CoreArray<GMathVec3f> normals;
    CoreArray<unsigned int> normal_index;

    unsigned int offset = 0;
    unsigned int *ids;

    if (geometry.get_normal_map_count() > 0) {
        geometry.get_normal_map_data(0, normals, normal_index);
    }

    geometry.get_primitive_indices(primitive_vertex_ids);

    char vtx_data[4][1024]; // temporary vertex buffer data
    unsigned int vidx = 0;

    for (unsigned int i = 0; i < geometry.get_primitive_count(); i++) {
        const unsigned int primitive_vertex_count = geometry.get_primitive_edge_count(i);
        // FIXME: potential problems with shading group since Redshift also support short
        const unsigned short shading_group_id = static_cast<unsigned short>(geometry.get_primitive_shading_group_index(i));
        const unsigned int id0 = vidx;
        const unsigned int id1 = vidx + 1;
        const unsigned int id2 = vidx + 2;
        const unsigned int id3 = vidx + 3;
        ids = &primitive_vertex_ids[offset];

        if (primitive_vertex_count == 3) {
            SetVertexData(vtx_data[0], p_offset, RSVector3(vertices[ids[0]][0], vertices[ids[0]][1], -vertices[ids[0]][2]));
            SetVertexData(vtx_data[1], p_offset, RSVector3(vertices[ids[1]][0], vertices[ids[1]][1], -vertices[ids[1]][2]));
            SetVertexData(vtx_data[2], p_offset, RSVector3(vertices[ids[2]][0], vertices[ids[2]][1], -vertices[ids[2]][2]));

            if (normals.get_count() > 0) { // don't like the if in the for loop
                SetVertexData(vtx_data[0], n_offset, RSNormal(normals[normal_index[id0]][0], normals[normal_index[id0]][1], -normals[normal_index[id0]][2]));
                SetVertexData(vtx_data[1], n_offset, RSNormal(normals[normal_index[id1]][0], normals[normal_index[id1]][1], -normals[normal_index[id1]][2]));
                SetVertexData(vtx_data[2], n_offset, RSNormal(normals[normal_index[id2]][0], normals[normal_index[id2]][1], -normals[normal_index[id2]][2]));
            } else {
                SetVertexData(vtx_data[0], n_offset, RSNormal(ptc->get_normal(ids[0])[0], ptc->get_normal(ids[0])[1], -ptc->get_normal(ids[0])[2]));
                SetVertexData(vtx_data[1], n_offset, RSNormal(ptc->get_normal(ids[1])[0], ptc->get_normal(ids[1])[1], -ptc->get_normal(ids[1])[2]));
                SetVertexData(vtx_data[2], n_offset, RSNormal(ptc->get_normal(ids[2])[0], ptc->get_normal(ids[2])[1], -ptc->get_normal(ids[2])[2]));
            }
            mesh->AddTri(vtx_data[0], vtx_data[1], vtx_data[2],
                          0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                          ids[0], ids[1], ids[2], shading_group_id);

        } else if (primitive_vertex_count == 4) {
            SetVertexData(vtx_data[0], p_offset, RSVector3(vertices[ids[0]][0], vertices[ids[0]][1], -vertices[ids[0]][2]));
            SetVertexData(vtx_data[1], p_offset, RSVector3(vertices[ids[1]][0], vertices[ids[1]][1], -vertices[ids[1]][2]));
            SetVertexData(vtx_data[2], p_offset, RSVector3(vertices[ids[2]][0], vertices[ids[2]][1], -vertices[ids[2]][2]));
            SetVertexData(vtx_data[3], p_offset, RSVector3(vertices[ids[3]][0], vertices[ids[3]][1], -vertices[ids[3]][2]));

            if (normals.get_count() > 0) {
                SetVertexData(vtx_data[0], n_offset, RSNormal(normals[normal_index[id0]][0], normals[normal_index[id0]][1], -normals[normal_index[id0]][2]));
                SetVertexData(vtx_data[1], n_offset, RSNormal(normals[normal_index[id1]][0], normals[normal_index[id1]][1], -normals[normal_index[id1]][2]));
                SetVertexData(vtx_data[2], n_offset, RSNormal(normals[normal_index[id2]][0], normals[normal_index[id2]][1], -normals[normal_index[id2]][2]));
                SetVertexData(vtx_data[3], n_offset, RSNormal(normals[normal_index[id3]][0], normals[normal_index[id3]][1], -normals[normal_index[id3]][2]));
            } else {
                SetVertexData(vtx_data[0], n_offset, RSNormal(ptc->get_normal(ids[0])[0], ptc->get_normal(ids[0])[1], -ptc->get_normal(ids[0])[2]));
                SetVertexData(vtx_data[1], n_offset, RSNormal(ptc->get_normal(ids[1])[0], ptc->get_normal(ids[1])[1], -ptc->get_normal(ids[1])[2]));
                SetVertexData(vtx_data[2], n_offset, RSNormal(ptc->get_normal(ids[2])[0], ptc->get_normal(ids[2])[1], -ptc->get_normal(ids[2])[2]));
                SetVertexData(vtx_data[3], n_offset, RSNormal(ptc->get_normal(ids[3])[0], ptc->get_normal(ids[3])[1], -ptc->get_normal(ids[3])[2]));
            }
            mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                          0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                          ids[0], ids[1], ids[2], ids[3], shading_group_id);

        }
        vidx += primitive_vertex_count;
        offset += primitive_vertex_count;
    }

    mesh->CompactDataAndPrepareForRendering();
    RS_VertexData_Delete(pMyVertexFormatData);
    return mesh;
}



RSPointCloud *
RedshiftUtils::CreateSphere(const float& radius, RSMaterial *material)
{
    RSPointData *pAttributeData = RS_PointData_New();

    // Declare the format of the attribute data and allocate the memory required.
    pAttributeData->SetNumAttributes(2);
    pAttributeData->SetAttributeDefinition(0, "RS_ATTRIBUTETYPE_FLOAT4", "AColor");
    pAttributeData->SetAttributeDefinition(1, "RS_ATTRIBUTETYPE_FLOAT1", "AScalar");
    pAttributeData->FinalizeAttributeFormatAndAllocate();

    // Now create the point cloud and set its instance matrices
    RSPointCloud *ptc = RS_PointCloud_New();
    // Set pre-requisite properties before adding point cloud primitives
    ptc->SetPrimitiveType("RS_POINTCLOUDPRIMITIVETYPE_SPHERE");
    ptc->SetIsTransformationBlurred(false);
    //ptc->SetMatrix(RSMatrix4x4().SetIdentity());
    ptc->SetAttributesFormat(pAttributeData);

    // MATERIAL
    RSArray<RSMaterial*> dMeshMaterials;
    dMeshMaterials.SetLength(1);
    dMeshMaterials[0] = material;
    ptc->SetMaterials(dMeshMaterials);

    // Start adding primitives and their attributes (if available)
    ptc->BeginPrimitives(1);
    RSVector4 posRadius(0, 0, 0, radius);
    ptc->AddPoint(posRadius, pAttributeData->GetRawAttributes());

    // Finished adding primitives - prepare for rendering
    ptc->CompactDataAndPrepareForRendering();

    RS_PointData_Delete(pAttributeData);
    return ptc;
}

RSMesh *
RedshiftUtils::CreateBox(const GMathVec3d& size, RSMaterial *material)
{
    const unsigned int attribute_count = 2;

    RSMesh *mesh = RS_Mesh_New(get_new_unique_name("GeometryBox").get_data());
    mesh->SetIsTransformationBlurred(false);
    mesh->SetNumMaterials(1);
    mesh->SetMaterial(0, material);

    RSArray<RSMaterial *> materials;
    materials.Add(material);

    RSVertexData* pMyVertexFormatData = RS_VertexData_New();
    pMyVertexFormatData->SetNumAttributes(attribute_count);

    const unsigned int positionStreamOriginalIndex = 0;
    const unsigned int normalStreamOriginalIndex = 1;
    //const int uv0StreamOriginalIndex = 2;

    pMyVertexFormatData->SetAttributeDefinition(positionStreamOriginalIndex, "RS_ATTRIBUTETYPE_FLOAT3", "RS_ATTRIBUTEUSAGETYPE_POSITION", "<<position>>");
    pMyVertexFormatData->SetAttributeDefinition(normalStreamOriginalIndex, "RS_ATTRIBUTETYPE_NORMAL", "RS_ATTRIBUTEUSAGETYPE_NORMAL", "<<normal>>");

    pMyVertexFormatData->FinalizeAttributeFormatAndAllocate(true, 1, false, mesh->GetName(), materials);

    // Cache the re-mapped offsets into the finalized vertex data structure
    unsigned int vtx_attr_offsets[attribute_count];
    for (unsigned int originalAttributeIndex = 0; originalAttributeIndex < attribute_count; originalAttributeIndex++) {
        // Check to see if the remapped attribute has been stripped by the 'RS_GenerateVertexFormatFromMeshMaterials' function. If it has, the remapped index will be 'RS_INVALIDATTRIBUTEINDEX'
        if (pMyVertexFormatData->IsAttributeUsed(originalAttributeIndex))
            vtx_attr_offsets[originalAttributeIndex] = pMyVertexFormatData->GetAttributeOffsetBytes(originalAttributeIndex);
        else
            vtx_attr_offsets[originalAttributeIndex] = 0xFFFF;	// an invalid offset (see AddTrianglesToMesh for usage)
    }

    unsigned int p_offset = vtx_attr_offsets[positionStreamOriginalIndex];
    unsigned int n_offset= vtx_attr_offsets[normalStreamOriginalIndex];

    mesh->SetAttributesFormat(pMyVertexFormatData);
    mesh->BeginPrimitives(1);

    char vtx_data[4][1024]; // temporary vertex buffer data
    GMathVec3f sizef(static_cast<float>(size[0] * 0.5), static_cast<float>(size[1] * 0.5), static_cast<float>(size[2] * 0.5));

    //front
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(-sizef[0], sizef[1], sizef[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(sizef[0], sizef[1], sizef[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(sizef[0], -sizef[1], sizef[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(-sizef[0], -sizef[1], sizef[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 0.0f, 1.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  0, 1, 2, 3, 0);
    // top
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(-sizef[0], sizef[1], sizef[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(-sizef[0], sizef[1], -sizef[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(sizef[0], sizef[1], -sizef[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(sizef[0], sizef[1], sizef[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 1.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  4, 5, 6, 7, 0);
    // back
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(-sizef[0], sizef[1], -sizef[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(sizef[0], sizef[1], -sizef[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(sizef[0], -sizef[1], -sizef[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(-sizef[0], -sizef[1], -sizef[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 0.0f, 1.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  8, 9, 10, 11, 0);
    // bottom
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(-sizef[0], -sizef[1], sizef[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(-sizef[0], -sizef[1], -sizef[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(sizef[0], -sizef[1], -sizef[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(sizef[0], -sizef[1], sizef[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 1.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  12, 13, 14, 15, 0);
    // left
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(-sizef[0], sizef[1], sizef[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(-sizef[0], -sizef[1], sizef[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(-sizef[0], -sizef[1], -sizef[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(-sizef[0], sizef[1], -sizef[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  16, 17, 18, 19, 0);
    // right
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(sizef[0], sizef[1], sizef[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(sizef[0], -sizef[1], sizef[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(sizef[0], -sizef[1], -sizef[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(sizef[0], sizef[1], -sizef[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  20, 21, 22, 23, 0);


    mesh->CompactDataAndPrepareForRendering();
    RS_VertexData_Delete(pMyVertexFormatData);
    return mesh;
}

RSMesh *
RedshiftUtils::CreateBbox(const GMathBbox3d& bbox, RSMaterial *material)
{
    const unsigned int attribute_count = 2;

    RSMesh *mesh = RS_Mesh_New(get_new_unique_name("GeometryBox").get_data());
    mesh->SetIsTransformationBlurred(false);
    mesh->SetNumMaterials(1);
    mesh->SetMaterial(0, material);

    RSArray<RSMaterial *> materials;
    materials.Add(material);

    RSVertexData* pMyVertexFormatData = RS_VertexData_New();
    pMyVertexFormatData->SetNumAttributes(attribute_count);

    const unsigned int positionStreamOriginalIndex = 0;
    const unsigned int normalStreamOriginalIndex = 1;
    //const int uv0StreamOriginalIndex = 2;

    pMyVertexFormatData->SetAttributeDefinition(positionStreamOriginalIndex, "RS_ATTRIBUTETYPE_FLOAT3", "RS_ATTRIBUTEUSAGETYPE_POSITION", "<<position>>");
    pMyVertexFormatData->SetAttributeDefinition(normalStreamOriginalIndex, "RS_ATTRIBUTETYPE_NORMAL", "RS_ATTRIBUTEUSAGETYPE_NORMAL", "<<normal>>");

    pMyVertexFormatData->FinalizeAttributeFormatAndAllocate(true, 1, false, mesh->GetName(), materials);

    // Cache the re-mapped offsets into the finalized vertex data structure
    unsigned int vtx_attr_offsets[attribute_count];
    for (unsigned int originalAttributeIndex = 0; originalAttributeIndex < attribute_count; originalAttributeIndex++) {
        // Check to see if the remapped attribute has been stripped by the 'RS_GenerateVertexFormatFromMeshMaterials' function. If it has, the remapped index will be 'RS_INVALIDATTRIBUTEINDEX'
        if (pMyVertexFormatData->IsAttributeUsed(originalAttributeIndex))
            vtx_attr_offsets[originalAttributeIndex] = pMyVertexFormatData->GetAttributeOffsetBytes(originalAttributeIndex);
        else
            vtx_attr_offsets[originalAttributeIndex] = 0xFFFF;	// an invalid offset (see AddTrianglesToMesh for usage)
    }

    unsigned int p_offset = vtx_attr_offsets[positionStreamOriginalIndex];
    unsigned int n_offset= vtx_attr_offsets[normalStreamOriginalIndex];

    mesh->SetAttributesFormat(pMyVertexFormatData);
    mesh->BeginPrimitives(1);

    char vtx_data[4][1024]; // temporary vertex buffer data
    GMathBbox3f bboxf(bbox.get_min(), bbox.get_max());

    //front
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_max()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_max()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_min()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_min()[1], bboxf.get_max()[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 0.0f, 1.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  0, 1, 2, 3, 0);
    // top
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_max()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_max()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_max()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_max()[1], bboxf.get_max()[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 1.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  4, 5, 6, 7, 0);
    // back
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_max()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_max()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_min()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_min()[1], bboxf.get_min()[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 0.0f, 1.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 0.0f, 1.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  8, 9, 10, 11, 0);
    // bottom
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_min()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_min()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_min()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_min()[1], bboxf.get_max()[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(0.0f, 1.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(0.0f, 1.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  12, 13, 14, 15, 0);
    // left
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_max()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_min()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_min()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(bboxf.get_min()[0], bboxf.get_max()[1], bboxf.get_min()[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  16, 17, 18, 19, 0);
    // right
    SetVertexData(vtx_data[0], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_max()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[1], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_min()[1], bboxf.get_max()[2]));
    SetVertexData(vtx_data[2], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_min()[1], bboxf.get_min()[2]));
    SetVertexData(vtx_data[3], p_offset, GMathVec3f(bboxf.get_max()[0], bboxf.get_max()[1], bboxf.get_min()[2]));

    SetVertexData(vtx_data[0], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[1], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[2], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));
    SetVertexData(vtx_data[3], n_offset, RSNormal(-1.0f, 0.0f, 0.0f));

    mesh->AddQuad(vtx_data[0], vtx_data[1], vtx_data[2], vtx_data[3],
                  0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,0.0f,0.0f,
                  20, 21, 22, 23, 0);


    mesh->CompactDataAndPrepareForRendering();
    RS_VertexData_Delete(pMyVertexFormatData);
    return mesh;
}


void
RedshiftUtils::CreateLight(const R2cSceneDelegate& delegate, R2cItemId lightid, RSLightInfo& light)
{
    // retreive the corresponding Clarisse light
    R2cItemDescriptor clight = delegate.get_render_item(lightid);

    //Create a light shader
    light.shader = RS_ShaderNode_Get(RedshiftUtils::get_new_unique_name(clight.get_item()->get_class_name()).get_data(), "Light" );	// "Light" is an RS Physical Light shader node
    if (light.shader != nullptr) {
        /* Physical light input parameters:

            INPUT	"on"					bool		STATIC	GUINAME("On")						DEFAULTVAL(true)
            INPUT	"color"					colorRGB			GUINAME("Color")					DEFAULTVAL(1.f,1.f,1.f)
            INPUT   "temperature"			float				GUINAME("Temperature")				DEFAULTVAL(6500.f)	SOFTLIMIT(1667.f,25000.f)
            INPUT   "colorMode"				int			STATIC	GUINAME("Mode")						DEFAULTVAL(0)		GUIENUM("Color",0,"Temperature",1)
            INPUT	"intensity"				float				GUINAME("Intensity Multiplier")		DEFAULTVAL(100.f)	SOFTLIMIT(0.f,10000.f)
            INPUT	"unitsType"				int			STATIC	GUINAME("Unit Type")				DEFAULTVAL(0)		GUIENUM("Image",0,"Luminous Power (lm)",1,"Luminance (cd/m^2)",2,"Radiant Power (W)",3,"Radiance (W/sr/m^2)",4)
            INPUT	"lumensperwatt"			float		STATIC	GUINAME("Luminous Efficacy (lm/w)")	DEFAULTVAL(17.f)
            INPUT	"decayType"				int			STATIC	GUINAME("Type")						DEFAULTVAL(0)		GUIENUM("Inverse-square",0,"None",1,"Linear",2)
            INPUT	"falloffStart"			wfloat		STATIC	GUINAME("Falloff Start")			DEFAULTVAL(0.0f)
            INPUT	"falloffStop"			wfloat		STATIC	GUINAME("Falloff Stop")				DEFAULTVAL(100.0f)
            INPUT	"shadow"				bool		STATIC	GUINAME("Enable")					DEFAULTVAL(true)
            INPUT	"shadowTransparency"	float				GUINAME("Transparency")				DEFAULTVAL(0.0f)	SOFTLIMIT(0.f,1.f)		HARDLIMIT(0.f,1.f)
            INPUT	"spotConeFalloffAngle"	float				GUINAME("Fall-off Angle")			DEFAULTVAL(5.0f)	SOFTLIMIT(0.f,180.f)	HARDLIMIT(0.f,180.f)

            // Maya Only
            INPUT	"dropoff"	float	DEFAULTVAL(0.f)

        */

        // Here, we'll just use the default light shader parameters (see above for the template), but below is an example of how to make the light green and turn off shadows:
        light.shader->SetParameterData("color", RSColor(0.01f, 0.01f, 0.01f ));
        light.shader->SetParameterData("decayType", 0);
        light.shader->SetParameterData("shadow",true);
    }

    //Create a point light
    light.ptr = RS_Light_New(clight.get_item()->get_name().get_data(), (clight.get_item()->get_name() + "_shader").get_data());
    const CoreString& classname = clight.get_item()->get_class_name();
    // Please look in RS_Light.h for list of types
    if (classname == "LightPhysicalSphere") {
        light.ptr->SetType("RS_LT_AREA");
        light.ptr->SetAreaGeometry("RS_LIGHTAREAGEOM_SPHERE");
        light.ptr->SetNormalizeEnabled(true);
        light.ptr->SetAreaScaling(RSVector3(0.05f, 0.05f, 0.05f));
    } else if (classname == "LightPhysicalDistant") {
        light.ptr->SetType("RS_LT_INFINITE");
        light.ptr->SetAreaGeometry("RS_LIGHTAREAGEOM_DISC");
        light.ptr->SetNormalizeEnabled(true);
    } else if (classname == "LightPhysicalPlane") {
        light.ptr->SetType("RS_LT_AREA");
        light.ptr->SetAreaGeometry("RS_LIGHTAREAGEOM_RECTANGLE");
        light.ptr->SetAreaScaling(RSVector3(1.0f, 1.0f, 1.0f));
        light.ptr->SetNormalizeEnabled(true);
    }
    light.ptr->SetBiDirectional(true);
    light.ptr->SetIsTransformationBlurred(false);

    // Lights don't use materials like meshes do, they reference light shaders directly
    light.ptr->SetDirectLightingShader(light.shader);
}

void
RedshiftUtils::CreateInstancer(RSInstancerInfo& instancer, RSResourceIndex& resources, const R2cSceneDelegate& delegate, RSScene *scene, R2cItemId cinstancer)
{
    R2cInstancer *instancer_info = delegate.create_instancer_description(cinstancer);
    // compressing source geometries sharing the same resources
    CoreArray<unsigned int> resource_remap(instancer_info->get_prototypes().get_count());
    CoreHashTable<R2cResourceId, unsigned int> resource_indices;
    unsigned int unique_resource_count = 0;

    for (unsigned int i = 0; i < instancer_info->get_prototypes().get_count(); i++) {
        // find the resource associated with the current prototype
        R2cItemId prototype_id = instancer_info->get_prototypes()[i];
        R2cResourceId resource = delegate.get_geometry_resource(prototype_id).get_id();
        unsigned int *idx = resource_indices.is_key_exists(resource);
        if (idx == nullptr) { // doesn't exist yet
            resource_indices.add(resource, unique_resource_count);
            resource_remap[i] = unique_resource_count;
            unique_resource_count++;
        } else {
            resource_remap[i] = *idx;
        }
    }

    instancer.resources.resize(unique_resource_count);
    // now that we have our number of unique prototypes we can create the point clouds
    // note: we might thing we could compress point clouds sharing the same mesh resource
    // but unfortunately because of the way redshift is handling materials in point clouds,
    // it is not possible.
    instancer.ptrs.resize(instancer_info->get_prototypes().get_count());
    // iterating through each actual resource of the instancer
    unsigned int idx = 0;
    // array to store the mesh for each resources since we will need it later
    CoreArray<RSMeshBase *> meshes(unique_resource_count);
    for (auto resource : resource_indices) {
        // check if the current resource exists in our resource index
        RSResourceInfo *stored_resource = resources.is_key_exists(resource.get_key());
        if (stored_resource == nullptr) { // the resource doesn't exists so let's create it
            // adding the new resource
            RSResourceInfo new_resource;
            new_resource.ptr = RedshiftUtils::CreateGeometry(delegate, instancer_info->get_prototypes()[resource.get_value()], RedshiftUtils::get_default_material(), new_resource.type);
            new_resource.refcount = 1;
            // adding the new resource
            resources.add(resource.get_key(), new_resource);
            // we need to add the new mesh
            scene->AddMesh(new_resource.ptr);
            meshes[idx] = new_resource.ptr;
        } else { // already exists
            meshes[idx] = stored_resource->ptr;
            stored_resource->refcount++;
        }
        instancer.resources[idx] = resource.get_key();
        idx++;
    }

    for (idx = 0; idx < instancer.ptrs.get_count(); idx++) {
        // generate its point cloud to instanciate the resource
        instancer.ptrs[idx] = RS_PointCloud_New();
        instancer.ptrs[idx]->SetIsTransformationBlurred(false);
        instancer.ptrs[idx]->SetPrimitiveType("RS_POINTCLOUDPRIMITIVETYPE_MESHINSTANCE");
        // assigning the correct ressource
        instancer.ptrs[idx]->SetInstanceTemplate(meshes[resource_remap[idx]]);
        instancer.ptrs[idx]->SetNumMaterials(meshes[resource_remap[idx]]->GetNumMaterials());
    }

    instancer.dirtiness = R2cSceneDelegate::DIRTINESS_KINEMATIC |
                          R2cSceneDelegate::DIRTINESS_SHADING_GROUP |
                          R2cSceneDelegate::DIRTINESS_VISIBILITY;

    const CoreArray<unsigned int>& indices = instancer_info->get_indices();
    const CoreArray<GMathMatrix4x4d>& matrices = instancer_info->get_matrices();
    // creating the actual points
    for (unsigned int i = 0; i < instancer.ptrs.get_count(); i++) instancer.ptrs[i]->BeginPrimitives(1);

    for (unsigned int i = 0; i < indices.get_count(); i++) {
        // getting the corresponding point cloud
        RSPointCloud *ptc = instancer.ptrs[indices[i]];
        // and adding the instance
        ptc->AddInstance(RedshiftUtils::ToRSMatrix4x4(matrices[i]));
    }
    // release instancer description since we don't need it anymore
    delegate.destroy_instancer_description(instancer_info);
    // finalizing point cloud data
    for (unsigned int i = 0; i < instancer.ptrs.get_count(); i++) instancer.ptrs[i]->CompactDataAndPrepareForRendering();
}

static
const GMathMatrix4x4d& get_scale_matrix()
{
    bool init = false;
    static GMathMatrix4x4d mx;
    if (!init) {
        mx.set_identity();
        mx.make_scaling(1.0,1.0,-1.0);
    }
    return mx;
}

RSMatrix4x4
RedshiftUtils::ToRSMatrix4x4(const GMathMatrix4x4d& m)
{
    GMathMatrix4x4d mx(m);
    mx.multiply_left(get_scale_matrix());
    mx.multiply_right(get_scale_matrix());
    return RSMatrix4x4(static_cast<float>(mx[0][0]), static_cast<float>(mx[1][0]), static_cast<float>(mx[2][0]), static_cast<float>(mx[3][0]),
                       static_cast<float>(mx[0][1]), static_cast<float>(mx[1][1]), static_cast<float>(mx[2][1]), static_cast<float>(mx[3][1]),
                       static_cast<float>(mx[0][2]), static_cast<float>(mx[1][2]), static_cast<float>(mx[2][2]), static_cast<float>(mx[3][2]),
                       static_cast<float>(mx[0][3]), static_cast<float>(mx[1][3]), static_cast<float>(mx[2][3]), static_cast<float>(mx[3][3]));
}


CoreString
RedshiftUtils::get_new_unique_name(const CoreString& prefix)
{
    static unsigned long long id = 0;
    CoreString result = "FromClarisse::";
    result += prefix;
    result += (++id);
    return result;
}

bool&
RS_is_initialized()
{
    static bool is_initialized = false;
    return is_initialized;
}

bool
RedshiftUtils::is_initialized()
{
    return RS_is_initialized();
}


// converts a Redshift parameter to a Clarisse attribute
bool
get_attribute_definition(const RSShaderInputParamInfo& input,
                         OfAttr::Type& type,
                         OfAttr::Container& container,
                         OfAttr::VisualHint& hint, unsigned int& size)
{
    switch (input.GetGUIType()) {
        // Basic types
        case RS_GUISHADERPARAMTYPE_COLOR_RGB:	// RSColor
            type = OfAttr::TYPE_DOUBLE;
            container = OfAttr::CONTAINER_ARRAY;
            hint = OfAttr::VISUAL_HINT_RGB;
            size = 3;
            break;
        case RS_GUISHADERPARAMTYPE_COLOR_RGBA:		// RSColor
            type = OfAttr::TYPE_DOUBLE;
            container = OfAttr::CONTAINER_ARRAY;
            hint = OfAttr::VISUAL_HINT_RGBA;
            size = 4;
            break;
        case RS_GUISHADERPARAMTYPE_FLOAT4:			// RSVector4
            type = OfAttr::TYPE_DOUBLE;
            container = OfAttr::CONTAINER_ARRAY;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 4;
            break;
        case RS_GUISHADERPARAMTYPE_FLOAT3:			// RSVector3
            type = OfAttr::TYPE_DOUBLE;
            container = OfAttr::CONTAINER_ARRAY;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 3;
            break;
        case RS_GUISHADERPARAMTYPE_FLOAT2:			// RSVector2
            type = OfAttr::TYPE_DOUBLE;
            container = OfAttr::CONTAINER_ARRAY;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 2;
            break;
        case RS_GUISHADERPARAMTYPE_FLOAT:			// float
            type = OfAttr::TYPE_DOUBLE;
            container = OfAttr::CONTAINER_SINGLE;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 1;
            break;
        case RS_GUISHADERPARAMTYPE_BOOL:				// bool
            type = OfAttr::TYPE_BOOL;
            container = OfAttr::CONTAINER_SINGLE;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 1;
            break;
        case RS_GUISHADERPARAMTYPE_INT:				// int
            type = OfAttr::TYPE_LONG;
            container = OfAttr::CONTAINER_SINGLE;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 1;
            break;
        case RS_GUISHADERPARAMTYPE_UINT4:			// RSUInt4
            type = OfAttr::TYPE_LONG;
            container = OfAttr::CONTAINER_SINGLE;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 4;
            break;

            // Resource types2
            // Other
        case RS_GUISHADERPARAMTYPE_STRING:			// RSString
        case RS_GUISHADERPARAMTYPE_AOVNAME:			// RSString
        case RS_GUISHADERPARAMTYPE_LAYERNAME:		// RSString
            type = OfAttr::TYPE_STRING;
            container = OfAttr::CONTAINER_SINGLE;
            hint = OfAttr::VISUAL_HINT_DEFAULT;
            size = 1;
            break;

        // unsupported attributes for now
            // Resource types
            // Texture/Color map space
        case RS_GUISHADERPARAMTYPE_TSPACE:			// Use with AddVertexAttributeMeshAssociation()
        case RS_GUISHADERPARAMTYPE_CSPACE:			// Use with AddVertexAttributeMeshAssociation()
            // Special shader node connections only
        case RS_GUISHADERPARAMTYPE_UV:				// For connecting a UV projection node
        case RS_GUISHADERPARAMTYPE_UVW:				// For connecting a UVW projection node
        case RS_GUISHADERPARAMTYPE_COLORRAMP:		// RSCurve* (where curve type is RS_CURVETYPE_COLOR)
        case RS_GUISHADERPARAMTYPE_CAMERAPICKER:		// requires custom call-back code (a string or node picker in the GUI)
        case RS_GUISHADERPARAMTYPE_CURVE:			// RSCurve*
        case RS_GUISHADERPARAMTYPE_TEXTURE:			// RSTexture*, RSUDIMTexture*, RSUVTILETexture*	(a filename string in the GUI)
        case RS_GUISHADERPARAMTYPE_LIGHT:			// RSLight*
        case RS_GUISHADERPARAMTYPE_MATRIX4X4:		// RSMatrix4x4
        default:
            return false;
    }
    return true;
}

void
register_attribute(OfClass& cls, const RSShaderInputParamInfo *input, const CoreString& category)
{
    OfAttr::Type type;
    OfAttr::Container container;
    OfAttr::VisualHint hint;
    unsigned int size;

    if (get_attribute_definition(*input, type, container, hint, size)) {

        OfAttr *attr = cls.attribute_exists(input->GetInternalName());
        if (attr == nullptr) {
            attr = cls.add_attribute(input->GetInternalName(), type, container, hint, category);
            attr->set_value_count(size);
            // todo set attribute description
            // setting default value
            if (attr->is_numeric_type()) {
                if (attr->get_type() == OfAttr::TYPE_DOUBLE) {
                    float value, vmin, vmax;
                    for (unsigned int i = 0; i < attr->get_value_count(); i++) {
                        input->GetDefaultValue(static_cast<int>(i), value);
                        attr->set_double(static_cast<double>(value), i);
                    }
                    if (input->GetHardMinMaxValue(vmin, vmax)) {
                       attr->set_numeric_range(static_cast<double>(vmin), static_cast<double>(vmax));
                       attr->enable_range(true);
                    }
                    if (input->GetSoftMinMaxValue(vmin, vmax)) {
                        attr->set_numeric_ui_range(static_cast<double>(vmin), static_cast<double>(vmax));
                        attr->enable_ui_range(true);
                    }
                } else {
                    int value, vmin, vmax;
                    for (unsigned int i = 0; i < attr->get_value_count(); i++) {
                        input->GetDefaultValue(static_cast<int>(i), value);
                        attr->set_long(static_cast<long>(value), i);
                    }
                    if (input->GetHardMinMaxValue(vmin, vmax)) {
                       attr->set_numeric_range(static_cast<double>(vmin), static_cast<double>(vmax));
                       attr->enable_range(true);
                    }
                    if (input->GetSoftMinMaxValue(vmin, vmax)) {
                        attr->set_numeric_ui_range(static_cast<double>(vmin), static_cast<double>(vmax));
                        attr->enable_ui_range(true);
                    }
                }
                attr->set_animatable(true);
                // FIXME: will need to set the RSTexture filter
                attr->set_texturable(input->IsTexturable());
                attr->set_slider(input->IsLogarithmicSlider());
            }
        } else {
            LOG_WARNING("Failed to add " << cls.get_name() << "::" << input->GetInternalName() << " since it already exists!\n");
        }
    } // else unsupported
}

CoreString
make_category(const CoreString& tab, const CoreVector<CoreString>& subtabs)
{
    CoreString result = tab;
    for (unsigned int i = 0; i < subtabs.get_count(); i++) {
        result += ">";
        result += subtabs[i];
    }
    return result;
}

void
register_attributes(OfClass& cls, const RSShaderGUIInfo& shader)
{
    CoreString category;
    int paramidx = 0;
    CoreString tab;
    CoreVector<CoreString> subtabs;

    for (int i = 0; i < shader.GetNumGUILayoutProperties(); i++) {
        const RSGUILayoutProperty *property = shader.GetGUILayoutProperty(i);
        if (property->GetPropertyType() == RS_GUIPROPERTYTYPE_TAB) {
            tab = property->GetPropertyName();
        } else if (property->GetPropertyType() == RS_GUIPROPERTYTYPE_SUBTAB_BEGIN) {
            subtabs.add(property->GetPropertyName());
        } else if (property->GetPropertyType() == RS_GUIPROPERTYTYPE_SUBTAB_END) {
            subtabs.remove_last();
        } else if (property->GetPropertyType() == RS_GUIPROPERTYTYPE_PARAMBLOCK) {
            CoreString category = make_category(tab, subtabs);
            for (int j = 0; j < property->GetNumParamBlockEntries(); j++) {
                const RSShaderInputParamInfo *input = shader.GetInputParameterInfo(paramidx++);
                register_attribute(cls, input, category);
            }
        }
    }
}

bool
register_shader(OfApp& application, OfClass& redshift_class, const CoreString& new_class, const RSShaderGUIInfo& shader)
{
    OfClass *cls = application.get_factory().get_classes().add(new_class, redshift_class.get_name());
    cls->set_callbacks(redshift_class.get_callbacks());
    register_attributes(*cls, shader);
    return true;
}

void register_shaders(OfApp& application)
{
    static OfClass *redshift_material = application.get_factory().get_classes().get("MaterialRedshift");
    //static OfClass *redshift_light = application.get_factory().get_classes().get("LightRedshift");

    CoreSet<CoreString> deprecated_materials;
    // populating deprecated materials to avoid exposing them
    deprecated_materials.add("Architectural");
    deprecated_materials.add("MatteShadow");

    static CoreSet<CoreString> deprecated_lights;
    CoreString cls_name;

    RSShaderPackageGUIInfo *shader_info = RS_ShaderPackageInfo_New("houdini");
    for (int i = 0; i < shader_info->GetNumShaders(); i++) {
        const RSShaderGUIInfo *shader = shader_info->GetShaderInfo(i);
        if (shader != nullptr) {
            switch (shader->GetType()) {
                case RS_GUISHADERTYPE_MATERIAL:
                    if (redshift_material != nullptr && !deprecated_materials.exists(shader->GetName())) {
                        cls_name = ModuleMaterialRedshift::mangle_class(shader->GetName());
                        register_shader(application, *redshift_material, cls_name, *shader);
                    }
                    break;
//                case RS_GUISHADERTYPE_LIGHT:
//                    for (int j =0 ; j < shader->GetNumInputParameters(); j++) {
//                        if (shader->GetInputParameterInfo(j)->IsVisibleInGUI()) {
//                            LOG_INFO(shader->GetName() << "::" << shader->GetInputParameterInfo(j)->GetInternalName() << " " << shader->GetInputParameterInfo(j)->GetPrettyName() << '\n');
//                        }
//                    }
//                    if (redshift_light != nullptr) {
//                        cls_name = ModuleLightRedshift::mangle_class(shader->GetName());
//                        register_shader(application, *redshift_light, cls_name, *shader, deprecated_lights);
//                    }
//                    break;
                default:
                    break;
            }
        }
    }
    RS_ShaderPackageInfo_Delete(shader_info);
}

bool
RedshiftUtils::initialize(OfApp& application)
{
    if (!is_initialized()) {
        int rs_major, rs_minor, rs_build;
        RS_Renderer_GetVersion(rs_major, rs_minor, rs_build);
        LOG_INFO("Registered Redshift module (version " << rs_major << '.' << rs_minor << '.' << rs_build << ")\n");

        RSString pathConfigPath, coreDataPath, localDataPath, proceduralsPath, prefsFileFullPath, licensePath;
        RS_Renderer_ObtainNecessaryPaths(pathConfigPath, coreDataPath, localDataPath, proceduralsPath, prefsFileFullPath, licensePath);

        RS_Log_Init(localDataPath);
        // redirecting output to Clarisse Log
#if CLARISSE_SINK
        RS_Log_AddSink(new ClarisseLogSink);
#endif
        RS_Renderer_SetPreferencesFilePath(prefsFileFullPath);
        RS_Renderer_Init();
        RS_Renderer_SetCoreDataPath(coreDataPath);
        RS_Renderer_SetLicensePath(licensePath);
        RS_Renderer_SetProceduralPath(proceduralsPath);

        unsigned int textureCacheBudgetMB = static_cast<unsigned int>(RS_Renderer_GetDefaultTextureCacheBudgetGB() * 1024);
        if (const char* var = getenv("REDSHIFT_TEXTURECACHEBUDGET")) {
            int textureCacheBudgetGB_query = atoi(var);
            RSLogMessage(Debug, "Querying texture cache buget from REDSHIFT_TEXTURECACHEBUDGET: %d GB", textureCacheBudgetGB_query);
            textureCacheBudgetMB = static_cast<unsigned int>(textureCacheBudgetGB_query * 1024);
        } else {
            int textureCacheBudgetGB_query;
            RS_Renderer_GetPreferenceValue("TextureCacheBudgetGB", textureCacheBudgetGB_query, RS_Renderer_GetDefaultTextureCacheBudgetGB());
            textureCacheBudgetMB = static_cast<unsigned int>(textureCacheBudgetGB_query * 1024);
            RSLogMessage(Debug, "Querying texture cache buget from preferences.xml: %d GB", textureCacheBudgetGB_query);
        }

        RSString cacheFolder_query;
        if (const char* var = getenv("REDSHIFT_CACHEPATH")) {
            cacheFolder_query = var;
            RSLogMessage(Debug, "Querying cache path from REDSHIFT_CACHEPATH: %s", static_cast<const char *>(cacheFolder_query));
        } else {
            RS_Renderer_GetPreferenceValue("CacheFolder", cacheFolder_query, RS_Renderer_GetDefaultCacheFolder());
            RSLogMessage(Debug, "Querying cache path from preferences.xml: %s", static_cast<const char *>(cacheFolder_query));
        }

        RS_Renderer_SetCachePath(static_cast<const char *>(cacheFolder_query), textureCacheBudgetMB);

        // initialize the redshift renderer
        RSArray<int> selectedCudaDeviceOrdinals;
        RS_Renderer_GetSelectedCudaDeviceOrdinalsFromPreferences(selectedCudaDeviceOrdinals);
        RS_Renderer_Create(static_cast<unsigned int>(selectedCudaDeviceOrdinals.Length()), static_cast<int *>(&selectedCudaDeviceOrdinals[0]));

        // register Redshift shaders to Clarisse
        register_shaders(application);

        RS_is_initialized() = true;
        return true;
    }
    LOG_ERROR("Redshift was already initialized!\n");
    return false;
}

static
RSMaterial *create_default_material()
{
    RSShaderNode *shader = RS_ShaderNode_Get("__clarisse_default_material__shader_node__", "Material" );
    if (shader != nullptr) {
        shader->BeginUpdate();
        shader->SetParameterData("diffuse", RSColor(0.5f, 0.5f, 0.5f));
        shader->SetParameterData("refl_roughness", 0.3f);
        shader->SetParameterData("refl_brdf", 1);
        shader->EndUpdate();
    }
    RSMaterial *default_material = RS_Material_Get("__clarisse_default_material__");
    default_material->SetSurfaceShaderNodeGraph(shader);

    RS_ShaderNode_Release(shader);
    return default_material;
}

RSMaterial *
RedshiftUtils::get_default_material()
{
    static RSMaterial *default_material = create_default_material();
    return default_material;
}

bool
RenderingAbortChecker::ShouldAbort()
{
    return m_application.must_stop_evaluation();
}
