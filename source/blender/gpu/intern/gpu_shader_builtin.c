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
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup gpu
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BKE_appdir.h"
#include "BKE_global.h"

#include "DNA_space_types.h"

#include "GPU_matrix.h"
#include "GPU_platform.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "GPU_uniform_buffer.h"

/* TODO(jbakker): Need a better way to retrieve create_infos. */
#include "gpu_shader_create_info_private.hh"

/* Adjust these constants as needed. */
#define MAX_DEFINE_LENGTH 256
#define MAX_EXT_DEFINE_LENGTH 512

/* Non-generated shaders */
extern char datatoc_gpu_shader_depth_only_frag_glsl[];
extern char datatoc_gpu_shader_uniform_color_frag_glsl[];
extern char datatoc_gpu_shader_checker_frag_glsl[];
extern char datatoc_gpu_shader_diag_stripes_frag_glsl[];
extern char datatoc_gpu_shader_simple_lighting_frag_glsl[];
extern char datatoc_gpu_shader_flat_color_frag_glsl[];
extern char datatoc_gpu_shader_flat_color_alpha_test_0_frag_glsl[];
extern char datatoc_gpu_shader_flat_id_frag_glsl[];
extern char datatoc_gpu_shader_2D_area_borders_vert_glsl[];
extern char datatoc_gpu_shader_2D_area_borders_frag_glsl[];
extern char datatoc_gpu_shader_2D_vert_glsl[];
extern char datatoc_gpu_shader_2D_flat_color_vert_glsl[];
extern char datatoc_gpu_shader_2D_smooth_color_uniform_alpha_vert_glsl[];
extern char datatoc_gpu_shader_2D_smooth_color_vert_glsl[];
extern char datatoc_gpu_shader_2D_smooth_color_frag_glsl[];
extern char datatoc_gpu_shader_2D_image_vert_glsl[];
extern char datatoc_gpu_shader_2D_image_rect_vert_glsl[];
extern char datatoc_gpu_shader_2D_image_multi_rect_vert_glsl[];
extern char datatoc_gpu_shader_2D_widget_base_vert_glsl[];
extern char datatoc_gpu_shader_2D_widget_base_frag_glsl[];
extern char datatoc_gpu_shader_2D_widget_shadow_vert_glsl[];
extern char datatoc_gpu_shader_2D_widget_shadow_frag_glsl[];
extern char datatoc_gpu_shader_2D_nodelink_frag_glsl[];
extern char datatoc_gpu_shader_2D_nodelink_vert_glsl[];

extern char datatoc_gpu_shader_3D_image_vert_glsl[];
extern char datatoc_gpu_shader_image_frag_glsl[];
extern char datatoc_gpu_shader_image_overlays_merge_frag_glsl[];
extern char datatoc_gpu_shader_image_overlays_stereo_merge_frag_glsl[];
extern char datatoc_gpu_shader_image_color_frag_glsl[];
extern char datatoc_gpu_shader_image_desaturate_frag_glsl[];
extern char datatoc_gpu_shader_image_modulate_alpha_frag_glsl[];
extern char datatoc_gpu_shader_image_varying_color_frag_glsl[];
extern char datatoc_gpu_shader_image_shuffle_color_frag_glsl[];
extern char datatoc_gpu_shader_3D_vert_glsl[];
extern char datatoc_gpu_shader_3D_normal_vert_glsl[];
extern char datatoc_gpu_shader_3D_flat_color_vert_glsl[];
extern char datatoc_gpu_shader_3D_polyline_frag_glsl[];
extern char datatoc_gpu_shader_3D_polyline_geom_glsl[];
extern char datatoc_gpu_shader_3D_polyline_vert_glsl[];
extern char datatoc_gpu_shader_3D_smooth_color_vert_glsl[];
extern char datatoc_gpu_shader_3D_smooth_color_frag_glsl[];
extern char datatoc_gpu_shader_3D_passthrough_vert_glsl[];
extern char datatoc_gpu_shader_3D_clipped_uniform_color_vert_glsl[];

extern char datatoc_gpu_shader_instance_variying_size_variying_color_vert_glsl[];

