#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <obs-module.h>
#include <obs.h>
#include <stdbool.h>
#include <string.h>

/**
 * OBS Adjustment Layer Plugin
 * Version: PLUGIN_VERSION (Defined in CMakeLists.txt)
 */

/* ------------------------------------------------------------------------- */
/* Data                                                                       */
/* ------------------------------------------------------------------------- */

struct adjustment_layer_source {
  obs_source_t *source;

  gs_texrender_t *render;     /* main accumulation */
  gs_texrender_t *sub_render; /* scratch capture */

  uint32_t width;
  uint32_t height;

  /* Track sub_render size; recreate only when needed */
  uint32_t sub_w;
  uint32_t sub_h;
};

/* ------------------------------------------------------------------------- */
/* Name                                                                       */
/* ------------------------------------------------------------------------- */

static const char *adjustment_layer_get_name(void *unused) {
  UNUSED_PARAMETER(unused);
  return obs_module_text("AdjustmentLayer");
}

/* ------------------------------------------------------------------------- */
/* Enforce our own scene item state */
/* ------------------------------------------------------------------------- */

static void enforce_item_state(obs_sceneitem_t *item) {
  if (!item)
    return;

  struct vec2 pos = {0.0f, 0.0f};
  struct vec2 scale = {1.0f, 1.0f};
  struct vec2 current_pos, current_scale;

  obs_sceneitem_get_pos(item, &current_pos);
  obs_sceneitem_get_scale(item, &current_scale);
  uint32_t align = obs_sceneitem_get_alignment(item);
  enum obs_bounds_type bounds = obs_sceneitem_get_bounds_type(item);

  if (current_pos.x != 0.0f || current_pos.y != 0.0f ||
      current_scale.x != 1.0f || current_scale.y != 1.0f ||
      align != (OBS_ALIGN_TOP | OBS_ALIGN_LEFT) || bounds != OBS_BOUNDS_NONE) {
    obs_sceneitem_set_pos(item, &pos);
    obs_sceneitem_set_scale(item, &scale);
    obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
    obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
  }

  if (!obs_sceneitem_locked(item))
    obs_sceneitem_set_locked(item, true);
}

/* ------------------------------------------------------------------------- */
/* Blend mode support (scene item) */
/* ------------------------------------------------------------------------- */

static const struct {
  enum gs_blend_type src_color;
  enum gs_blend_type dst_color;
  enum gs_blend_type src_alpha;
  enum gs_blend_type dst_alpha;
  enum gs_blend_op_type op;
} blend_params[] = {
    /* OBS_BLEND_NORMAL */
    {GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA,
     GS_BLEND_OP_ADD},
    /* OBS_BLEND_ADDITIVE */
    {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_ADD},
    /* OBS_BLEND_SUBTRACT */
    {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE,
     GS_BLEND_OP_REVERSE_SUBTRACT},
    /* OBS_BLEND_SCREEN */
    {GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA,
     GS_BLEND_OP_ADD},
    /* OBS_BLEND_MULTIPLY */
    {GS_BLEND_DSTCOLOR, GS_BLEND_INVSRCALPHA, GS_BLEND_DSTALPHA,
     GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},
    /* OBS_BLEND_LIGHTEN */
    {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MAX},
    /* OBS_BLEND_DARKEN */
    {GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MIN},
};

/* ------------------------------------------------------------------------- */
/* Blend method support (sRGB / sRGB off) */
/* ------------------------------------------------------------------------- */

static inline bool item_wants_srgb(obs_sceneitem_t *item) {
  enum obs_blending_method method = obs_sceneitem_get_blending_method(item);
  return method != OBS_BLEND_METHOD_SRGB_OFF;
}

static inline bool apply_item_srgb_state(obs_sceneitem_t *item) {
  bool prev = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(item_wants_srgb(item));
  return prev;
}

static inline void restore_item_srgb_state(bool prev) {
  gs_enable_framebuffer_srgb(prev);
}

static inline void apply_item_blending(obs_sceneitem_t *item) {
  enum obs_blending_type mode = obs_sceneitem_get_blending_mode(item);

  size_t max = sizeof(blend_params) / sizeof(blend_params[0]);
  if ((int)mode < 0 || (size_t)mode >= max)
    mode = OBS_BLEND_NORMAL;

  gs_enable_blending(true);
  gs_blend_function_separate(
      blend_params[mode].src_color, blend_params[mode].dst_color,
      blend_params[mode].src_alpha, blend_params[mode].dst_alpha);
  gs_blend_op(blend_params[mode].op);
}

