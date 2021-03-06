/*
 * GStreamer
 * Copyright (C) 2014 Lubosz Sarnecki <lubosz@gmail.com>
 * Copyright (C) 2016 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-gltransformation
 *
 * Transforms video on the GPU.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 gltestsrc ! gltransformation rotation-z=45 ! glimagesink
 * ]| A pipeline to rotate by 45 degrees
 * |[
 * gst-launch-1.0 gltestsrc ! gltransformation translation-x=0.5 ! glimagesink
 * ]| Translate the video by 0.5
 * |[
 * gst-launch-1.0 gltestsrc ! gltransformation scale-y=0.5 scale-x=0.5 ! glimagesink
 * ]| Resize the video by 0.5
 * |[
 * gst-launch-1.0 gltestsrc ! gltransformation rotation-x=-45 ortho=True ! glimagesink
 * ]| Rotate the video around the X-Axis by -45° with an orthographic projection
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstgltransformation.h"

#include <gst/gl/gstglapi.h>
#include <graphene-gobject.h>

#define GST_CAT_DEFAULT gst_gl_transformation_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define gst_gl_transformation_parent_class parent_class

enum
{
  PROP_0,
  PROP_FOV,
  PROP_ORTHO,
  PROP_TRANSLATION_X,
  PROP_TRANSLATION_Y,
  PROP_TRANSLATION_Z,
  PROP_ROTATION_X,
  PROP_ROTATION_Y,
  PROP_ROTATION_Z,
  PROP_SCALE_X,
  PROP_SCALE_Y,
  PROP_MVP,
  PROP_PIVOT_X,
  PROP_PIVOT_Y,
  PROP_PIVOT_Z,
};

#define DEBUG_INIT \
    GST_DEBUG_CATEGORY_INIT (gst_gl_transformation_debug, "gltransformation", 0, "gltransformation element");

G_DEFINE_TYPE_WITH_CODE (GstGLTransformation, gst_gl_transformation,
    GST_TYPE_GL_FILTER, DEBUG_INIT);

static void gst_gl_transformation_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_transformation_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_gl_transformation_set_caps (GstGLFilter * filter,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_gl_transformation_src_event (GstBaseTransform * trans,
    GstEvent * event);
static gboolean gst_gl_transformation_filter_meta (GstBaseTransform * trans,
    GstQuery * query, GType api, const GstStructure * params);
static gboolean gst_gl_transformation_decide_allocation (GstBaseTransform *
    trans, GstQuery * query);

static void gst_gl_transformation_reset_gl (GstGLFilter * filter);
static gboolean gst_gl_transformation_stop (GstBaseTransform * trans);
static gboolean gst_gl_transformation_init_shader (GstGLFilter * filter);
static void gst_gl_transformation_callback (gpointer stuff);
static void gst_gl_transformation_build_mvp (GstGLTransformation *
    transformation);

static GstFlowReturn
gst_gl_transformation_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf);
static gboolean gst_gl_transformation_filter (GstGLFilter * filter,
    GstBuffer * inbuf, GstBuffer * outbuf);
static gboolean gst_gl_transformation_filter_texture (GstGLFilter * filter,
    guint in_tex, guint out_tex);

static void
gst_gl_transformation_class_init (GstGLTransformationClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseTransformClass *base_transform_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);
  base_transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_gl_transformation_set_property;
  gobject_class->get_property = gst_gl_transformation_get_property;

  base_transform_class->src_event = gst_gl_transformation_src_event;
  base_transform_class->decide_allocation =
      gst_gl_transformation_decide_allocation;
  base_transform_class->filter_meta = gst_gl_transformation_filter_meta;

  GST_GL_FILTER_CLASS (klass)->init_fbo = gst_gl_transformation_init_shader;
  GST_GL_FILTER_CLASS (klass)->display_reset_cb =
      gst_gl_transformation_reset_gl;
  GST_GL_FILTER_CLASS (klass)->set_caps = gst_gl_transformation_set_caps;
  GST_GL_FILTER_CLASS (klass)->filter = gst_gl_transformation_filter;
  GST_GL_FILTER_CLASS (klass)->filter_texture =
      gst_gl_transformation_filter_texture;
  GST_BASE_TRANSFORM_CLASS (klass)->stop = gst_gl_transformation_stop;
  GST_BASE_TRANSFORM_CLASS (klass)->prepare_output_buffer =
      gst_gl_transformation_prepare_output_buffer;

  g_object_class_install_property (gobject_class, PROP_FOV,
      g_param_spec_float ("fov", "Fov", "Field of view angle in degrees",
          0.0, G_MAXFLOAT, 90.0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ORTHO,
      g_param_spec_boolean ("ortho", "Orthographic",
          "Use orthographic projection", FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Rotation */
  g_object_class_install_property (gobject_class, PROP_ROTATION_X,
      g_param_spec_float ("rotation-x", "X Rotation",
          "Rotates the video around the X-Axis in degrees.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ROTATION_Y,
      g_param_spec_float ("rotation-y", "Y Rotation",
          "Rotates the video around the Y-Axis in degrees.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ROTATION_Z,
      g_param_spec_float ("rotation-z", "Z Rotation",
          "Rotates the video around the Z-Axis in degrees.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Translation */
  g_object_class_install_property (gobject_class, PROP_TRANSLATION_X,
      g_param_spec_float ("translation-x", "X Translation",
          "Translates the video at the X-Axis, in universal [0-1] coordinate.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_TRANSLATION_Y,
      g_param_spec_float ("translation-y", "Y Translation",
          "Translates the video at the Y-Axis, in universal [0-1] coordinate.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_TRANSLATION_Z,
      g_param_spec_float ("translation-z", "Z Translation",
          "Translates the video at the Z-Axis, in universal [0-1] coordinate.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Scale */
  g_object_class_install_property (gobject_class, PROP_SCALE_X,
      g_param_spec_float ("scale-x", "X Scale",
          "Scale multiplier for the X-Axis.",
          -G_MAXFLOAT, G_MAXFLOAT, 1.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SCALE_Y,
      g_param_spec_float ("scale-y", "Y Scale",
          "Scale multiplier for the Y-Axis.",
          -G_MAXFLOAT, G_MAXFLOAT, 1.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Pivot */
  g_object_class_install_property (gobject_class, PROP_PIVOT_X,
      g_param_spec_float ("pivot-x", "X Pivot",
          "Rotation pivot point X coordinate, where 0 is the center,"
          " -1 the lef +1 the right boarder and <-1, >1 outside.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PIVOT_Y,
      g_param_spec_float ("pivot-y", "Y Pivot",
          "Rotation pivot point X coordinate, where 0 is the center,"
          " -1 the lef +1 the right boarder and <-1, >1 outside.",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PIVOT_Z,
      g_param_spec_float ("pivot-z", "Z Pivot",
          "Relevant for rotation in 3D space. You look into the negative Z axis direction",
          -G_MAXFLOAT, G_MAXFLOAT, 0.0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* MVP */
  g_object_class_install_property (gobject_class, PROP_MVP,
      g_param_spec_boxed ("mvp-matrix",
          "Modelview Projection Matrix",
          "The final Graphene 4x4 Matrix for transformation",
          GRAPHENE_TYPE_MATRIX, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_metadata (element_class, "OpenGL transformation filter",
      "Filter/Effect/Video", "Transform video on the GPU",
      "Lubosz Sarnecki <lubosz@gmail.com>\n"
      "Matthew Waters <matthew@centricular.com>");

  GST_GL_BASE_FILTER_CLASS (klass)->supported_gl_api =
      GST_GL_API_OPENGL | GST_GL_API_OPENGL3 | GST_GL_API_GLES2;
}

static void
gst_gl_transformation_init (GstGLTransformation * filter)
{
  filter->shader = NULL;
  filter->fov = 90;
  filter->aspect = 1.0;
  filter->znear = 0.1;
  filter->zfar = 100;

  filter->xscale = 1.0;
  filter->yscale = 1.0;

  filter->in_tex = 0;

  gst_gl_transformation_build_mvp (filter);
}

static void
gst_gl_transformation_build_mvp (GstGLTransformation * transformation)
{
  GstGLFilter *filter = GST_GL_FILTER (transformation);
  graphene_matrix_t modelview_matrix;

  if (!filter->out_info.finfo) {
    graphene_matrix_init_identity (&transformation->model_matrix);
    graphene_matrix_init_identity (&transformation->view_matrix);
    graphene_matrix_init_identity (&transformation->projection_matrix);
  } else {
    graphene_point3d_t translation_vector =
        GRAPHENE_POINT3D_INIT (transformation->xtranslation * 2.0 *
        transformation->aspect,
        transformation->ytranslation * 2.0,
        transformation->ztranslation * 2.0);

    graphene_point3d_t pivot_vector =
        GRAPHENE_POINT3D_INIT (-transformation->xpivot * transformation->aspect,
        transformation->ypivot,
        -transformation->zpivot);

    graphene_point3d_t negative_pivot_vector;

    graphene_vec3_t eye;
    graphene_vec3_t center;
    graphene_vec3_t up;

    gboolean current_passthrough;
    gboolean passthrough;

    graphene_vec3_init (&eye, 0.f, 0.f, 1.f);
    graphene_vec3_init (&center, 0.f, 0.f, 0.f);
    graphene_vec3_init (&up, 0.f, 1.f, 0.f);

    /* Translate into pivot origin */
    graphene_matrix_init_translate (&transformation->model_matrix,
        &pivot_vector);

    /* Scale */
    graphene_matrix_scale (&transformation->model_matrix,
        transformation->xscale, transformation->yscale, 1.0f);

    /* Rotation */
    graphene_matrix_rotate (&transformation->model_matrix,
        transformation->xrotation, graphene_vec3_x_axis ());
    graphene_matrix_rotate (&transformation->model_matrix,
        transformation->yrotation, graphene_vec3_y_axis ());
    graphene_matrix_rotate (&transformation->model_matrix,
        transformation->zrotation, graphene_vec3_z_axis ());

    /* Translate back from pivot origin */
    graphene_point3d_scale (&pivot_vector, -1.0, &negative_pivot_vector);
    graphene_matrix_translate (&transformation->model_matrix,
        &negative_pivot_vector);

    /* Translation */
    graphene_matrix_translate (&transformation->model_matrix,
        &translation_vector);

    if (transformation->ortho) {
      graphene_matrix_init_ortho (&transformation->projection_matrix,
          -transformation->aspect, transformation->aspect,
          -1, 1, transformation->znear, transformation->zfar);
    } else {
      graphene_matrix_init_perspective (&transformation->projection_matrix,
          transformation->fov,
          transformation->aspect, transformation->znear, transformation->zfar);
    }

    graphene_matrix_init_look_at (&transformation->view_matrix, &eye, &center,
        &up);

    current_passthrough =
        gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (transformation));
    passthrough = transformation->xtranslation == 0.
        && transformation->ytranslation == 0.
        && transformation->ztranslation == 0. && transformation->xrotation == 0.
        && transformation->yrotation == 0. && transformation->zrotation == 0.
        && transformation->xscale == 1. && transformation->yscale == 1.
        && gst_video_info_is_equal (&filter->in_info, &filter->out_info);
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (transformation),
        passthrough);
    if (current_passthrough != passthrough) {
      gst_base_transform_reconfigure_src (GST_BASE_TRANSFORM (transformation));
    }
  }

  graphene_matrix_multiply (&transformation->model_matrix,
      &transformation->view_matrix, &modelview_matrix);
  graphene_matrix_multiply (&modelview_matrix,
      &transformation->projection_matrix, &transformation->mvp_matrix);
}

static void
gst_gl_transformation_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLTransformation *filter = GST_GL_TRANSFORMATION (object);

  switch (prop_id) {
    case PROP_FOV:
      filter->fov = g_value_get_float (value);
      break;
    case PROP_ORTHO:
      filter->ortho = g_value_get_boolean (value);
      break;
    case PROP_TRANSLATION_X:
      filter->xtranslation = g_value_get_float (value);
      break;
    case PROP_TRANSLATION_Y:
      filter->ytranslation = g_value_get_float (value);
      break;
    case PROP_TRANSLATION_Z:
      filter->ztranslation = g_value_get_float (value);
      break;
    case PROP_ROTATION_X:
      filter->xrotation = g_value_get_float (value);
      break;
    case PROP_ROTATION_Y:
      filter->yrotation = g_value_get_float (value);
      break;
    case PROP_ROTATION_Z:
      filter->zrotation = g_value_get_float (value);
      break;
    case PROP_SCALE_X:
      filter->xscale = g_value_get_float (value);
      break;
    case PROP_SCALE_Y:
      filter->yscale = g_value_get_float (value);
      break;
    case PROP_PIVOT_X:
      filter->xpivot = g_value_get_float (value);
      break;
    case PROP_PIVOT_Y:
      filter->ypivot = g_value_get_float (value);
      break;
    case PROP_PIVOT_Z:
      filter->zpivot = g_value_get_float (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  gst_gl_transformation_build_mvp (filter);
}

static void
gst_gl_transformation_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLTransformation *filter = GST_GL_TRANSFORMATION (object);

  switch (prop_id) {
    case PROP_FOV:
      g_value_set_float (value, filter->fov);
      break;
    case PROP_ORTHO:
      g_value_set_boolean (value, filter->ortho);
      break;
    case PROP_TRANSLATION_X:
      g_value_set_float (value, filter->xtranslation);
      break;
    case PROP_TRANSLATION_Y:
      g_value_set_float (value, filter->ytranslation);
      break;
    case PROP_TRANSLATION_Z:
      g_value_set_float (value, filter->ztranslation);
      break;
    case PROP_ROTATION_X:
      g_value_set_float (value, filter->xrotation);
      break;
    case PROP_ROTATION_Y:
      g_value_set_float (value, filter->yrotation);
      break;
    case PROP_ROTATION_Z:
      g_value_set_float (value, filter->zrotation);
      break;
    case PROP_SCALE_X:
      g_value_set_float (value, filter->xscale);
      break;
    case PROP_SCALE_Y:
      g_value_set_float (value, filter->yscale);
      break;
    case PROP_PIVOT_X:
      g_value_set_float (value, filter->xpivot);
      break;
    case PROP_PIVOT_Y:
      g_value_set_float (value, filter->ypivot);
      break;
    case PROP_PIVOT_Z:
      g_value_set_float (value, filter->zpivot);
      break;
    case PROP_MVP:
      g_value_set_boxed (value, (gconstpointer) & filter->mvp_matrix);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_gl_transformation_set_caps (GstGLFilter * filter, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (filter);

  transformation->aspect =
      (gdouble) GST_VIDEO_INFO_WIDTH (&filter->out_info) /
      (gdouble) GST_VIDEO_INFO_HEIGHT (&filter->out_info);

  transformation->caps_change = TRUE;

  gst_gl_transformation_build_mvp (transformation);

  return TRUE;
}

static void
_find_plane_normal (const graphene_point3d_t * A, const graphene_point3d_t * B,
    const graphene_point3d_t * C, graphene_vec3_t * plane_normal)
{
  graphene_vec3_t U, V, A_v, B_v, C_v;

  graphene_point3d_to_vec3 (A, &A_v);
  graphene_point3d_to_vec3 (B, &B_v);
  graphene_point3d_to_vec3 (C, &C_v);

  graphene_vec3_subtract (&B_v, &A_v, &U);
  graphene_vec3_subtract (&C_v, &A_v, &V);

  graphene_vec3_cross (&U, &V, plane_normal);
  graphene_vec3_normalize (plane_normal, plane_normal);
}

static void
_find_model_coords (GstGLTransformation * transformation,
    const graphene_point3d_t * screen_coords, graphene_point3d_t * res)
{
  graphene_matrix_t modelview, inverse_proj, inverse_modelview;
  graphene_vec4_t v1, v2;
  graphene_point3d_t p1;
  gfloat w;

  graphene_vec4_init (&v1, screen_coords->x, screen_coords->y, screen_coords->z,
      1.);
  graphene_matrix_inverse (&transformation->projection_matrix, &inverse_proj);
  graphene_matrix_transform_vec4 (&inverse_proj, &v1, &v2);

  /* perspective division */
  w = graphene_vec4_get_w (&v2);
  p1.x = graphene_vec4_get_x (&v2) / w;
  p1.y = graphene_vec4_get_y (&v2) / w;
  p1.z = graphene_vec4_get_z (&v2) / w;

  graphene_matrix_multiply (&transformation->model_matrix,
      &transformation->view_matrix, &modelview);
  graphene_matrix_inverse (&modelview, &inverse_modelview);
  graphene_matrix_transform_point3d (&inverse_modelview, &p1, res);
}

static gboolean
gst_gl_transformation_src_event (GstBaseTransform * trans, GstEvent * event)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (trans);
  GstGLFilter *filter = GST_GL_FILTER (trans);
  gdouble new_x, new_y, x, y;
  GstStructure *structure;
  gboolean ret;

  GST_DEBUG_OBJECT (trans, "handling %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_NAVIGATION:
      event =
          GST_EVENT (gst_mini_object_make_writable (GST_MINI_OBJECT (event)));

      structure = (GstStructure *) gst_event_get_structure (event);
      if (gst_structure_get_double (structure, "pointer_x", &x) &&
          gst_structure_get_double (structure, "pointer_y", &y)) {
        gfloat w = (gfloat) GST_VIDEO_INFO_WIDTH (&filter->in_info);
        gfloat h = (gfloat) GST_VIDEO_INFO_HEIGHT (&filter->in_info);
        graphene_point3d_t screen_point_near, screen_point_far;
        graphene_point3d_t model_coord_near, model_coord_far;
        graphene_point3d_t bottom_left, bottom_right, top_left, top_right;
        graphene_point3d_t result;
        graphene_vec3_t plane_normal;
        graphene_plane_t video_plane;
        gfloat d;

        GST_DEBUG_OBJECT (trans, "converting %f,%f", x, y);

        graphene_point3d_init (&top_left, -1., 1., 0.);
        graphene_point3d_init (&top_right, 1., 1., 0.);
        graphene_point3d_init (&bottom_left, -1., -1., 0.);
        graphene_point3d_init (&bottom_right, 1., -1., 0.);
        /* to NDC */
        graphene_point3d_init (&screen_point_near, 2. * x / w - 1.,
            2. * y / h - 1., -1.);
        graphene_point3d_init (&screen_point_far, 2. * x / w - 1.,
            2. * y / h - 1., 1.);

        _find_plane_normal (&bottom_left, &top_left, &top_right, &plane_normal);
        graphene_plane_init_from_point (&video_plane, &plane_normal, &top_left);
        d = graphene_plane_get_constant (&video_plane);

        /* get the closest and furthest points in the viewing area for the
         * specified screen coordinate in order to construct a ray */
        _find_model_coords (transformation, &screen_point_near,
            &model_coord_near);
        _find_model_coords (transformation, &screen_point_far,
            &model_coord_far);

        {
          graphene_vec3_t model_coord_near_vec3, model_coord_far_vec3;
          graphene_vec3_t tmp, intersection, coord_dir;
          gfloat num, denom, t;

          /* get the direction of the ray */
          graphene_point3d_to_vec3 (&model_coord_near, &model_coord_near_vec3);
          graphene_point3d_to_vec3 (&model_coord_far, &model_coord_far_vec3);
          graphene_vec3_subtract (&model_coord_near_vec3, &model_coord_far_vec3,
              &coord_dir);

          /* Intersect the ray with the video plane to find the distance, t:
           * Ray: P = P0 + t Pdir
           * Plane: P dot N + d = 0
           *
           * Substituting for P and rearranging gives:
           *
           * t = (P0 dot N + d) / (Pdir dot N) */
          denom = graphene_vec3_dot (&coord_dir, &plane_normal);
          num = graphene_vec3_dot (&model_coord_near_vec3, &plane_normal);
          t = -(num + d) / denom;

          /* video coord = P0 + t Pdir */
          graphene_vec3_scale (&coord_dir, t, &tmp);
          graphene_vec3_add (&tmp, &model_coord_near_vec3, &intersection);
          graphene_point3d_init_from_vec3 (&result, &intersection);
        }

        new_x = (result.x / transformation->aspect + 1.) * w / 2;
        new_y = (result.y + 1.) * h / 2;

        if (new_x < 0. || new_x > w || new_y < 0 || new_y > h) {
          /* coords off video surface */
          gst_event_unref (event);
          return TRUE;
        }

        GST_DEBUG_OBJECT (trans, "to %fx%f", new_x, new_y);
        gst_structure_set (structure, "pointer_x", G_TYPE_DOUBLE, new_x,
            "pointer_y", G_TYPE_DOUBLE, new_y, NULL);
      }
      break;
    default:
      break;
  }

  ret = GST_BASE_TRANSFORM_CLASS (parent_class)->src_event (trans, event);

  return ret;
}

static gboolean
gst_gl_transformation_filter_meta (GstBaseTransform * trans, GstQuery * query,
    GType api, const GstStructure * params)
{
  if (api == GST_VIDEO_AFFINE_TRANSFORMATION_META_API_TYPE)
    return TRUE;

  if (api == GST_GL_SYNC_META_API_TYPE)
    return TRUE;

  return FALSE;
}

static gboolean
gst_gl_transformation_decide_allocation (GstBaseTransform * trans,
    GstQuery * query)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (trans);

  if (!GST_BASE_TRANSFORM_CLASS (parent_class)->decide_allocation (trans,
          query))
    return FALSE;

  if (gst_query_find_allocation_meta (query,
          GST_VIDEO_AFFINE_TRANSFORMATION_META_API_TYPE, NULL)) {
    transformation->downstream_supports_affine_meta = TRUE;
  } else {
    transformation->downstream_supports_affine_meta = FALSE;
  }

  return TRUE;
}

static void
gst_gl_transformation_reset_gl (GstGLFilter * filter)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (filter);
  const GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  if (transformation->vao) {
    gl->DeleteVertexArrays (1, &transformation->vao);
    transformation->vao = 0;
  }

  if (transformation->vertex_buffer) {
    gl->DeleteBuffers (1, &transformation->vertex_buffer);
    transformation->vertex_buffer = 0;
  }

  if (transformation->vbo_indices) {
    gl->DeleteBuffers (1, &transformation->vbo_indices);
    transformation->vbo_indices = 0;
  }

  if (transformation->shader) {
    gst_object_unref (transformation->shader);
    transformation->shader = NULL;
  }
}

static gboolean
gst_gl_transformation_stop (GstBaseTransform * trans)
{
  GstGLBaseFilter *basefilter = GST_GL_BASE_FILTER (trans);
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (trans);

  /* blocking call, wait until the opengl thread has destroyed the shader */
  if (basefilter->context && transformation->shader) {
    gst_gl_context_del_shader (basefilter->context, transformation->shader);
    transformation->shader = NULL;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->stop (trans);
}

static gboolean
gst_gl_transformation_init_shader (GstGLFilter * filter)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (filter);

  if (transformation->shader) {
    gst_object_unref (transformation->shader);
    transformation->shader = NULL;
  }

  if (gst_gl_context_get_gl_api (GST_GL_BASE_FILTER (filter)->context)) {
    /* blocking call, wait until the opengl thread has compiled the shader */
    return gst_gl_context_gen_shader (GST_GL_BASE_FILTER (filter)->context,
        gst_gl_shader_string_vertex_mat4_vertex_transform,
        gst_gl_shader_string_fragment_default, &transformation->shader);
  }
  return TRUE;
}

static const gfloat from_ndc_matrix[] = {
  0.5f, 0.0f, 0.0, 0.5f,
  0.0f, 0.5f, 0.0, 0.5f,
  0.0f, 0.0f, 0.5, 0.5f,
  0.0f, 0.0f, 0.0, 1.0f,
};

static const gfloat to_ndc_matrix[] = {
  2.0f, 0.0f, 0.0, -1.0f,
  0.0f, 2.0f, 0.0, -1.0f,
  0.0f, 0.0f, 2.0, -1.0f,
  0.0f, 0.0f, 0.0, 1.0f,
};

static GstFlowReturn
gst_gl_transformation_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (trans);
  GstGLFilter *filter = GST_GL_FILTER (trans);

  if (transformation->downstream_supports_affine_meta &&
      gst_video_info_is_equal (&filter->in_info, &filter->out_info)) {
    GstVideoAffineTransformationMeta *af_meta;
    graphene_matrix_t upstream_matrix, from_ndc, to_ndc, tmp, tmp2, inv_aspect;

    *outbuf = gst_buffer_make_writable (inbuf);

    af_meta = gst_buffer_get_video_affine_transformation_meta (inbuf);
    if (!af_meta)
      af_meta = gst_buffer_add_video_affine_transformation_meta (*outbuf);

    GST_LOG_OBJECT (trans, "applying transformation to existing affine "
        "transformation meta");

    /* apply the transformation to the existing affine meta */
    graphene_matrix_init_from_float (&from_ndc, from_ndc_matrix);
    graphene_matrix_init_from_float (&to_ndc, to_ndc_matrix);
    graphene_matrix_init_from_float (&upstream_matrix, af_meta->matrix);

    graphene_matrix_init_scale (&inv_aspect, transformation->aspect, 1., 1.);

    graphene_matrix_multiply (&from_ndc, &upstream_matrix, &tmp);
    graphene_matrix_multiply (&tmp, &transformation->mvp_matrix, &tmp2);
    graphene_matrix_multiply (&tmp2, &inv_aspect, &tmp);
    graphene_matrix_multiply (&tmp, &to_ndc, &tmp2);

    graphene_matrix_to_float (&tmp2, af_meta->matrix);
    return GST_FLOW_OK;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->prepare_output_buffer (trans,
      inbuf, outbuf);
}

static gboolean
gst_gl_transformation_filter (GstGLFilter * filter,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (filter);

  if (transformation->downstream_supports_affine_meta &&
      gst_video_info_is_equal (&filter->in_info, &filter->out_info)) {
    return TRUE;
  } else {
    return gst_gl_filter_filter_texture (filter, inbuf, outbuf);
  }
}

static gboolean
gst_gl_transformation_filter_texture (GstGLFilter * filter, guint in_tex,
    guint out_tex)
{
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (filter);

  transformation->in_tex = in_tex;

  /* blocking call, use a FBO */
  gst_gl_context_use_fbo_v2 (GST_GL_BASE_FILTER (filter)->context,
      GST_VIDEO_INFO_WIDTH (&filter->out_info),
      GST_VIDEO_INFO_HEIGHT (&filter->out_info),
      filter->fbo, filter->depthbuffer,
      out_tex, gst_gl_transformation_callback, (gpointer) transformation);

  return TRUE;
}

static const GLushort indices[] = { 0, 1, 2, 3, 0 };

static void
_upload_vertices (GstGLTransformation * transformation)
{
  const GstGLFuncs *gl =
      GST_GL_BASE_FILTER (transformation)->context->gl_vtable;

/* *INDENT-OFF* */
  GLfloat vertices[] = {
     -transformation->aspect,  1.0,  0.0, 1.0, 0.0, 1.0,
      transformation->aspect,  1.0,  0.0, 1.0, 1.0, 1.0,
      transformation->aspect, -1.0,  0.0, 1.0, 1.0, 0.0,
     -transformation->aspect, -1.0,  0.0, 1.0, 0.0, 0.0
  };
  /* *INDENT-ON* */

  gl->BindBuffer (GL_ARRAY_BUFFER, transformation->vertex_buffer);

  gl->BufferData (GL_ARRAY_BUFFER, 4 * 6 * sizeof (GLfloat), vertices,
      GL_STATIC_DRAW);
}

static void
_bind_buffer (GstGLTransformation * transformation)
{
  const GstGLFuncs *gl =
      GST_GL_BASE_FILTER (transformation)->context->gl_vtable;

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, transformation->vbo_indices);
  gl->BindBuffer (GL_ARRAY_BUFFER, transformation->vertex_buffer);

  /* Load the vertex position */
  gl->VertexAttribPointer (transformation->attr_position, 4, GL_FLOAT,
      GL_FALSE, 6 * sizeof (GLfloat), (void *) 0);

  /* Load the texture coordinate */
  gl->VertexAttribPointer (transformation->attr_texture, 2, GL_FLOAT, GL_FALSE,
      6 * sizeof (GLfloat), (void *) (4 * sizeof (GLfloat)));

  gl->EnableVertexAttribArray (transformation->attr_position);
  gl->EnableVertexAttribArray (transformation->attr_texture);
}

static void
_unbind_buffer (GstGLTransformation * transformation)
{
  const GstGLFuncs *gl =
      GST_GL_BASE_FILTER (transformation)->context->gl_vtable;

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
  gl->BindBuffer (GL_ARRAY_BUFFER, 0);

  gl->DisableVertexAttribArray (transformation->attr_position);
  gl->DisableVertexAttribArray (transformation->attr_texture);
}

static void
gst_gl_transformation_callback (gpointer stuff)
{
  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLTransformation *transformation = GST_GL_TRANSFORMATION (filter);
  GstGLFuncs *gl = GST_GL_BASE_FILTER (filter)->context->gl_vtable;

  GLfloat temp_matrix[16];

  gst_gl_context_clear_shader (GST_GL_BASE_FILTER (filter)->context);
  gl->BindTexture (GL_TEXTURE_2D, 0);

  gl->ClearColor (0.f, 0.f, 0.f, 0.f);
  gl->Clear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  gst_gl_shader_use (transformation->shader);

  gl->ActiveTexture (GL_TEXTURE0);
  gl->BindTexture (GL_TEXTURE_2D, transformation->in_tex);
  gst_gl_shader_set_uniform_1i (transformation->shader, "texture", 0);

  graphene_matrix_to_float (&transformation->mvp_matrix, temp_matrix);
  gst_gl_shader_set_uniform_matrix_4fv (transformation->shader,
      "u_transformation", 1, GL_FALSE, temp_matrix);

  if (!transformation->vertex_buffer) {
    transformation->attr_position =
        gst_gl_shader_get_attribute_location (transformation->shader,
        "a_position");

    transformation->attr_texture =
        gst_gl_shader_get_attribute_location (transformation->shader,
        "a_texcoord");

    if (gl->GenVertexArrays) {
      gl->GenVertexArrays (1, &transformation->vao);
      gl->BindVertexArray (transformation->vao);
    }

    gl->GenBuffers (1, &transformation->vertex_buffer);

    gl->GenBuffers (1, &transformation->vbo_indices);
    gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, transformation->vbo_indices);
    gl->BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices,
        GL_STATIC_DRAW);

    transformation->caps_change = TRUE;
  }

  if (gl->GenVertexArrays)
    gl->BindVertexArray (transformation->vao);

  if (transformation->caps_change) {
    _upload_vertices (transformation);
    _bind_buffer (transformation);
  } else if (!gl->GenVertexArrays) {
    _bind_buffer (transformation);
  }

  gl->DrawElements (GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, 0);

  if (gl->GenVertexArrays)
    gl->BindVertexArray (0);
  else
    _unbind_buffer (transformation);

  gst_gl_context_clear_shader (GST_GL_BASE_FILTER (filter)->context);
  transformation->caps_change = FALSE;
}
