/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2021, Blender Foundation.
 */

#include "draw_subdivision.h"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_editmesh.h"
#include "BKE_modifier.h"
#include "BKE_scene.h"
#include "BKE_subdiv.h"
#include "BKE_subdiv_eval.h"
#include "BKE_subdiv_foreach.h"
#include "BKE_subdiv_mesh.h"

#include "BLI_linklist.h"

#include "BLI_string.h"

#include "PIL_time.h"

#include "DRW_engine.h"
#include "DRW_render.h"

#include "GPU_capabilities.h"
#include "GPU_compute.h"
#include "GPU_index_buffer.h"
#include "GPU_state.h"
#include "GPU_vertex_buffer.h"

#include "opensubdiv_capi.h"
#include "opensubdiv_capi_type.h"
#include "opensubdiv_converter_capi.h"
#include "opensubdiv_evaluator_capi.h"
#include "opensubdiv_topology_refiner_capi.h"

#include "draw_cache_extract.h"
#include "draw_cache_extract_mesh_private.h"
#include "draw_cache_impl.h"
#include "draw_cache_inline.h"

#if 0
static void print_index_buffer(const char *message, int *index, int len)
{
  fprintf(stderr, "%s\n", message);
  for (int i = 0; i < len; i++) {
    fprintf(stderr, "index at %d: %d\n", i, index[i]);
  }
}
#endif

/* To-dos:
 * - improve OpenSubdiv_BufferInterface
 * - comments
 * - better structure for this file
 *
 * - fix edit mode rendering (face dot, snap to cage, etc.)
 * - (long term) add support for vertex colors
 *
 * - holes: deduplicate operator code in edit_mesh_tools.
 */

extern char datatoc_common_subdiv_lib_glsl[];
extern char datatoc_common_subdiv_buffer_lines_comp_glsl[];
extern char datatoc_common_subdiv_buffer_lnor_comp_glsl[];
extern char datatoc_common_subdiv_buffer_edge_fac_comp_glsl[];
extern char datatoc_common_subdiv_buffer_points_comp_glsl[];
extern char datatoc_common_subdiv_tris_comp_glsl[];
extern char datatoc_common_subdiv_normals_accumulate_comp_glsl[];
extern char datatoc_common_subdiv_normals_finalize_comp_glsl[];
extern char datatoc_common_subdiv_patch_evaluation_comp_glsl[];

enum {
  SHADER_BUFFER_LINES,
  SHADER_BUFFER_EDGE_FAC,
  SHADER_BUFFER_LNOR,
  SHADER_BUFFER_TRIS,
  SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS,
  SHADER_BUFFER_NORMALS_ACCUMULATE,
  SHADER_BUFFER_NORMALS_FINALIZE,
  SHADER_PATCH_EVALUATION,
  SHADER_PATCH_EVALUATION_LIMIT_NORMALS,
  SHADER_PATCH_FVAR_EVALUATION,
  SHADER_PATCH_EVALUATION_FACE_DOTS,

  NUM_SHADERS,
};

static GPUShader *g_subdiv_shaders[NUM_SHADERS];

static const char *get_shader_code(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_LINES: {
      return datatoc_common_subdiv_buffer_lines_comp_glsl;
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return datatoc_common_subdiv_buffer_edge_fac_comp_glsl;
    }
    case SHADER_BUFFER_LNOR: {
      return datatoc_common_subdiv_buffer_lnor_comp_glsl;
    }
    case SHADER_BUFFER_TRIS:
    case SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS: {
      return datatoc_common_subdiv_tris_comp_glsl;
    }
    case SHADER_BUFFER_NORMALS_ACCUMULATE: {
      return datatoc_common_subdiv_normals_accumulate_comp_glsl;
    }
    case SHADER_BUFFER_NORMALS_FINALIZE: {
      return datatoc_common_subdiv_normals_finalize_comp_glsl;
    }
    case SHADER_PATCH_EVALUATION:
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS:
    case SHADER_PATCH_FVAR_EVALUATION:
    case SHADER_PATCH_EVALUATION_FACE_DOTS: {
      return datatoc_common_subdiv_patch_evaluation_comp_glsl;
    }
  }
  return NULL;
}

static const char *get_shader_name(int shader_type)
{
  switch (shader_type) {
    case SHADER_BUFFER_LINES: {
      return "subdiv lines build";
    }
    case SHADER_BUFFER_LNOR: {
      return "subdiv lnor build";
    }
    case SHADER_BUFFER_EDGE_FAC: {
      return "subdiv edge fac build";
    }
    case SHADER_BUFFER_TRIS:
    case SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS: {
      return "subdiv tris";
    }
    case SHADER_BUFFER_NORMALS_ACCUMULATE: {
      return "subdiv normals accumulate";
    }
    case SHADER_BUFFER_NORMALS_FINALIZE: {
      return "subdiv normals finalize";
    }
    case SHADER_PATCH_EVALUATION:
    case SHADER_PATCH_EVALUATION_LIMIT_NORMALS:
    case SHADER_PATCH_FVAR_EVALUATION:
    case SHADER_PATCH_EVALUATION_FACE_DOTS: {
      return "subdiv patch evaluation";
    }
  }
  return NULL;
}

static GPUShader *get_patch_evaluation_shader(int shader_type)
{
  if (g_subdiv_shaders[shader_type] == NULL) {
    const char *compute_code = get_shader_code(shader_type);

    const char *defines = NULL;
    if (shader_type == SHADER_PATCH_EVALUATION) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n";
    }
    else if (shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define LIMIT_NORMALS\n";
    }
    else if (shader_type == SHADER_PATCH_FVAR_EVALUATION) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define FVAR_EVALUATION\n";
    }
    else if (shader_type == SHADER_PATCH_EVALUATION_FACE_DOTS) {
      defines =
          "#define OSD_PATCH_BASIS_GLSL\n"
          "#define OPENSUBDIV_GLSL_COMPUTE_USE_1ST_DERIVATIVES\n"
          "#define FDOTS_EVALUATION\n";
    }

    /* Merge OpenSubdiv library code with our own library code. */
    const char *patch_basis_source = openSubdiv_getGLSLPatchBasisSource();
    const char *subdiv_lib_code = datatoc_common_subdiv_lib_glsl;
    char *library_code = MEM_mallocN(strlen(patch_basis_source) + strlen(subdiv_lib_code) + 1,
                                     "subdiv patch evaluation library code");
    library_code[0] = '\0';
    strcat(library_code, patch_basis_source);
    strcat(library_code, subdiv_lib_code);

    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, library_code, defines, get_shader_name(shader_type));

    MEM_freeN(library_code);
  }

  return g_subdiv_shaders[shader_type];
}

static GPUShader *get_subdiv_shader(int shader_type, const char *defines)
{
  if (shader_type == SHADER_PATCH_EVALUATION ||
      shader_type == SHADER_PATCH_EVALUATION_LIMIT_NORMALS) {
    return get_patch_evaluation_shader(shader_type);
  }
  if (g_subdiv_shaders[shader_type] == NULL) {
    const char *compute_code = get_shader_code(shader_type);
    g_subdiv_shaders[shader_type] = GPU_shader_create_compute(
        compute_code, datatoc_common_subdiv_lib_glsl, defines, get_shader_name(shader_type));
  }
  return g_subdiv_shaders[shader_type];
}

typedef struct VertexBufferData {
  float co[4];
  float no[4];
} VertexBufferData;

/* Vertex format used for rendering the result; corresponds to the VertexBufferData struct above.
 */
static GPUVertFormat *get_render_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* WARNING Adjust #VertexBufferData struct in common_subdiv_lib accordingly.
     * We use 4 components for the vectors to account for padding in the compute shaders, where
     * vec3 is promoted to vec4. */
    GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    GPU_vertformat_alias_add(&format, "vnor");
  }
  return &format;
}

/* Vertex format for OpenSubdiv::Osd::PatchArray. */
static GPUVertFormat *get_patch_array_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_F32, 6, GPU_FETCH_FLOAT);
  }
  return &format;
}

static GPUVertFormat *get_uvs_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "uvs", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format for OpenSubdiv::Osd::PatchParam, not really used, it is only for making sure that
 * the GPUVertBuf used to wrap the OpenSubdiv patch param buffer is valid. */
static GPUVertFormat *get_patch_param_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  }
  return &format;
}

/* Vertex format for the patches' vertices index buffer. */
static GPUVertFormat *get_patch_index_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }
  return &format;
}

/* Vertex format for the OpenSubdiv vertex buffer. */
static GPUVertFormat *get_work_vertex_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* We use 4 components for the vectors to account for padding in the compute shaders, where
     * vec3 is promoted to vec4. */
    GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }
  return &format;
}

typedef struct CompressedPatchCoord {
  int ptex_face_index;
  /* UV coordinate encoded as u << 16 | v, where u and v are quantized on 16-bits. */
  unsigned int encoded_uv;
} CompressedPatchCoord;

MINLINE CompressedPatchCoord make_patch_coord(int ptex_face_index, float u, float v)
{
  CompressedPatchCoord patch_coord = {
      .ptex_face_index = ptex_face_index,
      .encoded_uv = ((unsigned int)(u * 65535.0f) << 16) | (unsigned int)(v * 65535.0f),
  };
  return patch_coord;
}

/* Vertex format used for the #CompressedPatchCoord. */
static GPUVertFormat *get_blender_patch_coords_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    /* WARNING! Adjust #CompressedPatchCoord accordingly. */
    GPU_vertformat_attr_add(&format, "ptex_face_index", GPU_COMP_U32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "uv", GPU_COMP_U32, 1, GPU_FETCH_INT);
  }
  return &format;
}

/* Vertex format used for the PatchTable::PatchHandle. */
static GPUVertFormat *get_patch_handle_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "vertex_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "array_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
    GPU_vertformat_attr_add(&format, "patch_index", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }
  return &format;
}

/* Vertex format used for the quadtree nodes of the PatchMap. */
static GPUVertFormat *get_quadtree_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "child", GPU_COMP_U32, 4, GPU_FETCH_INT);
  }
  return &format;
}

static GPUVertFormat *get_edge_fac_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "wd", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  }
  return &format;
}

static GPUVertFormat *get_lnor_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
    GPU_vertformat_alias_add(&format, "lnor");
  }
  return &format;
}

static GPUVertFormat *get_fdots_pos_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }
  return &format;
}

static GPUVertFormat *get_fdots_nor_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "nor", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);
  }
  return &format;
}

static GPUVertFormat *get_edit_data_format(void)
{
  static GPUVertFormat format = {0};
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "data", GPU_COMP_U16, 4, GPU_FETCH_INT);
    GPU_vertformat_alias_add(&format, "flag");
  }
  return &format;
}

static GPUVertFormat *get_origindex_format(void)
{
  static GPUVertFormat format;
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "color", GPU_COMP_U32, 1, GPU_FETCH_INT);
  }
  return &format;
}

// --------------------------------------------------------

static uint tris_count_from_number_of_loops(const uint number_of_loops)
{
  const uint32_t number_of_quads = number_of_loops / 4;
  return number_of_quads * 2;
}

// --------------------------------------------------------

static uint vertbuf_bind_gpu(OpenSubdiv_BufferInterface *buffer)
{
  GPUVertBuf *verts = (GPUVertBuf *)(buffer->data);
  GPU_vertbuf_use(verts);
  return GPU_vertbuf_get_device_ptr(verts);
}