static inline bool should_force_sub_render(obs_sceneitem_t *item) {
  if (obs_sceneitem_get_blending_mode(item) != OBS_BLEND_NORMAL)
    return true;

  if (obs_sceneitem_get_blending_method(item) == OBS_BLEND_METHOD_SRGB_OFF)
    return true;

  return false;
}

/* ------------------------------------------------------------------------- */
/* Scene discovery (NO caching; NO obs_source_active filtering) */
/* ------------------------------------------------------------------------- */

struct find_scene_data {
  obs_source_t *target;
  obs_scene_t *found_scene;
};

static bool check_scene_item(obs_scene_t *scene, obs_sceneitem_t *item,
                             void *param);

static bool find_source_in_scene(void *param, obs_source_t *scene_source) {
  struct find_scene_data *d = param;
  obs_scene_t *scene = obs_scene_from_source(scene_source);
  if (!scene)
    return true;

  obs_scene_enum_items(scene, check_scene_item, d);
  return (d->found_scene == NULL);
}

static bool check_scene_item(obs_scene_t *scene, obs_sceneitem_t *item,
                             void *param) {
  struct find_scene_data *d = param;
  obs_source_t *src = obs_sceneitem_get_source(item);

  if (src == d->target) {
    d->found_scene = scene;
    return false;
  }

  /* Recurse into groups */
  obs_scene_t *group_scene = obs_scene_from_source(src);
  if (group_scene) {
    obs_scene_enum_items(group_scene, check_scene_item, d);
    if (d->found_scene)
      return false;
  }

  return true;
}

static obs_scene_t *find_parent_scene_for_source(obs_source_t *target) {
  struct find_scene_data d = {0};
  d.target = target;
  d.found_scene = NULL;
  obs_enum_scenes(find_source_in_scene, &d);
  return d.found_scene;
}

/* ------------------------------------------------------------------------- */
/* Render helpers */
/*   - transitions: NOT supported (instant switch) */
/*   - NO obs_enter_graphics anywhere (Windows deadlock avoidance) */
/* ------------------------------------------------------------------------- */

static inline bool is_zero_crop(const struct obs_sceneitem_crop *c) {
  return (c->left == 0 && c->top == 0 && c->right == 0 && c->bottom == 0);
}

static void ensure_texrenders(struct adjustment_layer_source *ctx) {
  /* Called only from video_render (graphics context) */
  if (!ctx->render)
    ctx->render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

  if (!ctx->sub_render) {
    ctx->sub_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    ctx->sub_w = 0;
    ctx->sub_h = 0;
  }
}

static void recreate_sub_render_if_needed(struct adjustment_layer_source *ctx,
                                          uint32_t sw, uint32_t sh) {
  /* Called only from video_render (graphics context) */
  if (!ctx->sub_render || ctx->sub_w != sw || ctx->sub_h != sh) {
    if (ctx->sub_render) {
      gs_texrender_destroy(ctx->sub_render);
      ctx->sub_render = NULL;
    }
    ctx->sub_render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    ctx->sub_w = sw;
    ctx->sub_h = sh;
  }
}

/* Capture src into sub_render with a clean, predictable blend state */
static gs_texture_t *capture_source_to_sub(struct adjustment_layer_source *ctx,
                                           obs_source_t *src, uint32_t sw,
                                           uint32_t sh) {
  recreate_sub_render_if_needed(ctx, sw, sh);
  if (!ctx->sub_render)
    return NULL;

  gs_texrender_reset(ctx->sub_render);
  if (gs_texrender_begin(ctx->sub_render, sw, sh)) {
    struct vec4 clear_color;
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);

    /* IMPORTANT:
     * Capture must be done with NORMAL blend fixed to avoid leakage
     * from previous item blending. */
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_blend_op(GS_BLEND_OP_ADD);

    obs_source_video_render(src);

    gs_blend_state_pop();
    gs_texrender_end(ctx->sub_render);
  }

  return gs_texrender_get_texture(ctx->sub_render);
}