extern char datatoc_gpu_shader_point_uniform_color_aa_frag_glsl[];
extern char datatoc_gpu_shader_point_uniform_color_outline_aa_frag_glsl[];
extern char datatoc_gpu_shader_point_varying_color_varying_outline_aa_frag_glsl[];
extern char datatoc_gpu_shader_point_varying_color_frag_glsl[];
extern char datatoc_gpu_shader_3D_point_fixed_size_varying_color_vert_glsl[];
extern char datatoc_gpu_shader_3D_point_varying_size_varying_color_vert_glsl[];
extern char datatoc_gpu_shader_3D_point_uniform_size_aa_vert_glsl[];
extern char datatoc_gpu_shader_2D_point_uniform_size_aa_vert_glsl[];
extern char datatoc_gpu_shader_2D_point_uniform_size_outline_aa_vert_glsl[];

extern char datatoc_gpu_shader_2D_line_dashed_uniform_color_vert_glsl[];
extern char datatoc_gpu_shader_2D_line_dashed_frag_glsl[];
extern char datatoc_gpu_shader_3D_line_dashed_uniform_color_vert_glsl[];

extern char datatoc_gpu_shader_text_vert_glsl[];
extern char datatoc_gpu_shader_text_frag_glsl[];
extern char datatoc_gpu_shader_keyframe_shape_vert_glsl[];
extern char datatoc_gpu_shader_keyframe_shape_frag_glsl[];

extern char datatoc_gpu_shader_gpencil_stroke_vert_glsl[];
extern char datatoc_gpu_shader_gpencil_stroke_frag_glsl[];
extern char datatoc_gpu_shader_gpencil_stroke_geom_glsl[];

extern char datatoc_gpu_shader_cfg_world_clip_lib_glsl[];

extern char datatoc_gpu_shader_colorspace_lib_glsl[];

const struct GPUShaderConfigData GPU_shader_cfg_data[GPU_SHADER_CFG_LEN] = {
    [GPU_SHADER_CFG_DEFAULT] =
        {
            .lib = "",
            .def = "#define blender_srgb_to_framebuffer_space(a) a\n",
        },
    [GPU_SHADER_CFG_CLIPPED] =
        {
            .lib = datatoc_gpu_shader_cfg_world_clip_lib_glsl,
            .def = "#define USE_WORLD_CLIP_PLANES\n"
                   "#define blender_srgb_to_framebuffer_space(a) a\n",
        },
};

/* cache of built-in shaders (each is created on first use) */
static GPUShader *builtin_shaders[GPU_SHADER_CFG_LEN][GPU_SHADER_BUILTIN_LEN] = {{NULL}};

typedef struct {
  const char *name;
  const char *vert;
  /** Optional. */
  const char *geom;
  const char *frag;
  /** Optional. */
  const char *defs;

  const char *create_info;
  const char *clipped_create_info;
} GPUShaderStages;