static void *vertbuf_alloc(OpenSubdiv_BufferInterface *interface, const uint len)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  GPU_vertbuf_data_alloc(verts, len);
  return GPU_vertbuf_get_data(verts);
}

static void vertbuf_device_alloc(OpenSubdiv_BufferInterface *interface, const uint len)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  /* This assumes that GPU_USAGE_DEVICE_ONLY was used, which won't allocate host memory. */
  BLI_assert(GPU_vertbuf_get_memory_usage() == GPU_USAGE_DEVICE_ONLY);
  GPU_vertbuf_data_alloc(verts, len);
}

static int vertbuf_num_vertices(OpenSubdiv_BufferInterface *interface)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  return GPU_vertbuf_get_vertex_len(verts);
}

static void vertbuf_wrap(OpenSubdiv_BufferInterface *interface, uint device_ptr)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  GPU_vertbuf_wrap_device_ptr(verts, device_ptr);
}

static void vertbuf_update_data(OpenSubdiv_BufferInterface *interface,
                                uint start,
                                uint len,
                                const void *data)
{
  GPUVertBuf *verts = (GPUVertBuf *)(interface->data);
  GPU_vertbuf_update_sub(verts, start, len, data);
}

static void opensubdiv_gpu_buffer_init(OpenSubdiv_BufferInterface *buffer_interface,
                                       GPUVertBuf *vertbuf)
{
  buffer_interface->data = vertbuf;
  buffer_interface->bind = vertbuf_bind_gpu;
  buffer_interface->alloc = vertbuf_alloc;
  buffer_interface->num_vertices = vertbuf_num_vertices;
  buffer_interface->buffer_offset = 0;
  buffer_interface->wrap = vertbuf_wrap;
  buffer_interface->device_alloc = vertbuf_device_alloc;
  buffer_interface->update_data = vertbuf_update_data;
}

// --------------------------------------------------------

static void initialize_uv_buffer(GPUVertBuf *uvs,
                                 struct MeshBatchCache *cache,
                                 CustomData *cd_ldata,
                                 uint v_len,
                                 uint *r_uv_layers)
{
  GPUVertFormat format = {0};

  if (!mesh_extract_uv_format_init(&format, cache, cd_ldata, MR_EXTRACT_MESH, r_uv_layers)) {
    // TODO(kevindietrich): handle this more gracefully.
    v_len = 1;
  }

  GPU_vertbuf_init_build_on_device(uvs, &format, v_len);
}

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
 * \{ */

static void init_origindex_buffer(GPUVertBuf *buffer,
                                  int *vert_origindex,
                                  uint num_loops,
                                  uint loose_len)
{
  GPU_vertbuf_init_with_format_ex(buffer, get_origindex_format(), GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(buffer, num_loops + loose_len);

  int *vbo_data = (int *)GPU_vertbuf_get_data(buffer);
  memcpy(vbo_data, vert_origindex, num_loops * sizeof(int));
}

static struct GPUVertBuf *build_origindex_buffer(int *vert_origindex, uint num_loops)
{
  GPUVertBuf *buffer = GPU_vertbuf_calloc();
  init_origindex_buffer(buffer, vert_origindex, num_loops, 0);
  return buffer;
}

static struct GPUVertBuf *build_flags_buffer(int *vert_origindex, uint num_loops)
{
  GPUVertBuf *buffer = GPU_vertbuf_calloc();

  static GPUVertFormat format;
  if (format.attr_len == 0) {
    GPU_vertformat_attr_add(&format, "flags", GPU_COMP_I32, 1, GPU_FETCH_INT);
  }

  GPU_vertbuf_init_with_format_ex(buffer, &format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(buffer, num_loops);

  int *vbo_data = (int *)GPU_vertbuf_get_data(buffer);

  for (int i = 0; i < num_loops; ++i) {
    vbo_data[i] = vert_origindex[i];
  }

  return buffer;
}

/** \} */

typedef struct GPUPatchMap {
  GPUVertBuf *patch_map_handles;
  GPUVertBuf *patch_map_quadtree;
  int min_patch_face;
  int max_patch_face;
  int max_depth;
  int patches_are_triangular;
} GPUPatchMap;

static void gpu_patch_map_build(GPUPatchMap *gpu_patch_map, Subdiv *subdiv)
{
  GPUVertBuf *patch_map_handles = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(patch_map_handles, get_patch_handle_format(), GPU_USAGE_STATIC);

  GPUVertBuf *patch_map_quadtree = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(patch_map_quadtree, get_quadtree_format(), GPU_USAGE_STATIC);

  OpenSubdiv_BufferInterface patch_map_handles_interface;
  opensubdiv_gpu_buffer_init(&patch_map_handles_interface, patch_map_handles);

  OpenSubdiv_BufferInterface patch_map_quad_tree_interface;
  opensubdiv_gpu_buffer_init(&patch_map_quad_tree_interface, patch_map_quadtree);

  int min_patch_face = 0;
  int max_patch_face = 0;
  int max_depth = 0;
  int patches_are_triangular = 0;

  OpenSubdiv_Evaluator *evaluator = subdiv->evaluator;
  evaluator->getPatchMap(evaluator,
                         &patch_map_handles_interface,
                         &patch_map_quad_tree_interface,
                         &min_patch_face,
                         &max_patch_face,
                         &max_depth,
                         &patches_are_triangular);

  gpu_patch_map->patch_map_handles = patch_map_handles;
  gpu_patch_map->patch_map_quadtree = patch_map_quadtree;
  gpu_patch_map->min_patch_face = min_patch_face;
  gpu_patch_map->max_patch_face = max_patch_face;
  gpu_patch_map->max_depth = max_depth;
  gpu_patch_map->patches_are_triangular = patches_are_triangular;
}

static void gpu_patch_map_free(GPUPatchMap *gpu_patch_map)
{
  GPU_VERTBUF_DISCARD_SAFE(gpu_patch_map->patch_map_handles);
  GPU_VERTBUF_DISCARD_SAFE(gpu_patch_map->patch_map_quadtree);
  gpu_patch_map->min_patch_face = 0;
  gpu_patch_map->max_patch_face = 0;
  gpu_patch_map->max_depth = 0;
  gpu_patch_map->patches_are_triangular = 0;
}

#include "BLI_bitmap.h"
#include "BLI_memarena.h"

typedef struct LooseVertex {
  struct LooseVertex *next;
  int coarse_vertex_index;
  int subdiv_vertex_index;
  float co[3];
} LooseVertex;

typedef struct LooseEdge {
  struct LooseEdge *next;
  uint v1;
  uint v2;
  int coarse_edge_index;
} LooseEdge;

/* -------------------------------------------------------------------- */
/** \name DRWSubdivCache
 * \{ */

typedef struct DRWSubdivCache {
  /* Coordinates used to evaluate patches, for UVs, positions, and normals. */
  GPUVertBuf *patch_coords;
  GPUVertBuf *fdots_patch_coords;

  /* Resolution used to generate the patch coordinates. */
  int resolution;

  /* Number of coordinantes. */
  uint num_patch_coords;

  int coarse_poly_count;
  int num_vertices;  // subdiv_vertex_count;

  uint num_edges;

  /* Maps subdivision loop to original coarse vertex index. */
  int *subdiv_loop_vert_index;
  /* Maps subdivision loop to subdivided vertex index. */
  int *subdiv_loop_subdiv_vert_index;
  /* Maps subdivision loop to original coarse edge index. */
  int *subdiv_loop_edge_index;
  /* Maps subdivision loop to original coarse poly index. */
  int *subdiv_loop_poly_index;

  /* Indices of faces adjacent to the vertices, ordered by vertex index, with no particular
   * winding. */
  GPUVertBuf *subdiv_vertex_face_adjacency;
  /* The difference between value (i + 1) and (i) gives the number of faces adjacent to vertex (i).
   */
  GPUVertBuf *subdiv_vertex_face_adjacency_offsets;

  /* Maps to original element in the coarse mesh, only for edit mode. */
  GPUVertBuf *verts_orig_index;
  GPUVertBuf *faces_orig_index;
  GPUVertBuf *edges_orig_index;

  int *face_ptex_offset;
  GPUVertBuf *subdiv_polygon_offset_buffer;
  int *subdiv_polygon_offset;

  GPUVertBuf *face_flags;

  int *point_indices;

  /* Material offsets. */
  int *mat_start;
  int *mat_end;
  GPUVertBuf *polygon_mat_offset;

  GPUPatchMap gpu_patch_map;

  /* Loose Vertices and Edges */
  MemArena *loose_memarena;
  LooseVertex *loose_verts;
  LooseEdge *loose_edges;

  int vert_loose_len;
  int edge_loose_len;
  int loop_loose_len;
} DRWSubdivCache;

static void draw_subdiv_cache_free_material_data(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->polygon_mat_offset);
  MEM_SAFE_FREE(cache->mat_start);
  MEM_SAFE_FREE(cache->mat_end);
}

static void draw_free_edit_mode_cache(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->verts_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->edges_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->faces_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->fdots_patch_coords);
}

static void draw_subdiv_cache_free(DRWSubdivCache *cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache->patch_coords);
  GPU_VERTBUF_DISCARD_SAFE(cache->subdiv_polygon_offset_buffer);
  GPU_VERTBUF_DISCARD_SAFE(cache->face_flags);
  MEM_SAFE_FREE(cache->subdiv_loop_edge_index);
  MEM_SAFE_FREE(cache->subdiv_loop_subdiv_vert_index);
  MEM_SAFE_FREE(cache->subdiv_loop_poly_index);
  MEM_SAFE_FREE(cache->subdiv_loop_vert_index);
  MEM_SAFE_FREE(cache->point_indices);
  MEM_SAFE_FREE(cache->face_ptex_offset);
  MEM_SAFE_FREE(cache->subdiv_polygon_offset);
  GPU_VERTBUF_DISCARD_SAFE(cache->subdiv_vertex_face_adjacency_offsets);
  GPU_VERTBUF_DISCARD_SAFE(cache->subdiv_vertex_face_adjacency);
  cache->resolution = 0;
  cache->num_patch_coords = 0;
  cache->coarse_poly_count = 0;
  draw_free_edit_mode_cache(cache);
  draw_subdiv_cache_free_material_data(cache);
  gpu_patch_map_free(&cache->gpu_patch_map);

  cache->edge_loose_len = 0;
  cache->vert_loose_len = 0;
  cache->loop_loose_len = 0;
  cache->loose_verts = NULL;
  cache->loose_edges = NULL;
  if (cache->loose_memarena) {
    BLI_memarena_free(cache->loose_memarena);
    cache->loose_memarena = NULL;
  }
}

static void draw_subdiv_cache_update_face_flags(DRWSubdivCache *cache, Mesh *mesh)
{
  if (cache->face_flags == NULL) {
    cache->face_flags = GPU_vertbuf_calloc();
    static GPUVertFormat format;
    if (format.attr_len == 0) {
      GPU_vertformat_attr_add(&format, "flag", GPU_COMP_U32, 1, GPU_FETCH_INT);
    }
    GPU_vertbuf_init_with_format_ex(cache->face_flags, &format, GPU_USAGE_DYNAMIC);
    GPU_vertbuf_data_alloc(cache->face_flags, mesh->totpoly);
  }

  uint32_t *flags_data = (uint32_t *)(GPU_vertbuf_get_data(cache->face_flags));

  for (int i = 0; i < mesh->totpoly; i++) {
    uint8_t flag = 0;
    if ((mesh->mpoly[i].flag & ME_SMOOTH) != 0) {
      flag = 1;
    }
    flags_data[i] = flag;
  }

  /* Make sure updated data is reuploaded. */
  GPU_vertbuf_tag_dirty(cache->face_flags);
}