/* Composite a texture to main render with item transform + crop + item blend */
static void composite_texture_with_item(struct adjustment_layer_source *ctx,
                                        obs_sceneitem_t *item,
                                        gs_texture_t *tex,
                                        const struct obs_sceneitem_crop *crop) {
  if (!ctx || !item || !tex || !crop)
    return;

  uint32_t tw = gs_texture_get_width(tex);
  uint32_t th = gs_texture_get_height(tex);
  if (tw == 0 || th == 0)
    return;

  // crop values can be signed; clamp to 0 to avoid signed/unsigned warnings and
  // weird math
  const int left_i = crop->left;
  const int right_i = crop->right;
  const int top_i = crop->top;
  const int bottom_i = crop->bottom;

  const uint32_t left = (left_i > 0) ? (uint32_t)left_i : 0u;
  const uint32_t right = (right_i > 0) ? (uint32_t)right_i : 0u;
  const uint32_t top = (top_i > 0) ? (uint32_t)top_i : 0u;
  const uint32_t bottom = (bottom_i > 0) ? (uint32_t)bottom_i : 0u;

  uint32_t cw = (tw > (left + right)) ? (tw - left - right) : 0u;
  uint32_t ch = (th > (top + bottom)) ? (th - top - bottom) : 0u;

  if (cw == 0 || ch == 0)
    return;

  gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  gs_eparam_t *image = gs_effect_get_param_by_name(draw_effect, "image");

  const bool srgb = item_wants_srgb(item);

  if (srgb)
    gs_effect_set_texture_srgb(image, tex);
  else
    gs_effect_set_texture(image, tex);

  struct matrix4 transform;
  obs_sceneitem_get_draw_transform(item, &transform);

  gs_matrix_push();
  gs_matrix_mul(&transform);

  gs_blend_state_push();

  const bool prev = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(srgb);

  apply_item_blending(item);

  while (gs_effect_loop(draw_effect, "Draw")) {
    if (is_zero_crop(crop)) {
      gs_draw_sprite(tex, 0, tw, th);
    } else {
      gs_draw_sprite_subregion(tex, 0, crop->left, crop->top, cw, ch);
    }
  }

  restore_item_srgb_state(prev);

  gs_blend_state_pop();
  gs_matrix_pop();
}

/* Direct render path (ONLY safe for NORMAL + zero-crop) */
static void render_item_direct_normal_nocrop(obs_sceneitem_t *item,
                                             obs_source_t *src) {
  if (!item || !src)
    return;

  uint32_t sw = obs_source_get_width(src);
  uint32_t sh = obs_source_get_height(src);
  if (sw == 0 || sh == 0)
    return;

  struct matrix4 transform;
  obs_sceneitem_get_draw_transform(item, &transform);

  gs_matrix_push();
  gs_matrix_mul(&transform);

  gs_blend_state_push();

  bool prev_srgb = apply_item_srgb_state(item);

  gs_enable_blending(true);
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  gs_blend_op(GS_BLEND_OP_ADD);

  obs_source_video_render(src);

  restore_item_srgb_state(prev_srgb);

  gs_blend_state_pop();
  gs_matrix_pop();
}

static void render_item(struct adjustment_layer_source *ctx,
                        obs_sceneitem_t *item, obs_source_t *src) {
  if (!ctx || !item || !src)
    return;

  if (!obs_sceneitem_visible(item))
    return;

  uint32_t sw = obs_source_get_width(src);
  uint32_t sh = obs_source_get_height(src);
  if (sw == 0 || sh == 0)
    return;

  struct obs_sceneitem_crop crop;
  obs_sceneitem_get_crop(item, &crop);

  bool force_sub = should_force_sub_render(item);

  /* Fast path: NORMAL + no crop -> direct render */
  if (!force_sub && is_zero_crop(&crop)) {
    render_item_direct_normal_nocrop(item, src);
    return;
  }

  /* Otherwise: capture then composite (stable for non-NORMAL and/or crop) */
  gs_texture_t *sub_tex = capture_source_to_sub(ctx, src, sw, sh);
  if (!sub_tex)
    return;

  /* NOTE: sub_tex size == sw/sh */
  composite_texture_with_item(ctx, item, sub_tex, &crop);
}

/* ------------------------------------------------------------------------- */
/* Enum items below adjustment layer */
/* ------------------------------------------------------------------------- */

struct render_params {
  struct adjustment_layer_source *ctx;
  bool found_self;
};

static bool render_enum_callback(obs_scene_t *scene, obs_sceneitem_t *item,
                                 void *param) {
  UNUSED_PARAMETER(scene);
  struct render_params *p = param;

  obs_source_t *src = obs_sceneitem_get_source(item);
  if (!src)
    return true;

  if (src == p->ctx->source) {
    enforce_item_state(item);
    p->found_self = true;
    return true;
  }

  /* Items BEFORE self treated as "below" */
  if (!p->found_self) {
    render_item(p->ctx, item, src);
  }

  return true;
}