static const GPUShaderStages builtin_shader_stages[GPU_SHADER_BUILTIN_LEN] = {
    [GPU_SHADER_TEXT] =
        {
            .name = "GPU_SHADER_TEXT",
            .create_info = "gpu_shader_text",
        },
    [GPU_SHADER_KEYFRAME_SHAPE] =
        {
            .name = "GPU_SHADER_KEYFRAME_SHAPE",
            .create_info = "gpu_shader_keyframe_shape",
        },
    [GPU_SHADER_SIMPLE_LIGHTING] =
        {
            .name = "GPU_SHADER_SIMPLE_LIGHTING",
            .create_info = "gpu_shader_simple_lighting",
        },
    [GPU_SHADER_3D_IMAGE_MODULATE_ALPHA] =
        {
            .name = "GPU_SHADER_3D_IMAGE_MODULATE_ALPHA",
            .create_info = "gpu_shader_3D_image_modulate_alpha",
        },
    [GPU_SHADER_2D_CHECKER] =
        {
            .name = "GPU_SHADER_2D_CHECKER",
            .create_info = "gpu_shader_2D_checker",
        },

    [GPU_SHADER_2D_DIAG_STRIPES] =
        {
            .name = "GPU_SHADER_2D_DIAG_STRIPES",
            .create_info = "gpu_shader_2D_diag_stripes",
        },

    [GPU_SHADER_2D_UNIFORM_COLOR] = {.name = "GPU_SHADER_2D_UNIFORM_COLOR",
                                     .create_info = "gpu_shader_2D_uniform_color"},
    [GPU_SHADER_2D_FLAT_COLOR] = {.name = "GPU_SHADER_2D_FLAT_COLOR",
                                  .create_info = "gpu_shader_2D_flat_color"},
    [GPU_SHADER_2D_SMOOTH_COLOR] = {.name = "GPU_SHADER_2D_SMOOTH_COLOR",
                                    .create_info = "gpu_shader_2D_smooth_color"},
    [GPU_SHADER_2D_IMAGE_OVERLAYS_MERGE] = {.name = "GPU_SHADER_2D_IMAGE_OVERLAYS_MERGE",
                                            .create_info = "gpu_shader_2D_image_overlays_merge"},
    [GPU_SHADER_2D_IMAGE_OVERLAYS_STEREO_MERGE] =
        {.name = "GPU_SHADER_2D_IMAGE_OVERLAYS_STEREO_MERGE",
         .create_info = "gpu_shader_2D_image_overlays_stereo_merge"},
    [GPU_SHADER_2D_IMAGE] = {.name = "GPU_SHADER_2D_IMAGE", .create_info = "gpu_shader_2D_image"},
    [GPU_SHADER_2D_IMAGE_COLOR] = {.name = "GPU_SHADER_2D_IMAGE_COLOR",
                                   .create_info = "gpu_shader_2D_image_color"},
    [GPU_SHADER_2D_IMAGE_DESATURATE_COLOR] = {.name = "GPU_SHADER_2D_IMAGE_DESATURATE_COLOR",
                                              .create_info =
                                                  "gpu_shader_2D_image_desaturate_color"},
    [GPU_SHADER_2D_IMAGE_SHUFFLE_COLOR] =
        {
            .name = "GPU_SHADER_2D_IMAGE_SHUFFLE_COLOR",
            .create_info = "gpu_shader_2D_image_shuffle_color",
        },
    [GPU_SHADER_2D_IMAGE_RECT_COLOR] = {.name = "GPU_SHADER_2D_IMAGE_RECT_COLOR",
                                        .create_info = "gpu_shader_2D_image_rect_color"},
    [GPU_SHADER_2D_IMAGE_MULTI_RECT_COLOR] = {.name = "GPU_SHADER_2D_IMAGE_MULTI_RECT_COLOR",
                                              .create_info =
                                                  "gpu_shader_2D_image_multi_rect_color"},

    [GPU_SHADER_3D_UNIFORM_COLOR] =
        {
            .name = "GPU_SHADER_3D_UNIFORM_COLOR",
            .create_info = "gpu_shader_3D_uniform_color",
            .clipped_create_info = "gpu_shader_3D_uniform_color_clipped",
        },
    [GPU_SHADER_3D_FLAT_COLOR] = {.name = "GPU_SHADER_3D_FLAT_COLOR",
                                  .create_info = "gpu_shader_3D_flat_color",
                                  .clipped_create_info = "gpu_shader_3D_flat_color_clipped"},
    [GPU_SHADER_3D_SMOOTH_COLOR] = {.name = "GPU_SHADER_3D_SMOOTH_COLOR",
                                    .create_info = "gpu_shader_3D_smooth_color",
                                    .clipped_create_info = "gpu_shader_3D_smooth_color_clipped"},
    [GPU_SHADER_3D_DEPTH_ONLY] = {.name = "GPU_SHADER_3D_DEPTH_ONLY",
                                  .create_info = "gpu_shader_3D_depth_only",
                                  .clipped_create_info = "gpu_shader_3D_depth_only_clipped"},
    [GPU_SHADER_3D_CLIPPED_UNIFORM_COLOR] =
        {
            .name = "GPU_SHADER_3D_CLIPPED_UNIFORM_COLOR",
            .vert = datatoc_gpu_shader_3D_clipped_uniform_color_vert_glsl,
            .frag = datatoc_gpu_shader_uniform_color_frag_glsl,
        },

    [GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR] =
        {
            .name = "GPU_SHADER_3D_POLYLINE_UNIFORM_COLOR",
            .vert = datatoc_gpu_shader_3D_polyline_vert_glsl,
            .geom = datatoc_gpu_shader_3D_polyline_geom_glsl,
            .frag = datatoc_gpu_shader_3D_polyline_frag_glsl,
            .defs = "#define UNIFORM\n",
        },
    [GPU_SHADER_3D_POLYLINE_CLIPPED_UNIFORM_COLOR] =
        {
            .name = "GPU_SHADER_3D_POLYLINE_CLIPPED_UNIFORM_COLOR",
            .vert = datatoc_gpu_shader_3D_polyline_vert_glsl,
            .geom = datatoc_gpu_shader_3D_polyline_geom_glsl,
            .frag = datatoc_gpu_shader_3D_polyline_frag_glsl,
            .defs = "#define UNIFORM\n"
                    "#define CLIP\n",
        },
    [GPU_SHADER_3D_POLYLINE_FLAT_COLOR] =
        {
            .name = "GPU_SHADER_3D_POLYLINE_FLAT_COLOR",
            .vert = datatoc_gpu_shader_3D_polyline_vert_glsl,
            .geom = datatoc_gpu_shader_3D_polyline_geom_glsl,
            .frag = datatoc_gpu_shader_3D_polyline_frag_glsl,
            .defs = "#define FLAT\n",
        },
    [GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR] =
        {
            .name = "GPU_SHADER_3D_POLYLINE_SMOOTH_COLOR",
            .vert = datatoc_gpu_shader_3D_polyline_vert_glsl,
            .geom = datatoc_gpu_shader_3D_polyline_geom_glsl,
            .frag = datatoc_gpu_shader_3D_polyline_frag_glsl,
            .defs = "#define SMOOTH\n",
        },

    [GPU_SHADER_2D_LINE_DASHED_UNIFORM_COLOR] =
        {
            .name = "GPU_SHADER_2D_LINE_DASHED_UNIFORM_COLOR",
            .vert = datatoc_gpu_shader_2D_line_dashed_uniform_color_vert_glsl,
            .frag = datatoc_gpu_shader_2D_line_dashed_frag_glsl,
        },
    [GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR] =
        {
            .name = "GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR",
            .vert = datatoc_gpu_shader_3D_line_dashed_uniform_color_vert_glsl,
            .frag = datatoc_gpu_shader_2D_line_dashed_frag_glsl,
        },

    [GPU_SHADER_2D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA] =
        {
            .name = "GPU_SHADER_2D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA",
            .create_info = "gpu_shader_2D_point_uniform_size_uniform_color_aa",
        },
    [GPU_SHADER_2D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_OUTLINE_AA] =
        {
            .name = "GPU_SHADER_2D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_OUTLINE_AA",
            .create_info = "gpu_shader_2D_point_uniform_size_uniform_color_outline_aa",
        },
    [GPU_SHADER_3D_POINT_FIXED_SIZE_VARYING_COLOR] =
        {.name = "GPU_SHADER_3D_POINT_FIXED_SIZE_VARYING_COLOR",
         .create_info = "gpu_shader_3D_point_fixed_size_varying_color"},
    [GPU_SHADER_3D_POINT_VARYING_SIZE_VARYING_COLOR] =
        {.name = "GPU_SHADER_3D_POINT_VARYING_SIZE_VARYING_COLOR",
         .create_info = "gpu_shader_3D_point_varying_size_varying_color"},
    [GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA] =
        {.name = "GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA",
         .create_info = "gpu_shader_3D_point_uniform_size_uniform_color_aa",
         .clipped_create_info = "gpu_shader_3D_point_uniform_size_uniform_color_aa_clipped"},

    [GPU_SHADER_INSTANCE_VARIYING_COLOR_VARIYING_SIZE] =
        {
            .name = "GPU_SHADER_INSTANCE_VARIYING_COLOR_VARIYING_SIZE",
            .vert = datatoc_gpu_shader_instance_variying_size_variying_color_vert_glsl,
            .frag = datatoc_gpu_shader_flat_color_frag_glsl,
            .defs = "#define UNIFORM_SCALE\n",
        },

    [GPU_SHADER_2D_AREA_BORDERS] = {.name = "GPU_SHADER_2D_AREA_BORDERS",
                                    .create_info = "gpu_shader_2D_area_borders"},
    [GPU_SHADER_2D_WIDGET_BASE] =
        {
            .name = "GPU_SHADER_2D_WIDGET_BASE",
            .vert = datatoc_gpu_shader_2D_widget_base_vert_glsl,
            .frag = datatoc_gpu_shader_2D_widget_base_frag_glsl,
        },
    [GPU_SHADER_2D_WIDGET_BASE_INST] =
        {
            .name = "GPU_SHADER_2D_WIDGET_BASE_INST",
            .vert = datatoc_gpu_shader_2D_widget_base_vert_glsl,
            .frag = datatoc_gpu_shader_2D_widget_base_frag_glsl,
            .defs = "#define USE_INSTANCE\n",
        },
    [GPU_SHADER_2D_WIDGET_SHADOW] =
        {
            .name = "GPU_SHADER_2D_WIDGET_SHADOW",
            .vert = datatoc_gpu_shader_2D_widget_shadow_vert_glsl,
            .frag = datatoc_gpu_shader_2D_widget_shadow_frag_glsl,
        },
    [GPU_SHADER_2D_NODELINK] = {.name = "GPU_SHADER_2D_NODELINK",
                                .create_info = "gpu_shader_2D_nodelink"},

    [GPU_SHADER_2D_NODELINK_INST] = {.name = "GPU_SHADER_2D_NODELINK_INST",
                                     .create_info = "gpu_shader_2D_nodelink_inst"},

    [GPU_SHADER_GPENCIL_STROKE] = {.name = "GPU_SHADER_GPENCIL_STROKE",
                                   .create_info = "gpu_shader_gpencil_stroke"},
};