static void draw_subdiv_cache_print_memory_used(DRWSubdivCache *cache)
{
  size_t memory_used = 0;

  if (cache->patch_coords) {
    memory_used += cache->num_patch_coords * 8;
  }

  if (cache->subdiv_polygon_offset_buffer) {
    memory_used += cache->coarse_poly_count * sizeof(int);
  }

  if (cache->face_flags) {
    memory_used += cache->coarse_poly_count * sizeof(int);
  }

  if (cache->subdiv_loop_edge_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->subdiv_loop_subdiv_vert_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->subdiv_loop_poly_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->subdiv_loop_vert_index) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

  if (cache->point_indices) {
    memory_used += cache->num_vertices * sizeof(int);
  }

  if (cache->face_ptex_offset) {
    memory_used += cache->coarse_poly_count * sizeof(int);
  }

  if (cache->subdiv_vertex_face_adjacency_offsets) {
    memory_used += cache->num_vertices * sizeof(int);
  }

  if (cache->subdiv_vertex_face_adjacency) {
    memory_used += cache->num_patch_coords * sizeof(int);
  }

#if 0
  GPU_VERTBUF_DISCARD_SAFE(cache->polygon_mat_offset);
  MEM_SAFE_FREE(cache->mat_start);
  MEM_SAFE_FREE(cache->mat_end);
  GPU_VERTBUF_DISCARD_SAFE(cache->verts_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->edges_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->faces_orig_index);
  GPU_VERTBUF_DISCARD_SAFE(cache->fdots_patch_coords);
#endif

  fprintf(stderr, "Memory used by the GPU subdivision cache: %lu bytes\n", memory_used);

  if (cache->patch_coords) {
    fprintf(stderr, "Memory used for the patch coords: %u bytes\n", cache->num_patch_coords * 8);
  }
}

static void free_draw_cache_from_subdiv_cb(void *ptr)
{
  DRWSubdivCache *cache = (DRWSubdivCache *)(ptr);
  draw_subdiv_cache_free(cache);
  MEM_freeN(cache);
}

static DRWSubdivCache *ensure_draw_cache(Subdiv *subdiv)
{
  DRWSubdivCache *draw_cache = subdiv->draw_cache;
  if (draw_cache == NULL) {
    // fprintf(stderr, "Creating a new cache !\n");
    draw_cache = MEM_callocN(sizeof(DRWSubdivCache), "DRWSubdivCache");
  }
  subdiv->draw_cache = draw_cache;
  subdiv->free_draw_cache = free_draw_cache_from_subdiv_cb;
  return draw_cache;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name DRWCacheBuildingContext
 * \{ */

typedef struct DRWCacheBuildingContext {
  const Mesh *coarse_mesh;
  const SubdivToMeshSettings *settings;

  CompressedPatchCoord *patch_coords;
  /* Number of coordinantes. */
  uint num_patch_coords;
  uint num_vertices;
  uint num_edges;

  int *subdiv_loop_vert_index;
  int *vert_origindex_map;
  int *edge_origindex_map;
  int *subdiv_loop_subdiv_vert_index;
  int *subdiv_loop_edge_index;
  int *subdiv_loop_poly_index;

  /* ibo.points, one value per subdivided vertex, mapping coarse vertices -> subdivided loop */
  int *point_indices;

  int *face_ptex_offset;
  int *subdiv_polygon_offset;
} DRWCacheBuildingContext;

static bool patch_coords_topology_info(const SubdivForeachContext *foreach_context,
                                       const int num_vertices,
                                       const int num_edges,
                                       const int num_loops,
                                       const int UNUSED(num_polygons),
                                       const int *face_ptex_offset,
                                       const int *subdiv_polygon_offset)
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->patch_coords = MEM_mallocN(num_loops * sizeof(CompressedPatchCoord),
                                  "CompressedPatchCoord");
  ctx->subdiv_loop_vert_index = MEM_mallocN(num_loops * sizeof(int), "subdiv_loop_vert_index");
  ctx->subdiv_loop_subdiv_vert_index = MEM_mallocN(num_loops * sizeof(int),
                                                   "subdiv_loop_subdiv_vert_index");
  ctx->subdiv_loop_edge_index = MEM_mallocN(num_loops * sizeof(int), "subdiv_loop_edge_index");
  ctx->subdiv_loop_poly_index = MEM_mallocN(num_loops * sizeof(int), "subdiv_loop_poly_index");

  ctx->face_ptex_offset = MEM_dupallocN(face_ptex_offset);
  ctx->subdiv_polygon_offset = MEM_dupallocN(subdiv_polygon_offset);

  ctx->num_patch_coords = num_loops;
  ctx->num_vertices = num_vertices;
  ctx->num_edges = num_edges;

  ctx->vert_origindex_map = MEM_mallocN(num_vertices * sizeof(int), "subdiv_vert_origindex_map");
  for (int i = 0; i < num_vertices; i++) {
    ctx->vert_origindex_map[i] = -1;
  }

  ctx->edge_origindex_map = MEM_mallocN(num_edges * sizeof(int), "subdiv_edge_origindex_map");
  for (int i = 0; i < num_edges; i++) {
    ctx->edge_origindex_map[i] = -1;
  }

  ctx->point_indices = MEM_mallocN(num_vertices * sizeof(int), "point_indices");
  for (int i = 0; i < num_vertices; i++) {
    ctx->point_indices[i] = -1;
  }

  return true;
}

static void draw_subdiv_vertex_corner_cb(const SubdivForeachContext *foreach_context,
                                         void *UNUSED(tls),
                                         const int UNUSED(ptex_face_index),
                                         const float UNUSED(u),
                                         const float UNUSED(v),
                                         const int coarse_vertex_index,
                                         const int UNUSED(coarse_poly_index),
                                         const int UNUSED(coarse_corner),
                                         const int subdiv_vertex_index)
{
  BLI_assert(coarse_vertex_index != ORIGINDEX_NONE);
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->vert_origindex_map[subdiv_vertex_index] = coarse_vertex_index;
}

static void draw_subdiv_vertex_edge_cb(const SubdivForeachContext *UNUSED(foreach_context),
                                       void *UNUSED(tls_v),
                                       const int UNUSED(ptex_face_index),
                                       const float UNUSED(u),
                                       const float UNUSED(v),
                                       const int UNUSED(coarse_edge_index),
                                       const int UNUSED(coarse_poly_index),
                                       const int UNUSED(coarse_corner),
                                       const int UNUSED(subdiv_vertex_index))
{
  /* Required if SubdivForeachContext.vertex_corner is also set. */
}

static void draw_subdiv_edge_cb(const SubdivForeachContext *foreach_context,
                                void *UNUSED(tls),
                                const int coarse_edge_index,
                                const int subdiv_edge_index,
                                const int UNUSED(subdiv_v1),
                                const int UNUSED(subdiv_v2))
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->edge_origindex_map[subdiv_edge_index] = coarse_edge_index;
}

static void draw_subdiv_loop_cb(const SubdivForeachContext *foreach_context,
                                void *UNUSED(tls_v),
                                const int ptex_face_index,
                                const float u,
                                const float v,
                                const int UNUSED(coarse_loop_index),
                                const int coarse_poly_index,
                                const int UNUSED(coarse_corner),
                                const int subdiv_loop_index,
                                const int subdiv_vertex_index,
                                const int subdiv_edge_index)
{
  DRWCacheBuildingContext *ctx = (DRWCacheBuildingContext *)(foreach_context->user_data);
  ctx->patch_coords[subdiv_loop_index] = make_patch_coord(ptex_face_index, u, v);

  const int coarse_vertex_index = ctx->vert_origindex_map[subdiv_vertex_index];

  if (coarse_vertex_index != -1) {
    ctx->point_indices[coarse_vertex_index] = subdiv_loop_index;
  }

  ctx->subdiv_loop_subdiv_vert_index[subdiv_loop_index] = subdiv_vertex_index;
  /* For now index the subdiv_edge_index, it will be replaced by the actual coarse edge index
   * at the end of the traversal as some edges are only then traversed. */
  ctx->subdiv_loop_edge_index[subdiv_loop_index] = subdiv_edge_index;
  ctx->subdiv_loop_poly_index[subdiv_loop_index] = coarse_poly_index;
  ctx->subdiv_loop_vert_index[subdiv_loop_index] = coarse_vertex_index;
}

static void draw_subdiv_foreach_callbacks(SubdivForeachContext *foreach_context)
{
  memset(foreach_context, 0, sizeof(*foreach_context));
  foreach_context->topology_info = patch_coords_topology_info;
  foreach_context->loop = draw_subdiv_loop_cb;
  foreach_context->edge = draw_subdiv_edge_cb;
  foreach_context->vertex_corner = draw_subdiv_vertex_corner_cb;
  foreach_context->vertex_edge = draw_subdiv_vertex_edge_cb;
}

static void build_cached_data_from_subdiv(DRWCacheBuildingContext *cache_building_context,
                                          Subdiv *subdiv)
{
  SubdivForeachContext foreach_context;
  draw_subdiv_foreach_callbacks(&foreach_context);
  foreach_context.user_data = cache_building_context;

  BKE_subdiv_foreach_subdiv_geometry(subdiv,
                                     &foreach_context,
                                     cache_building_context->settings,
                                     cache_building_context->coarse_mesh);

  /* Setup actual coarse edge index. */
  for (int i = 0; i < cache_building_context->num_patch_coords; i++) {
    cache_building_context->subdiv_loop_edge_index[i] =
        cache_building_context
            ->edge_origindex_map[cache_building_context->subdiv_loop_edge_index[i]];
  }
}

static GPUVertBuf *gpu_vertbuf_create_from_format(GPUVertFormat *format, uint len)
{
  GPUVertBuf *verts = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format(verts, format);
  GPU_vertbuf_data_alloc(verts, len);
  return verts;
}

static GPUVertBuf *gpu_vertbuf_from_blender_patch_coords(CompressedPatchCoord *patch_coords,
                                                         uint len)
{
  GPUVertBuf *blender_patch_coords = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      blender_patch_coords, get_blender_patch_coords_format(), GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(blender_patch_coords, len);
  memcpy(GPU_vertbuf_get_data(blender_patch_coords),
         patch_coords,
         len * sizeof(CompressedPatchCoord));
  return blender_patch_coords;
}