/* ------------------------------------------------------------------------- */
/* OBS callbacks */
/* ------------------------------------------------------------------------- */

static void *adjustment_layer_create(obs_data_t *settings,
                                     obs_source_t *source) {
  UNUSED_PARAMETER(settings);

  struct adjustment_layer_source *ctx = bzalloc(sizeof(*ctx));
  ctx->source = source;

  struct obs_video_info ovi;
  if (obs_get_video_info(&ovi)) {
    ctx->width = ovi.base_width;
    ctx->height = ovi.base_height;
  }

  ctx->render = NULL;
  ctx->sub_render = NULL;
  ctx->sub_w = 0;
  ctx->sub_h = 0;

  return ctx;
}

static void adjustment_layer_destroy(void *data) {
  struct adjustment_layer_source *ctx = data;
  if (!ctx)
    return;

  if (ctx->render) {
    gs_texrender_destroy(ctx->render);
    ctx->render = NULL;
  }
  if (ctx->sub_render) {
    gs_texrender_destroy(ctx->sub_render);
    ctx->sub_render = NULL;
  }

  bfree(ctx);
}

static void adjustment_layer_update(void *data, obs_data_t *settings) {
  UNUSED_PARAMETER(data);
  UNUSED_PARAMETER(settings);
}

static uint32_t adjustment_layer_get_width(void *data) {
  struct adjustment_layer_source *ctx = data;
  return ctx ? ctx->width : 0;
}

static uint32_t adjustment_layer_get_height(void *data) {
  struct adjustment_layer_source *ctx = data;
  return ctx ? ctx->height : 0;
}

static void adjustment_layer_video_render(void *data, gs_effect_t *effect) {
  UNUSED_PARAMETER(effect);

  struct adjustment_layer_source *ctx = data;
  if (!ctx)
    return;

  ensure_texrenders(ctx);
  if (!ctx->render)
    return;

  obs_scene_t *scene = find_parent_scene_for_source(ctx->source);
  if (!scene)
    return;

  gs_texrender_reset(ctx->render);
  if (gs_texrender_begin(ctx->render, ctx->width, ctx->height)) {
    struct vec4 clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

    gs_ortho(0.0f, (float)ctx->width, 0.0f, (float)ctx->height, -100.0f,
             100.0f);

    struct render_params p = {0};
    p.ctx = ctx;
    p.found_self = false;

    obs_scene_enum_items(scene, render_enum_callback, &p);

    gs_texrender_end(ctx->render);
  }

  gs_texture_t *tex = gs_texrender_get_texture(ctx->render);
  if (!tex)
    return;

  gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  if (!draw_effect)
    return;

  gs_eparam_t *image = gs_effect_get_param_by_name(draw_effect, "image");
  if (!image)
    return;

  gs_effect_set_texture(image, tex);

  gs_blend_state_push();
  gs_enable_blending(true);
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  gs_blend_op(GS_BLEND_OP_ADD);

  gs_matrix_push();
  gs_matrix_identity();
  while (gs_effect_loop(draw_effect, "Draw")) {
    gs_draw_sprite(tex, 0, ctx->width, ctx->height);
  }
  gs_matrix_pop();

  gs_blend_state_pop();
}

static void adjustment_layer_video_tick(void *data, float seconds) {
  UNUSED_PARAMETER(seconds);

  struct adjustment_layer_source *ctx = data;
  if (!ctx)
    return;

  struct obs_video_info ovi;
  if (obs_get_video_info(&ovi)) {
    ctx->width = ovi.base_width;
    ctx->height = ovi.base_height;
  }
}

struct obs_source_info adjustment_layer_info = {
    .id = "adjustment-layer",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .get_name = adjustment_layer_get_name,
    .create = adjustment_layer_create,
    .destroy = adjustment_layer_destroy,
    .update = adjustment_layer_update,
    .get_width = adjustment_layer_get_width,
    .get_height = adjustment_layer_get_height,
    .video_render = adjustment_layer_video_render,
    .video_tick = adjustment_layer_video_tick,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("adjustment-layer", "en-US")

bool obs_module_load(void) {
  obs_register_source(&adjustment_layer_info);
  return true;
}