GPUShader *GPU_shader_get_builtin_shader_with_config(eGPUBuiltinShader shader,
                                                     eGPUShaderConfig sh_cfg)
{
  BLI_assert(shader < GPU_SHADER_BUILTIN_LEN);
  BLI_assert(sh_cfg < GPU_SHADER_CFG_LEN);
  GPUShader **sh_p = &builtin_shaders[sh_cfg][shader];

  if (*sh_p == NULL) {
    const GPUShaderStages *stages = &builtin_shader_stages[shader];

    /* common case */
    if (sh_cfg == GPU_SHADER_CFG_DEFAULT) {
      if (stages->create_info != NULL) {
        *sh_p = GPU_shader_create_from_info(gpu_shader_create_info_get(stages->create_info));
      }
      else {
        *sh_p = GPU_shader_create_from_arrays_named(
            stages->name,
            {
                .vert = (const char *[]){stages->vert, NULL},
                .geom = (const char *[]){stages->geom, NULL},
                .frag =
                    (const char *[]){datatoc_gpu_shader_colorspace_lib_glsl, stages->frag, NULL},
                .defs = (const char *[]){stages->defs, NULL},
            });
      }
    }
    else if (sh_cfg == GPU_SHADER_CFG_CLIPPED) {
      /* Remove eventually, for now ensure support for each shader has been added. */
      BLI_assert(ELEM(shader,
                      GPU_SHADER_3D_UNIFORM_COLOR,
                      GPU_SHADER_3D_SMOOTH_COLOR,
                      GPU_SHADER_3D_DEPTH_ONLY,
                      GPU_SHADER_INSTANCE_VARIYING_COLOR_VARIYING_SIZE,
                      GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA,
                      GPU_SHADER_3D_FLAT_COLOR,
                      GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR));
      /* In rare cases geometry shaders calculate clipping themselves. */
      if (stages->clipped_create_info != NULL) {
        *sh_p = GPU_shader_create_from_info(
            gpu_shader_create_info_get(stages->clipped_create_info));
      }
      else {
        const char *world_clip_lib = datatoc_gpu_shader_cfg_world_clip_lib_glsl;
        const char *world_clip_def = "#define USE_WORLD_CLIP_PLANES\n";
        *sh_p = GPU_shader_create_from_arrays_named(
            stages->name,
            {
                .vert = (const char *[]){world_clip_lib, stages->vert, NULL},
                .geom = (const char *[]){stages->geom ? world_clip_lib : NULL, stages->geom, NULL},
                .frag =
                    (const char *[]){datatoc_gpu_shader_colorspace_lib_glsl, stages->frag, NULL},
                .defs = (const char *[]){world_clip_def, stages->defs, NULL},
            });
      }
    }
    else {
      BLI_assert(0);
    }
  }

  return *sh_p;
}

GPUShader *GPU_shader_get_builtin_shader(eGPUBuiltinShader shader)
{
  return GPU_shader_get_builtin_shader_with_config(shader, GPU_SHADER_CFG_DEFAULT);
}

void GPU_shader_get_builtin_shader_code(eGPUBuiltinShader shader,
                                        const char **r_vert,
                                        const char **r_frag,
                                        const char **r_geom,
                                        const char **r_defines)
{
  const GPUShaderStages *stages = &builtin_shader_stages[shader];
  *r_vert = stages->vert;
  *r_frag = stages->frag;
  *r_geom = stages->geom;
  *r_defines = stages->defs;
}

void GPU_shader_free_builtin_shaders(void)
{
  for (int i = 0; i < GPU_SHADER_CFG_LEN; i++) {
    for (int j = 0; j < GPU_SHADER_BUILTIN_LEN; j++) {
      if (builtin_shaders[i][j]) {
        GPU_shader_free(builtin_shaders[i][j]);
        builtin_shaders[i][j] = NULL;
      }
    }
  }
}