static void build_vertex_face_adjacency_maps(DRWSubdivCache *cache)
{
  /* +1 so that we do not require a special for the last vertex, this extra offset will contain the
   * total number of adjacent faces. */
  cache->subdiv_vertex_face_adjacency_offsets = gpu_vertbuf_create_from_format(
      get_origindex_format(), cache->num_vertices + 1);

  int *vertex_offsets = (int *)GPU_vertbuf_get_data(cache->subdiv_vertex_face_adjacency_offsets);
  memset(vertex_offsets, 0, sizeof(int) * cache->num_vertices + 1);

  for (int i = 0; i < cache->num_patch_coords; i++) {
    vertex_offsets[cache->subdiv_loop_subdiv_vert_index[i]]++;
  }

  int ofs = vertex_offsets[0];
  vertex_offsets[0] = 0;
  for (uint i = 1; i < cache->num_vertices + 1; i++) {
    int tmp = vertex_offsets[i];
    vertex_offsets[i] = ofs;
    ofs += tmp;
  }

  cache->subdiv_vertex_face_adjacency = gpu_vertbuf_create_from_format(get_origindex_format(),
                                                                       cache->num_patch_coords);
  int *adjacent_faces = (int *)GPU_vertbuf_get_data(cache->subdiv_vertex_face_adjacency);
  int *tmp_set_faces = MEM_callocN(sizeof(int) * cache->num_vertices, "tmp subdiv vertex offset");

  for (int i = 0; i < cache->num_patch_coords / 4; i++) {
    for (int j = 0; j < 4; j++) {
      const int subdiv_vertex = cache->subdiv_loop_subdiv_vert_index[i * 4 + j];
      int first_face_offset = vertex_offsets[subdiv_vertex] + tmp_set_faces[subdiv_vertex];
      adjacent_faces[first_face_offset] = i;
      tmp_set_faces[subdiv_vertex] += 1;
    }
  }

  MEM_freeN(tmp_set_faces);
}

static bool generate_required_cached_data(DRWSubdivCache *cache,
                                          Subdiv *subdiv,
                                          Mesh *mesh_eval,
                                          const Scene *scene,
                                          const SubsurfModifierData *smd,
                                          const bool is_final_render)
{
  const int level = get_render_subsurf_level(&scene->r, smd->levels, is_final_render);
  SubdivToMeshSettings to_mesh_settings;
  to_mesh_settings.resolution = (1 << level) + 1;
  to_mesh_settings.use_optimal_display = false;

  if (cache->resolution != to_mesh_settings.resolution) {
    // fprintf(stderr, "Resolution changed rebuilding cache !\n");
    /* Resolution chaged, we need to rebuild. */
    draw_subdiv_cache_free(cache);
  }

  if (cache->patch_coords != NULL) {
    // fprintf(stderr, "Cache does not need to be rebuilt !\n");
    /* No need to rebuild anything. */
    return true;
  }

  DRWCacheBuildingContext cache_building_context;
  cache_building_context.coarse_mesh = mesh_eval;
  cache_building_context.settings = &to_mesh_settings;

  build_cached_data_from_subdiv(&cache_building_context, subdiv);
  if (cache_building_context.num_patch_coords == 0) {
    if (cache_building_context.face_ptex_offset) {
      MEM_freeN(cache_building_context.face_ptex_offset);
    }
    if (cache_building_context.subdiv_polygon_offset) {
      MEM_freeN(cache_building_context.subdiv_polygon_offset);
    }
    return false;
  }

  cache->patch_coords = gpu_vertbuf_from_blender_patch_coords(
      cache_building_context.patch_coords, cache_building_context.num_patch_coords);

  /* Build buffers for the PatchMap. */
  gpu_patch_map_build(&cache->gpu_patch_map, subdiv);

  // Build patch coordinates for all the face dots
  cache->fdots_patch_coords = gpu_vertbuf_create_from_format(get_blender_patch_coords_format(),
                                                             mesh_eval->totpoly);
  CompressedPatchCoord *blender_fdots_patch_coords = (CompressedPatchCoord *)GPU_vertbuf_get_data(
      cache->fdots_patch_coords);
  for (int i = 0; i < mesh_eval->totpoly; i++) {
    const int ptex_face_index = cache_building_context.face_ptex_offset[i];
    if (mesh_eval->mpoly[i].totloop == 4) {
      /* For quads, the center coordinate of the coarse face has `u = v = 0.5`. */
      blender_fdots_patch_coords[i] = make_patch_coord(ptex_face_index, 0.5f, 0.5f);
    }
    else {
      /* For N-gons, since they are split into quads from the center, and since the center is
       * chosen to be the top right corner of each quad, the center coordinate of the coarse face
       * is anyone of those top right corner with `u = v = 1.0`. */
      blender_fdots_patch_coords[i] = make_patch_coord(ptex_face_index, 1.0f, 1.0f);
    }
  }

  cache->resolution = to_mesh_settings.resolution;
  cache->num_patch_coords = cache_building_context.num_patch_coords;
  cache->num_edges = cache_building_context.num_edges;

  cache->verts_orig_index = build_origindex_buffer(cache_building_context.subdiv_loop_vert_index,
                                                   cache_building_context.num_patch_coords);
  cache->edges_orig_index = build_origindex_buffer(cache_building_context.subdiv_loop_edge_index,
                                                   cache_building_context.num_patch_coords);

  cache->face_ptex_offset = cache_building_context.face_ptex_offset;

  cache->subdiv_polygon_offset_buffer = build_origindex_buffer(
      cache_building_context.subdiv_polygon_offset, mesh_eval->totpoly);
  cache->subdiv_polygon_offset = cache_building_context.subdiv_polygon_offset;
  cache->coarse_poly_count = mesh_eval->totpoly;
  cache->point_indices = cache_building_context.point_indices;
  cache->num_vertices = cache_building_context.num_vertices;

  cache->subdiv_loop_subdiv_vert_index = cache_building_context.subdiv_loop_subdiv_vert_index;
  cache->subdiv_loop_vert_index = cache_building_context.subdiv_loop_vert_index;
  cache->subdiv_loop_edge_index = cache_building_context.subdiv_loop_edge_index;
  cache->subdiv_loop_poly_index = cache_building_context.subdiv_loop_poly_index;

  build_vertex_face_adjacency_maps(cache);

  /* Cleanup. */
  MEM_freeN(cache_building_context.patch_coords);
  MEM_freeN(cache_building_context.vert_origindex_map);
  MEM_freeN(cache_building_context.edge_origindex_map);

  return true;
}

/** \} */

// --------------------------------------------------------

typedef struct DRWSubdivBuffers {
  uint number_of_loops;
  uint number_of_quads;
  uint number_of_triangles;
  uint number_of_subdiv_verts;
  int coarse_poly_count;

  GPUVertBuf *patch_coords;
  GPUVertBuf *fdots_patch_coords;
  GPUVertBuf *face_flags;
  GPUVertBuf *subdiv_polygon_offset;

  GPUVertBuf *face_origindex;
  GPUVertBuf *vert_origindex;
  GPUVertBuf *edge_origindex;

  GPUVertBuf *polygon_mat_offset;
  const GPUPatchMap *gpu_patch_map;
} DRWSubdivBuffers;

static void initialize_buffers(DRWSubdivBuffers *buffers, const DRWSubdivCache *draw_cache)
{
  const uint number_of_loops = draw_cache->num_patch_coords;
  buffers->number_of_loops = number_of_loops;
  buffers->number_of_quads = number_of_loops / 4;
  buffers->number_of_triangles = tris_count_from_number_of_loops(number_of_loops);
  buffers->number_of_subdiv_verts = draw_cache->num_vertices;
  buffers->polygon_mat_offset = draw_cache->polygon_mat_offset;
  buffers->patch_coords = draw_cache->patch_coords;
  buffers->edge_origindex = draw_cache->edges_orig_index;
  buffers->vert_origindex = draw_cache->verts_orig_index;
  buffers->face_origindex = draw_cache->faces_orig_index;
  buffers->face_flags = draw_cache->face_flags;
  buffers->subdiv_polygon_offset = draw_cache->subdiv_polygon_offset_buffer;
  buffers->coarse_poly_count = draw_cache->coarse_poly_count;
  buffers->fdots_patch_coords = draw_cache->fdots_patch_coords;
  buffers->gpu_patch_map = &draw_cache->gpu_patch_map;
}

// --------------------------------------------------------

#define PATCH_EVALUATION_WORK_GROUP_SIZE 64
static uint get_patch_evaluation_work_group_size(uint elements)
{
  return divide_ceil_u(elements, PATCH_EVALUATION_WORK_GROUP_SIZE);
}

static void draw_subdiv_extract_pos_nor(DRWSubdivBuffers *buffers,
                                        GPUVertBuf *pos_nor,
                                        Subdiv *subdiv,
                                        const bool do_limit_normals)
{
  GPUVertBuf *src_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(src_buffer, get_work_vertex_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface src_buffer_interface;
  opensubdiv_gpu_buffer_init(&src_buffer_interface, src_buffer);
  subdiv->evaluator->buildSrcBuffer(subdiv->evaluator, &src_buffer_interface);

  GPUVertBuf *patch_arrays_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_arrays_buffer, get_patch_array_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_arrays_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_arrays_buffer_interface, patch_arrays_buffer);
  subdiv->evaluator->buildPatchArraysBuffer(subdiv->evaluator, &patch_arrays_buffer_interface);

  GPUVertBuf *patch_index_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_index_buffer, get_patch_index_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_index_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_index_buffer_interface, patch_index_buffer);
  subdiv->evaluator->buildPatchIndexBuffer(subdiv->evaluator, &patch_index_buffer_interface);

  GPUVertBuf *patch_param_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_param_buffer, get_patch_param_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_param_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_param_buffer_interface, patch_param_buffer);
  subdiv->evaluator->buildPatchParamBuffer(subdiv->evaluator, &patch_param_buffer_interface);

  GPUShader *shader = get_patch_evaluation_shader(
      do_limit_normals ? SHADER_PATCH_EVALUATION : SHADER_PATCH_EVALUATION_LIMIT_NORMALS);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "min_patch_face", buffers->gpu_patch_map->min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", buffers->gpu_patch_map->max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", buffers->gpu_patch_map->max_depth);
  GPU_shader_uniform_1i(
      shader, "patches_are_triangular", buffers->gpu_patch_map->patches_are_triangular);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map->patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map->patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(buffers->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(buffers->vert_origindex, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(pos_nor, 8);

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(buffers->number_of_quads), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  /* Free the memory used for allocating a GPUVertBuf, but not the underlying buffers. */
  GPU_vertbuf_wrap_device_ptr(patch_index_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(patch_param_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(src_buffer, 0);

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

static void draw_subdiv_extract_uvs_ex(DRWSubdivBuffers *buffers,
                                       GPUVertBuf *uvs,
                                       Subdiv *subdiv,
                                       const int face_varying_channel,
                                       const int dst_offset)
{
  GPUVertBuf *src_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(src_buffer, get_uvs_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface src_buffer_interface;
  opensubdiv_gpu_buffer_init(&src_buffer_interface, src_buffer);
  subdiv->evaluator->buildFVarSrcBuffer(
      subdiv->evaluator, face_varying_channel, &src_buffer_interface);

  GPUVertBuf *patch_arrays_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_arrays_buffer, get_patch_array_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_arrays_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_arrays_buffer_interface, patch_arrays_buffer);
  subdiv->evaluator->buildFVarPatchArraysBuffer(
      subdiv->evaluator, face_varying_channel, &patch_arrays_buffer_interface);

  GPUVertBuf *patch_index_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_index_buffer, get_patch_index_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_index_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_index_buffer_interface, patch_index_buffer);
  subdiv->evaluator->buildFVarPatchIndexBuffer(
      subdiv->evaluator, face_varying_channel, &patch_index_buffer_interface);

  GPUVertBuf *patch_param_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_param_buffer, get_patch_param_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_param_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_param_buffer_interface, patch_param_buffer);
  subdiv->evaluator->buildFVarPatchParamBuffer(
      subdiv->evaluator, face_varying_channel, &patch_param_buffer_interface);

  GPUShader *shader = get_patch_evaluation_shader(SHADER_PATCH_FVAR_EVALUATION);
  GPU_shader_bind(shader);

  /* The buffer offset has the stride baked in (which is 2 as we have UVs) so remove the stride by
   * dividing by 2 */
  GPU_shader_uniform_1i(shader, "src_offset", src_buffer_interface.buffer_offset / 2);
  GPU_shader_uniform_1i(shader, "dst_offset", dst_offset);
  GPU_shader_uniform_1i(shader, "min_patch_face", buffers->gpu_patch_map->min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", buffers->gpu_patch_map->max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", buffers->gpu_patch_map->max_depth);
  GPU_shader_uniform_1i(
      shader, "patches_are_triangular", buffers->gpu_patch_map->patches_are_triangular);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map->patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map->patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(buffers->patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(buffers->vert_origindex, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(uvs, 8);

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(buffers->number_of_quads), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  /* Free the memory used for allocating a GPUVertBuf, but not the underlying buffers. */
  GPU_vertbuf_wrap_device_ptr(patch_index_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(patch_param_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(src_buffer, 0);

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

static void draw_subdiv_extract_uvs(DRWSubdivBuffers *buffers,
                                    Subdiv *subdiv,
                                    GPUVertBuf *face_varying,
                                    uint uv_layers)
{
  /* Index of the UV layer in the compact buffer. Used UV layers are stored in a single buffer. */
  int pack_layer_index = 0;
  for (int i = 0; i < MAX_MTFACE; i++) {
    if (uv_layers & (1 << i)) {
      const int offset = (int)buffers->number_of_loops * pack_layer_index++;
      draw_subdiv_extract_uvs_ex(buffers, face_varying, subdiv, i, offset);
    }
  }
}

static void do_accumulate_normals(DRWSubdivBuffers *buffers,
                                  GPUVertBuf *pos_nor,
                                  GPUVertBuf *face_adjacency_offsets,
                                  GPUVertBuf *face_adjacency_lists,
                                  GPUVertBuf *vertex_normals)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_NORMALS_ACCUMULATE, NULL);
  GPU_shader_bind(shader);

  int binding_point = 0;

  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);
  GPU_vertbuf_bind_as_ssbo(face_adjacency_offsets, binding_point++);
  GPU_vertbuf_bind_as_ssbo(face_adjacency_lists, binding_point++);
  GPU_vertbuf_bind_as_ssbo(vertex_normals, binding_point++);

  GPU_compute_dispatch(shader, buffers->number_of_subdiv_verts, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_finalize_normals(DRWSubdivBuffers *buffers,
                                GPUVertBuf *vertex_normals,
                                GPUVertBuf *subdiv_loop_subdiv_vert_index,
                                GPUVertBuf *pos_nor)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_NORMALS_FINALIZE, NULL);
  GPU_shader_bind(shader);

  int binding_point = 0;
  GPU_vertbuf_bind_as_ssbo(vertex_normals, binding_point++);
  GPU_vertbuf_bind_as_ssbo(subdiv_loop_subdiv_vert_index, binding_point++);
  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_tris_buffer(DRWSubdivBuffers *buffers,
                                 GPUIndexBuf *subdiv_tris,
                                 const int material_count)
{
  const bool do_single_material = material_count <= 1;

  const char *defines = NULL;
  if (do_single_material) {
    defines = "#define SINGLE_MATERIAL\n";
  }

  GPUShader *shader = get_subdiv_shader(
      do_single_material ? SHADER_BUFFER_TRIS : SHADER_BUFFER_TRIS_MULTIPLE_MATERIALS, defines);
  GPU_shader_bind(shader);

  int binding_point = 0;

  /* Inputs */

  /* Outputs */
  GPU_indexbuf_bind_as_ssbo(subdiv_tris, binding_point++);

  if (!do_single_material) {
    GPU_shader_uniform_1i(shader, "coarse_poly_count", buffers->coarse_poly_count);
    GPU_vertbuf_bind_as_ssbo(buffers->polygon_mat_offset, binding_point++);
    GPU_vertbuf_bind_as_ssbo(buffers->subdiv_polygon_offset, binding_point++);
  }

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_fdots_buffer(DRWSubdivBuffers *buffers,
                                  Subdiv *subdiv,
                                  GPUVertBuf *fdots_pos,
                                  GPUVertBuf *fdots_nor,
                                  GPUIndexBuf *fdots_indices)
{
  GPUVertBuf *src_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(src_buffer, get_work_vertex_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface src_buffer_interface;
  opensubdiv_gpu_buffer_init(&src_buffer_interface, src_buffer);
  subdiv->evaluator->buildSrcBuffer(subdiv->evaluator, &src_buffer_interface);

  GPUVertBuf *patch_arrays_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_arrays_buffer, get_patch_array_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_arrays_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_arrays_buffer_interface, patch_arrays_buffer);
  subdiv->evaluator->buildPatchArraysBuffer(subdiv->evaluator, &patch_arrays_buffer_interface);

  GPUVertBuf *patch_index_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_index_buffer, get_patch_index_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_index_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_index_buffer_interface, patch_index_buffer);
  subdiv->evaluator->buildPatchIndexBuffer(subdiv->evaluator, &patch_index_buffer_interface);

  GPUVertBuf *patch_param_buffer = GPU_vertbuf_calloc();
  GPU_vertbuf_init_with_format_ex(
      patch_param_buffer, get_patch_param_format(), GPU_USAGE_DEVICE_ONLY);
  OpenSubdiv_BufferInterface patch_param_buffer_interface;
  opensubdiv_gpu_buffer_init(&patch_param_buffer_interface, patch_param_buffer);
  subdiv->evaluator->buildPatchParamBuffer(subdiv->evaluator, &patch_param_buffer_interface);

  GPUShader *shader = get_patch_evaluation_shader(SHADER_PATCH_EVALUATION_FACE_DOTS);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "min_patch_face", buffers->gpu_patch_map->min_patch_face);
  GPU_shader_uniform_1i(shader, "max_patch_face", buffers->gpu_patch_map->max_patch_face);
  GPU_shader_uniform_1i(shader, "max_depth", buffers->gpu_patch_map->max_depth);
  GPU_shader_uniform_1i(
      shader, "patches_are_triangular", buffers->gpu_patch_map->patches_are_triangular);

  GPU_vertbuf_bind_as_ssbo(src_buffer, 0);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map->patch_map_handles, 1);
  GPU_vertbuf_bind_as_ssbo(buffers->gpu_patch_map->patch_map_quadtree, 2);
  GPU_vertbuf_bind_as_ssbo(buffers->fdots_patch_coords, 3);
  GPU_vertbuf_bind_as_ssbo(buffers->vert_origindex, 4);
  GPU_vertbuf_bind_as_ssbo(patch_arrays_buffer, 5);
  GPU_vertbuf_bind_as_ssbo(patch_index_buffer, 6);
  GPU_vertbuf_bind_as_ssbo(patch_param_buffer, 7);
  GPU_vertbuf_bind_as_ssbo(fdots_pos, 8);
  GPU_vertbuf_bind_as_ssbo(fdots_nor, 9);
  GPU_indexbuf_bind_as_ssbo(fdots_indices, 10);

  GPU_compute_dispatch(
      shader, get_patch_evaluation_work_group_size(buffers->coarse_poly_count), 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();

  /* Free the memory used for allocating a GPUVertBuf, but not the underlying buffers. */
  GPU_vertbuf_wrap_device_ptr(patch_index_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(patch_param_buffer, 0);
  GPU_vertbuf_wrap_device_ptr(src_buffer, 0);

  GPU_vertbuf_discard(patch_index_buffer);
  GPU_vertbuf_discard(patch_param_buffer);
  GPU_vertbuf_discard(patch_arrays_buffer);
  GPU_vertbuf_discard(src_buffer);
}

static void do_build_lines_buffer(DRWSubdivBuffers *buffers,
                                  GPUIndexBuf *lines_indices,
                                  const bool optimal_display)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LINES, NULL);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "optimal_display", optimal_display);

  GPU_vertbuf_bind_as_ssbo(buffers->edge_origindex, 0);
  GPU_indexbuf_bind_as_ssbo(lines_indices, 1);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_edge_fac_buffer(DRWSubdivBuffers *buffers,
                                     GPUVertBuf *pos_nor,
                                     GPUVertBuf *edge_idx,
                                     bool optimal_display,
                                     GPUVertBuf *edge_fac)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_EDGE_FAC, NULL);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1b(shader, "optimal_display", optimal_display);

  GPU_vertbuf_bind_as_ssbo(pos_nor, 0);
  GPU_vertbuf_bind_as_ssbo(edge_idx, 1);
  GPU_vertbuf_bind_as_ssbo(edge_fac, 2);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

static void do_build_lnor_buffer(DRWSubdivBuffers *buffers, GPUVertBuf *pos_nor, GPUVertBuf *lnor)
{
  GPUShader *shader = get_subdiv_shader(SHADER_BUFFER_LNOR, NULL);
  GPU_shader_bind(shader);

  GPU_shader_uniform_1i(shader, "coarse_poly_count", buffers->coarse_poly_count);

  int binding_point = 0;
  /* Inputs */
  GPU_vertbuf_bind_as_ssbo(pos_nor, binding_point++);
  GPU_vertbuf_bind_as_ssbo(buffers->face_flags, binding_point++);
  GPU_vertbuf_bind_as_ssbo(buffers->subdiv_polygon_offset, binding_point++);

  /* Outputs */
  GPU_vertbuf_bind_as_ssbo(lnor, binding_point++);

  GPU_compute_dispatch(shader, buffers->number_of_quads, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);

  /* Cleanup. */
  GPU_shader_unbind();
}

// --------------------------------------------------------

static SubsurfModifierData *get_subsurf_modifier(Object *ob)
{
  ModifierData *md = (ModifierData *)(ob->modifiers.last);

  if (md == NULL) {
    return NULL;
  }

  if (md->type != eModifierType_Subsurf) {
    return NULL;
  }

  return (SubsurfModifierData *)(md);
}
/* -------------------------------------------------------------------- */

static void print_requests(MeshBufferCache *mbc)
{
  fprintf(stderr, "============== REQUESTS ==============\n");

#define PRINT_VBO_REQUEST(request) \
  if (DRW_vbo_requested(mbc->request)) \
  fprintf(stderr, #request " requested\n")
#define PRINT_IBO_REQUEST(request) \
  if (DRW_ibo_requested(mbc->request)) \
  fprintf(stderr, #request " requested\n")

  PRINT_VBO_REQUEST(vbo.lnor);
  PRINT_VBO_REQUEST(vbo.pos_nor);
  PRINT_VBO_REQUEST(vbo.uv);
  PRINT_VBO_REQUEST(vbo.vcol);
  PRINT_VBO_REQUEST(vbo.sculpt_data);
  PRINT_VBO_REQUEST(vbo.weights);
  PRINT_VBO_REQUEST(vbo.edge_fac);
  PRINT_VBO_REQUEST(vbo.mesh_analysis);
  PRINT_VBO_REQUEST(vbo.tan);
  PRINT_VBO_REQUEST(vbo.orco);
  PRINT_VBO_REQUEST(vbo.edit_data);
  PRINT_VBO_REQUEST(vbo.fdots_pos);
  PRINT_VBO_REQUEST(vbo.fdots_nor);
  PRINT_VBO_REQUEST(vbo.skin_roots);
  PRINT_VBO_REQUEST(vbo.vert_idx);
  PRINT_VBO_REQUEST(vbo.edge_idx);
  PRINT_VBO_REQUEST(vbo.poly_idx);
  PRINT_VBO_REQUEST(vbo.fdot_idx);
  PRINT_VBO_REQUEST(vbo.edituv_data);
  PRINT_VBO_REQUEST(vbo.edituv_stretch_area);
  PRINT_VBO_REQUEST(vbo.edituv_stretch_angle);
  PRINT_VBO_REQUEST(vbo.fdots_uv);
  PRINT_VBO_REQUEST(vbo.fdots_edituv_data);

  PRINT_IBO_REQUEST(ibo.tris);
  PRINT_IBO_REQUEST(ibo.lines);
  PRINT_IBO_REQUEST(ibo.lines_loose);
  PRINT_IBO_REQUEST(ibo.lines_adjacency);
  PRINT_IBO_REQUEST(ibo.lines_paint_mask);
  PRINT_IBO_REQUEST(ibo.points);
  PRINT_IBO_REQUEST(ibo.fdots);
  PRINT_IBO_REQUEST(ibo.edituv_tris);
  PRINT_IBO_REQUEST(ibo.edituv_lines);
  PRINT_IBO_REQUEST(ibo.edituv_points);
  PRINT_IBO_REQUEST(ibo.edituv_fdots);

#undef PRINT_IBO_REQUEST
#undef PRINT_VBO_REQUEST
}

static void update_edit_data(DRWSubdivCache *cache,
                             Mesh *mesh,
                             EditLoopData *edit_data,
                             const ToolSettings *toolsettings)
{
  if (!mesh->edit_mesh) {
    return;
  }

  BMesh *bm = mesh->edit_mesh->bm;
  BM_mesh_elem_table_ensure(bm, BM_EDGE | BM_FACE | BM_VERT);

  MeshRenderData mr;
  mr.bm = bm;
  mr.toolsettings = toolsettings;
  mr.eed_act = BM_mesh_active_edge_get(bm);
  mr.efa_act = BM_mesh_active_face_get(bm, false, true);
  mr.eve_act = BM_mesh_active_vert_get(bm);
  mr.vert_crease_ofs = CustomData_get_offset(&bm->vdata, CD_CREASE);
  mr.edge_crease_ofs = CustomData_get_offset(&bm->edata, CD_CREASE);
  mr.bweight_ofs = CustomData_get_offset(&bm->edata, CD_BWEIGHT);
#ifdef WITH_FREESTYLE
  mr.freestyle_edge_ofs = CustomData_get_offset(&bm->edata, CD_FREESTYLE_EDGE);
  mr.freestyle_face_ofs = CustomData_get_offset(&bm->pdata, CD_FREESTYLE_FACE);
#endif

  for (uint i = 0; i < cache->num_patch_coords; i++) {
    const int vert_origindex = cache->subdiv_loop_vert_index[i];
    const int edge_origindex = cache->subdiv_loop_edge_index[i];
    const int poly_origindex = cache->subdiv_loop_poly_index[i];

    EditLoopData *edit_loop_data = &edit_data[i];
    memset(edit_loop_data, 0, sizeof(EditLoopData));

    if (vert_origindex != -1) {
      const BMVert *eve = BM_vert_at_index(bm, vert_origindex);
      mesh_render_data_vert_flag(&mr, eve, edit_loop_data);
    }

    if (edge_origindex != -1) {
      const BMEdge *eed = BM_edge_at_index(bm, edge_origindex);
      mesh_render_data_edge_flag(&mr, eed, edit_loop_data);
    }

    BMFace *efa = BM_face_at_index(bm, poly_origindex);
    /* The -1 parameter is for edit_uvs, which we don't do here. */
    mesh_render_data_face_flag(&mr, efa, -1, edit_loop_data);
  }

  LooseEdge *loose_edge = cache->loose_edges;
  int ledge_index = 0;
  while (loose_edge) {
    const int offset = cache->num_patch_coords + ledge_index * 2;
    EditLoopData *data = &edit_data[offset];
    memset(data, 0, sizeof(EditLoopData));
    BMEdge *eed = BM_edge_at_index(bm, loose_edge->coarse_edge_index);
    mesh_render_data_edge_flag(&mr, eed, &data[0]);
    data[1] = data[0];
    mesh_render_data_vert_flag(&mr, eed->v1, &data[0]);
    mesh_render_data_vert_flag(&mr, eed->v2, &data[1]);
    ledge_index += 1;
    loose_edge = loose_edge->next;
  }
}

/* For material assignements we want indices for triangles that share a common material to be laid
 * out contiguously in memory. To achieve this, we sort the indices based on which material the
 * coarse polygon was assigned. The sort is performed by offsetting the loops indices so that they
 * are directly assigned to the right sorted indices.
 *
 * Here is a visual representation, considering four quads:
 * +---------+---------+---------+---------+
 * | 3     2 | 7     6 | 11   10 | 15   14 |
 * |         |         |         |         |
 * | 0     1 | 4     5 | 8     9 | 12   13 |
 * +---------+---------+---------+---------+
 *
 * If the first and third quads have the same material, we should have:
 * +---------+---------+---------+---------+
 * | 3     2 | 11   10 | 7     6 | 15   14 |
 * |         |         |         |         |
 * | 0     1 | 8     9 | 4     5 | 12   13 |
 * +---------+---------+---------+---------+
 *
 * So the offsets would be:
 * +---------+---------+---------+---------+
 * | 0     0 | 4     4 | -4   -4 | 0     0 |
 * |         |         |         |         |
 * | 0     0 | 4     4 | -4   -4 | 0     0 |
 * +---------+---------+---------+---------+
 *
 * The offsets are computed not based on the loops indices, but on the number of subdivided
 * polygons for each coarse polygon. We then only store a single offset for each coarse polygon,
 * since all subfaces are contiguous, they all share the same offset.
 */
static void draw_subdiv_cache_ensure_mat_offsets(DRWSubdivCache *cache,
                                                 Mesh *mesh_eval,
                                                 uint mat_len)
{
  draw_subdiv_cache_free_material_data(cache);

  const int number_of_quads = cache->num_patch_coords / 4;

  if (mat_len == 1) {
    cache->mat_start = MEM_callocN(sizeof(int), "subdiv mat_end");
    cache->mat_end = MEM_callocN(sizeof(int), "subdiv mat_end");
    cache->mat_start[0] = 0;
    cache->mat_end[0] = number_of_quads;
    return;
  }

  /* Count number of subdivided polygons for each material. */
  int *mat_start = MEM_callocN(sizeof(int) * mat_len, "subdiv mat_start");
  int *subdiv_polygon_offset = cache->subdiv_polygon_offset;

  // TODO: parallel_reduce?
  for (int i = 0; i < mesh_eval->totpoly; i++) {
    const MPoly *mpoly = &mesh_eval->mpoly[i];
    const int next_offset = (i == mesh_eval->totpoly - 1) ? number_of_quads :
                                                            subdiv_polygon_offset[i + 1];
    const int quad_count = next_offset - subdiv_polygon_offset[i];
    const int mat_index = mpoly->mat_nr;
    mat_start[mat_index] += quad_count;
  }

  /* Accumulate offsets. */
  int ofs = mat_start[0];
  mat_start[0] = 0;
  for (uint i = 1; i < mat_len; i++) {
    int tmp = mat_start[i];
    mat_start[i] = ofs;
    ofs += tmp;
  }

  /* Compute per polygon offsets. */
  int *mat_end = MEM_dupallocN(mat_start);
  int *per_polygon_mat_offset = MEM_mallocN(sizeof(int) * mesh_eval->totpoly,
                                            "per_polygon_mat_offset");

  for (int i = 0; i < mesh_eval->totpoly; i++) {
    const MPoly *mpoly = &mesh_eval->mpoly[i];
    const int mat_index = mpoly->mat_nr;
    const int single_material_index = subdiv_polygon_offset[i];
    const int material_offset = mat_end[mat_index];
    const int next_offset = (i == mesh_eval->totpoly - 1) ? number_of_quads :
                                                            subdiv_polygon_offset[i + 1];
    const int quad_count = next_offset - subdiv_polygon_offset[i];
    mat_end[mat_index] += quad_count;

    per_polygon_mat_offset[i] = material_offset - single_material_index;
  }

  cache->polygon_mat_offset = build_origindex_buffer(per_polygon_mat_offset, mesh_eval->totpoly);
  cache->mat_start = mat_start;
  cache->mat_end = mat_end;

  MEM_freeN(per_polygon_mat_offset);
}

// TODO(kevindietrich) : copied from extract_mesh_ibo_lines_adjacency
// \{
#define NO_EDGE INT_MAX

#include "BLI_edgehash.h"

struct MeshExtract_LineAdjacency_Data {
  GPUIndexBufBuilder elb;
  EdgeHash *eh;
  bool is_manifold;
  /* Array to convert vert index to any loop index of this vert. */
  uint *vert_to_loop;
};

static void init_lines_adj(uint tess_edge_len,
                           uint vert_len,
                           uint loop_len,
                           struct MeshExtract_LineAdjacency_Data *data)
{
  data->vert_to_loop = (uint *)(MEM_callocN(sizeof(uint) * vert_len, __func__));

  GPU_indexbuf_init(&data->elb, GPU_PRIM_LINES_ADJ, tess_edge_len, loop_len);
  data->eh = BLI_edgehash_new_ex(__func__, tess_edge_len);
  data->is_manifold = true;
}

BLI_INLINE void lines_adjacency_triangle(uint v1,
                                         uint v2,
                                         uint v3,
                                         uint l1,
                                         uint l2,
                                         uint l3,
                                         struct MeshExtract_LineAdjacency_Data *data)
{
  GPUIndexBufBuilder *elb = &data->elb;
  /* Iterate around the triangle's edges. */
  for (int e = 0; e < 3; e++) {
    SHIFT3(uint, v3, v2, v1);
    SHIFT3(uint, l3, l2, l1);

    bool inv_indices = (v2 > v3);
    void **pval;
    bool value_is_init = BLI_edgehash_ensure_p(data->eh, v2, v3, &pval);
    int v_data = POINTER_AS_INT(*pval);
    if (!value_is_init || v_data == NO_EDGE) {
      /* Save the winding order inside the sign bit. Because the
       * Edge-hash sort the keys and we need to compare winding later. */
      int value = (int)l1 + 1; /* 0 cannot be signed so add one. */
      *pval = POINTER_FROM_INT((inv_indices) ? -value : value);
      /* Store loop indices for remaining non-manifold edges. */
      data->vert_to_loop[v2] = l2;
      data->vert_to_loop[v3] = l3;
    }
    else {
      /* HACK Tag as not used. Prevent overhead of BLI_edgehash_remove. */
      *pval = POINTER_FROM_INT(NO_EDGE);
      bool inv_opposite = (v_data < 0);
      uint l_opposite = (uint)abs(v_data) - 1;
      /* TODO: Make this part thread-safe. */
      if (inv_opposite == inv_indices) {
        /* Don't share edge if triangles have non matching winding. */
        GPU_indexbuf_add_line_adj_verts(elb, l1, l2, l3, l1);
        GPU_indexbuf_add_line_adj_verts(elb, l_opposite, l2, l3, l_opposite);
        data->is_manifold = false;
      }
      else {
        GPU_indexbuf_add_line_adj_verts(elb, l1, l2, l3, l_opposite);
      }
    }
  }
}
// \}

// Similar to mesh_render_data_loose_geom_mesh but we use a linked list instead of an array.
static void update_loose_elements(DRWSubdivCache *cache, const Mesh *mesh)
{
  const MEdge *medge = mesh->medge;
  const MVert *mvert = mesh->mvert;

  if (cache->loose_memarena) {
    /* Topology did not change (otherwise cache would have been freed), so simply update
     * coordinates. */
    LooseVertex *loose_vertex = cache->loose_verts;
    while (loose_vertex) {
      copy_v3_v3(loose_vertex->co, mvert[loose_vertex->coarse_vertex_index].co);
      loose_vertex = loose_vertex->next;
    }
    return;
  }

  BLI_bitmap *vertex_used_map = BLI_BITMAP_NEW(mesh->totvert, "vert used map");
  BLI_bitmap *edge_used_map = BLI_BITMAP_NEW(mesh->totvert, "edge used map");

  MemArena *memarena = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, __func__);
  LooseVertex *loose_verts = NULL;
  LooseEdge *loose_edges = NULL;

  int num_loose_edges = 0;
  const MEdge *med = medge;
  for (int med_index = 0; med_index < mesh->totedge; med_index++, med++) {
    if (med->flag & ME_LOOSEEDGE) {
      LooseEdge *loose_edge = BLI_memarena_alloc(memarena, sizeof(LooseEdge));
      loose_edge->v1 = med->v1;
      loose_edge->v2 = med->v2;
      loose_edge->coarse_edge_index = med_index;
      loose_edge->next = loose_edges;
      loose_edges = loose_edge;
      num_loose_edges += 1;
    }
    /* Tag verts as not loose. */
    BLI_BITMAP_ENABLE(vertex_used_map, med->v1);
    BLI_BITMAP_ENABLE(vertex_used_map, med->v2);
  }

  int num_loose_verts = 0;
  for (int vertex_index = 0; vertex_index < mesh->totvert; vertex_index++) {
    if (BLI_BITMAP_TEST_BOOL(vertex_used_map, vertex_index)) {
      continue;
    }

    const MVert *vert = &mvert[vertex_index];

    LooseVertex *loose_vert = BLI_memarena_alloc(memarena, sizeof(LooseVertex));
    loose_vert->coarse_vertex_index = vertex_index;
    copy_v3_v3(loose_vert->co, vert->co);
    loose_vert->next = loose_verts;
    loose_verts = loose_vert;
    num_loose_verts += 1;
  }

  if (loose_verts != 0 || loose_edges != 0) {
    cache->vert_loose_len = num_loose_verts;
    cache->edge_loose_len = num_loose_edges;
    cache->loop_loose_len = num_loose_verts + num_loose_edges * 2;
    cache->loose_edges = loose_edges;
    cache->loose_verts = loose_verts;
    cache->loose_memarena = memarena;
  }
  else {
    BLI_memarena_free(memarena);
  }

  MEM_freeN(edge_used_map);
  MEM_freeN(vertex_used_map);
}

static bool draw_subdiv_create_requested_buffers(const Scene *scene,
                                                 Object *ob,
                                                 Mesh *mesh,
                                                 struct MeshBatchCache *batch_cache,
                                                 MeshBufferCache *mbc,
                                                 const ToolSettings *toolsettings,
                                                 OpenSubdiv_EvaluatorCache *evaluator_cache)
{
  SubsurfModifierData *smd = get_subsurf_modifier(ob);
  BLI_assert(smd);

  const bool is_final_render = DRW_state_is_scene_render();

  SubdivSettings settings;
  BKE_subdiv_settings_init_from_modifier(&settings, smd, is_final_render);

  if (settings.level == 0) {
    return false;
  }

  Mesh *mesh_eval = mesh;
  if (mesh->edit_mesh) {
    mesh_eval = mesh->edit_mesh->mesh_eval_final;
  }

  BKE_modifier_subsurf_ensure_runtime(smd);

  Subdiv *subdiv = BKE_modifier_subsurf_subdiv_descriptor_ensure(smd, &settings, mesh_eval, true);
  if (!subdiv) {
    return false;
  }

  if (!BKE_subdiv_eval_begin_from_mesh(
          subdiv, mesh_eval, NULL, OPENSUBDIV_EVALUATOR_GLSL_COMPUTE, evaluator_cache)) {
    return false;
  }

  DRWSubdivCache *draw_cache = ensure_draw_cache(subdiv);
  if (!generate_required_cached_data(draw_cache, subdiv, mesh_eval, scene, smd, is_final_render)) {
    return false;
  }

  update_loose_elements(draw_cache, mesh_eval);

  if (DRW_ibo_requested(mbc->ibo.tris)) {
    draw_subdiv_cache_ensure_mat_offsets(draw_cache, mesh_eval, batch_cache->mat_len);
  }

  draw_subdiv_cache_update_face_flags(draw_cache, mesh_eval);
  DRWSubdivBuffers subdiv_buffers = {0};
  /* We can only evaluate limit normals if the patches are adaptive. */
  const bool do_limit_normals = settings.is_adaptive;
  initialize_buffers(&subdiv_buffers, draw_cache);

  // print_requests(mbc);

  if (DRW_ibo_requested(mbc->ibo.tris)) {
    /* Initialise the index buffer, it was already allocated, it will be filled on the device. */
    GPU_indexbuf_init_build_on_device(mbc->ibo.tris, subdiv_buffers.number_of_triangles * 3);

    if (mbc->tris_per_mat) {
      for (int i = 0; i < batch_cache->mat_len; i++) {
        if (mbc->tris_per_mat[i] == NULL) {
          mbc->tris_per_mat[i] = GPU_indexbuf_calloc();
        }

        /* Multiply by 6 since we have 2 triangles per quad. */
        const int start = draw_cache->mat_start[i] * 6;
        const int len = (draw_cache->mat_end[i] - draw_cache->mat_start[i]) * 6;
        GPU_indexbuf_create_subrange_in_place(mbc->tris_per_mat[i], mbc->ibo.tris, start, len);
      }
    }

    do_build_tris_buffer(&subdiv_buffers, mbc->ibo.tris, batch_cache->mat_len);
  }

  if (DRW_vbo_requested(mbc->vbo.pos_nor)) {
    /* Initialise the vertex buffer, it was already allocated. */
    GPU_vertbuf_init_build_on_device(mbc->vbo.pos_nor,
                                     get_render_format(),
                                     subdiv_buffers.number_of_loops + draw_cache->loop_loose_len);

    draw_subdiv_extract_pos_nor(&subdiv_buffers, mbc->vbo.pos_nor, subdiv, do_limit_normals);

    if (!do_limit_normals) {
      /* We cannot evaluate vertex normals using the limit surface, so compute them manually. */
      GPUVertBuf *subdiv_loop_subdiv_vert_index = build_origindex_buffer(
          draw_cache->subdiv_loop_subdiv_vert_index, subdiv_buffers.number_of_loops);

      GPUVertBuf *vertex_normals = GPU_vertbuf_calloc();
      GPU_vertbuf_init_build_on_device(
          vertex_normals, get_lnor_format(), subdiv_buffers.number_of_subdiv_verts);

      /* accumulate normals */
      do_accumulate_normals(&subdiv_buffers,
                            mbc->vbo.pos_nor,
                            draw_cache->subdiv_vertex_face_adjacency_offsets,
                            draw_cache->subdiv_vertex_face_adjacency,
                            vertex_normals);

      /* normalize and assign */
      do_finalize_normals(
          &subdiv_buffers, vertex_normals, subdiv_loop_subdiv_vert_index, mbc->vbo.pos_nor);

      GPU_vertbuf_discard(vertex_normals);
      GPU_vertbuf_discard(subdiv_loop_subdiv_vert_index);
    }

    /* Manually copy loose vertices at the end of the buffer. */

    /* First loose edges */
    {
      uint offset = draw_cache->num_patch_coords;

      VertexBufferData vbuf_data[2];
      LooseEdge *loose_edge = draw_cache->loose_edges;
      while (loose_edge) {
        copy_v3_v3(vbuf_data[0].co, mesh_eval->mvert[loose_edge->v1].co);
        zero_v4(vbuf_data[0].no);
        copy_v3_v3(vbuf_data[0].no, mesh_eval->mvert[loose_edge->v1].no);

        copy_v3_v3(vbuf_data[1].co, mesh_eval->mvert[loose_edge->v2].co);
        zero_v4(vbuf_data[1].no);
        copy_v3_v3(vbuf_data[1].no, mesh_eval->mvert[loose_edge->v2].no);

        GPU_vertbuf_update_sub(mbc->vbo.pos_nor,
                               offset * sizeof(VertexBufferData),
                               sizeof(VertexBufferData) * 2,
                               &vbuf_data);
        loose_edge = loose_edge->next;
        offset += 2;
      }
    }

    /* Then loose vertices */
    {
      uint offset = draw_cache->num_patch_coords + draw_cache->edge_loose_len * 2;
      VertexBufferData vbuf_data;

      uint subdiv_vertex_index = subdiv_buffers.number_of_subdiv_verts;
      LooseVertex *loose_vertex = draw_cache->loose_verts;
      while (loose_vertex) {
        copy_v3_v3(vbuf_data.co, loose_vertex->co);
        zero_v4(vbuf_data.no);

        loose_vertex->subdiv_vertex_index = subdiv_vertex_index++;

        GPU_vertbuf_update_sub(mbc->vbo.pos_nor,
                               offset * sizeof(VertexBufferData),
                               sizeof(VertexBufferData),
                               &vbuf_data);
        loose_vertex = loose_vertex->next;
        offset += 1;
      }
    }
  }

  if (DRW_vbo_requested(mbc->vbo.lnor)) {
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.lnor, get_lnor_format(), subdiv_buffers.number_of_loops);
    do_build_lnor_buffer(&subdiv_buffers, mbc->vbo.pos_nor, mbc->vbo.lnor);
  }

  if (DRW_ibo_requested(mbc->ibo.fdots)) {
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.fdots_nor, get_fdots_nor_format(), subdiv_buffers.coarse_poly_count);
    GPU_vertbuf_init_build_on_device(
        mbc->vbo.fdots_pos, get_fdots_pos_format(), subdiv_buffers.coarse_poly_count);
    GPU_indexbuf_init_build_on_device(mbc->ibo.fdots, subdiv_buffers.coarse_poly_count);
    do_build_fdots_buffer(
        &subdiv_buffers, subdiv, mbc->vbo.fdots_pos, mbc->vbo.fdots_nor, mbc->ibo.fdots);
  }

  const bool optimal_display = (smd->flags & eSubsurfModifierFlag_ControlEdges);

  if (DRW_ibo_requested(mbc->ibo.lines)) {
    GPU_indexbuf_init_build_on_device(
        mbc->ibo.lines, subdiv_buffers.number_of_loops * 2 + draw_cache->edge_loose_len * 2);
    do_build_lines_buffer(&subdiv_buffers, mbc->ibo.lines, optimal_display);

    /* Make sure buffer is active for sending loose data. */
    GPU_indexbuf_use(mbc->ibo.lines);

    LooseEdge *loose_edge = draw_cache->loose_edges;
    uint offset = subdiv_buffers.number_of_loops * 2;
    uint loop_index = subdiv_buffers.number_of_loops;
    while (loose_edge) {
      GPU_indexbuf_update_sub(mbc->ibo.lines, (offset) * sizeof(uint), sizeof(uint), &loop_index);
      loop_index += 1;
      GPU_indexbuf_update_sub(
          mbc->ibo.lines, (offset + 1) * sizeof(uint), sizeof(uint), &loop_index);
      loose_edge = loose_edge->next;
      loop_index += 1;
      offset += 2;
    }
  }

  if (DRW_vbo_requested(mbc->vbo.vert_idx)) {
    /* Each element points to an element in the ibo.points. */
    init_origindex_buffer(mbc->vbo.vert_idx,
                          draw_cache->subdiv_loop_subdiv_vert_index,
                          draw_cache->num_patch_coords,
                          draw_cache->loop_loose_len);

    uint *vert_idx_data = (uint *)GPU_vertbuf_get_data(mbc->vbo.vert_idx);

    uint offset = subdiv_buffers.number_of_loops;
    LooseEdge *loose_edge = draw_cache->loose_edges;
    while (loose_edge) {
      vert_idx_data[offset] = loose_edge->v1;
      vert_idx_data[offset + 1] = loose_edge->v2;
      loose_edge = loose_edge->next;
      offset += 2;
    }

    offset = subdiv_buffers.number_of_loops + draw_cache->edge_loose_len * 2;
    LooseVertex *loose_vertex = draw_cache->loose_verts;
    while (loose_vertex) {
      vert_idx_data[offset] = loose_vertex->coarse_vertex_index;
      offset += 1;
      loose_vertex = loose_vertex->next;
    }
  }

  if (DRW_vbo_requested(mbc->vbo.edge_idx)) {
    init_origindex_buffer(mbc->vbo.edge_idx,
                          draw_cache->subdiv_loop_edge_index,
                          draw_cache->num_patch_coords,
                          draw_cache->edge_loose_len * 2);

    uint *edge_idx_data = (uint *)GPU_vertbuf_get_data(mbc->vbo.edge_idx);

    uint offset = subdiv_buffers.number_of_loops;
    LooseEdge *loose_edge = draw_cache->loose_edges;
    while (loose_edge) {
      edge_idx_data[offset] = loose_edge->coarse_edge_index;
      edge_idx_data[offset + 1] = loose_edge->coarse_edge_index;
      loose_edge = loose_edge->next;
      offset += 2;
    }
  }

  if (DRW_vbo_requested(mbc->vbo.poly_idx)) {
    init_origindex_buffer(
        mbc->vbo.poly_idx, draw_cache->subdiv_loop_poly_index, draw_cache->num_patch_coords, 0);
  }

  if (DRW_vbo_requested(mbc->vbo.edge_fac)) {
    GPU_vertbuf_init_build_on_device(mbc->vbo.edge_fac,
                                     get_edge_fac_format(),
                                     subdiv_buffers.number_of_loops + draw_cache->loop_loose_len);

    /* Create a temporary buffer for the edge original indices if it was not requested. */
    const bool has_edge_idx = mbc->vbo.edge_idx != NULL;
    GPUVertBuf *loop_edge_idx = NULL;
    if (has_edge_idx) {
      loop_edge_idx = mbc->vbo.edge_idx;
    }
    else {
      loop_edge_idx = GPU_vertbuf_calloc();
      init_origindex_buffer(
          loop_edge_idx, draw_cache->subdiv_loop_edge_index, draw_cache->num_patch_coords, 0);
    }

    do_build_edge_fac_buffer(
        &subdiv_buffers, mbc->vbo.pos_nor, loop_edge_idx, optimal_display, mbc->vbo.edge_fac);

    /* Make sure buffer is active for sending loose data. */
    GPU_vertbuf_use(mbc->vbo.edge_fac);

    LooseEdge *loose_edge = draw_cache->loose_edges;
    uint offset = draw_cache->num_patch_coords;
    float loose_edge_fac[2] = {1.0f, 1.0f};
    while (loose_edge) {
      GPU_vertbuf_update_sub(
          mbc->vbo.edge_fac, offset * sizeof(float), sizeof(loose_edge_fac), loose_edge_fac);
      loose_edge = loose_edge->next;
      offset += 2;
    }

    if (!has_edge_idx) {
      GPU_vertbuf_discard(loop_edge_idx);
    }
  }

  if (DRW_ibo_requested(mbc->ibo.points)) {
    GPUIndexBufBuilder builder;
    /* Copy the points as the data upload will free them. */
    builder.data = (uint *)MEM_dupallocN(draw_cache->point_indices);
    builder.index_len = draw_cache->num_vertices;
    builder.index_min = 0;
    builder.index_max = draw_cache->num_patch_coords - 1;
    builder.prim_type = GPU_PRIM_POINTS;

    if (draw_cache->loop_loose_len) {
      builder.data = MEM_reallocN(builder.data,
                                  sizeof(uint) *
                                      (draw_cache->num_patch_coords + draw_cache->loop_loose_len));

      uint offset = draw_cache->num_patch_coords;
      LooseEdge *loose_edge = draw_cache->loose_edges;
      while (loose_edge) {
        if (builder.data[loose_edge->v1] == -1u) {
          builder.data[loose_edge->v1] = offset;
        }
        if (builder.data[loose_edge->v2] == -1u) {
          builder.data[loose_edge->v2] = offset + 1;
        }
        builder.index_max += 2;
        builder.index_len += 2;
        offset += 2;
        loose_edge = loose_edge->next;
      }

      LooseVertex *loose_vert = draw_cache->loose_verts;
      while (loose_vert) {
        if (builder.data[loose_vert->coarse_vertex_index] == -1u) {
          builder.data[loose_vert->coarse_vertex_index] = offset;
        }
        builder.index_max += 1;
        builder.index_len += 1;
        offset += 1;
        loose_vert = loose_vert->next;
      }
    }

    GPU_indexbuf_build_in_place(&builder, mbc->ibo.points);
  }

  if (DRW_vbo_requested(mbc->vbo.edit_data)) {
    GPU_vertbuf_init_with_format(mbc->vbo.edit_data, get_edit_data_format());
    GPU_vertbuf_data_alloc(mbc->vbo.edit_data,
                           subdiv_buffers.number_of_loops + draw_cache->loop_loose_len);

    EditLoopData *edit_data = (EditLoopData *)GPU_vertbuf_get_data(mbc->vbo.edit_data);
    update_edit_data(draw_cache, mesh_eval, edit_data, toolsettings);
  }

  if (DRW_vbo_requested(mbc->vbo.uv)) {
    uint uv_layers;
    initialize_uv_buffer(
        mbc->vbo.uv, batch_cache, &mesh_eval->ldata, subdiv_buffers.number_of_loops, &uv_layers);

    if (uv_layers != 0) {
      draw_subdiv_extract_uvs(&subdiv_buffers, subdiv, mbc->vbo.uv, uv_layers);
    }
  }

  if (DRW_ibo_requested(mbc->ibo.lines_adjacency)) {
    struct MeshExtract_LineAdjacency_Data data;

    /* For each polygon there is (loop + triangle - 1) edges. Since we only have quads, and a quad
     * is split into 2 triangles, we have (loop + 2 - 1) = (loop + 1) edges for each quad, or in
     * total: (number_of_loops + number_of_quads). */
    uint tess_less = subdiv_buffers.number_of_loops + subdiv_buffers.number_of_quads;
    init_lines_adj(tess_less, draw_cache->num_vertices, draw_cache->num_patch_coords, &data);

    for (uint i = 0; i < subdiv_buffers.number_of_quads; i++) {
      const uint loop_index = i * 4;
      const uint l0 = loop_index + 0;
      const uint l1 = loop_index + 1;
      const uint l2 = loop_index + 2;
      const uint l3 = loop_index + 3;

      const uint v0 = draw_cache->subdiv_loop_subdiv_vert_index[l0];
      const uint v1 = draw_cache->subdiv_loop_subdiv_vert_index[l1];
      const uint v2 = draw_cache->subdiv_loop_subdiv_vert_index[l2];
      const uint v3 = draw_cache->subdiv_loop_subdiv_vert_index[l3];

      lines_adjacency_triangle(v0, v1, v2, l0, l1, l2, &data);
      lines_adjacency_triangle(v0, v2, v3, l0, l2, l3, &data);
    }

    GPU_indexbuf_build_in_place(&data.elb, mbc->ibo.lines_adjacency);
    BLI_edgehash_free(data.eh, NULL);
    MEM_freeN(data.vert_to_loop);
  }

  // draw_subdiv_cache_print_memory_used(draw_cache);
  return true;
}

static OpenSubdiv_EvaluatorCache *g_evaluator_cache = NULL;

void DRW_create_subdivision(const Scene *scene,
                            Object *ob,
                            Mesh *mesh,
                            struct MeshBatchCache *batch_cache,
                            MeshBufferCache *mbc,
                            const ToolSettings *toolsettings)
{
  if (g_evaluator_cache == NULL) {
    g_evaluator_cache = openSubdiv_createEvaluatorCache(OPENSUBDIV_EVALUATOR_GLSL_COMPUTE);
  }

#undef TIME_SUBDIV

#ifdef TIME_SUBDIV
  const double begin_time = PIL_check_seconds_timer();
#endif

  if (!draw_subdiv_create_requested_buffers(
          scene, ob, mesh, batch_cache, mbc, toolsettings, g_evaluator_cache)) {
    fprintf(stderr,
            "Cannot evaluate subdivision on the GPU, falling back to the regular draw code.\n");
    return;
  }

#ifdef TIME_SUBDIV
  const double end_time = PIL_check_seconds_timer();
  fprintf(stderr, "Time to update subdivision: %f\n", end_time - begin_time);
  fprintf(stderr, "Maximum FPS: %f\n", 1.0 / (end_time - begin_time));
#endif
}

void DRW_subdiv_free(void)
{
  for (int i = 0; i < NUM_SHADERS; ++i) {
    GPU_shader_free(g_subdiv_shaders[i]);
  }

  DRW_cache_free_old_subdiv();

  if (g_evaluator_cache) {
    openSubdiv_deleteEvaluatorCache(g_evaluator_cache);
    g_evaluator_cache = NULL;
  }
}

static LinkNode *gpu_subdiv_free_queue = NULL;
static ThreadMutex gpu_subdiv_queue_mutex = BLI_MUTEX_INITIALIZER;

void DRW_subdiv_cache_free(Subdiv *subdiv)
{
  BLI_mutex_lock(&gpu_subdiv_queue_mutex);
  BLI_linklist_prepend(&gpu_subdiv_free_queue, subdiv);
  BLI_mutex_unlock(&gpu_subdiv_queue_mutex);
}

void DRW_cache_free_old_subdiv()
{
  if (gpu_subdiv_free_queue == NULL) {
    return;
  }

  BLI_mutex_lock(&gpu_subdiv_queue_mutex);

  while (gpu_subdiv_free_queue != NULL) {
    Subdiv *subdiv = BLI_linklist_pop(&gpu_subdiv_free_queue);
    BKE_subdiv_free(subdiv);
  }

  BLI_mutex_unlock(&gpu_subdiv_queue_mutex);
}