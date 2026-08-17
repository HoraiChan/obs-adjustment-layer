#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <obs-module.h>
#include <obs.h>
#include <util/darray.h>
#include <stdbool.h>
#include <stdio.h>
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
  /* Bundled localized PNG used only for the shared-scene warning overlay.
   * This is a graphics texture, not an OBS scene/source item. */
  gs_image_file4_t warning_image;
  bool warning_image_attempted;

  gs_texrender_t *render;     /* main accumulation */
  gs_texrender_t *sub_render; /* scratch capture */

  uint32_t width;
  uint32_t height;

  /* Track sub_render size; recreate only when needed */
  uint32_t sub_w;
  uint32_t sub_h;

  /* Weak scene source to avoid a parent-scene/source reference cycle. */
  obs_weak_source_t *cached_scene;

  /* A source cannot safely be shared by multiple parent scenes because OBS
   * does not pass the scene item to video_render. */
  bool ambiguous_scene;
  bool warned_ambiguous_scene;

  /* video_render can re-enter through nested scenes or filters. */
  bool rendering;

  /* Reuse one completed layer render for all same-frame consumers.  Without
   * this, stacking layers causes the lower scene to be rendered repeatedly
   * (and can grow exponentially with the number of layers). */
  uint64_t rendered_frame_time;
  bool has_rendered_frame;
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

  struct vec2 pos;
  struct vec2 scale;
  struct vec2 current_pos, current_scale;

  vec2_set(&pos, 0.0f, 0.0f);
  vec2_set(&scale, 1.0f, 1.0f);

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

static inline bool item_wants_srgb(enum obs_blending_method method) {
  return method != OBS_BLEND_METHOD_SRGB_OFF;
}

static inline bool apply_item_srgb_state(enum obs_blending_method method) {
  bool prev = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(item_wants_srgb(method));
  return prev;
}

static inline void restore_item_srgb_state(bool prev) {
  gs_enable_framebuffer_srgb(prev);
}

static inline void apply_item_blending(enum obs_blending_type mode) {
  size_t max = sizeof(blend_params) / sizeof(blend_params[0]);
  if ((int)mode < 0 || (size_t)mode >= max)
    mode = OBS_BLEND_NORMAL;

  gs_enable_blending(true);
  gs_blend_function_separate(
      blend_params[mode].src_color, blend_params[mode].dst_color,
      blend_params[mode].src_alpha, blend_params[mode].dst_alpha);
  gs_blend_op(blend_params[mode].op);
}

static inline bool should_force_sub_render(enum obs_blending_type mode,
                                           enum obs_blending_method method) {
  if (mode != OBS_BLEND_NORMAL)
    return true;

  if (method == OBS_BLEND_METHOD_SRGB_OFF)
    return true;

  return false;
}

/* ------------------------------------------------------------------------- */
/* Scene discovery. The result owns one scene reference until the caller turns
 * it into a weak source reference. */
/* ------------------------------------------------------------------------- */

struct find_scene_data {
  obs_source_t *target;
  obs_scene_t *found_scene;
  bool multiple_scenes;
};

static bool check_scene_item(obs_scene_t *scene, obs_sceneitem_t *item,
                             void *param);

static bool find_source_in_scene(void *param, obs_source_t *scene_source) {
  struct find_scene_data *d = param;
  obs_scene_t *scene = obs_scene_from_source(scene_source);
  if (!scene)
    return true;

  obs_scene_enum_items(scene, check_scene_item, d);
  return true;
}

static bool collect_scene_ref(void *param, obs_source_t *scene_source) {
  DARRAY(obs_scene_t *) *scenes = param;
  obs_scene_t *scene = obs_scene_from_source(scene_source);

  if (scene) {
    scene = obs_scene_get_ref(scene);
    if (scene)
      da_push_back(*scenes, &scene);
  }

  return true;
}

static void record_found_scene(struct find_scene_data *d, obs_scene_t *scene) {
  if (!scene)
    return;

  if (!d->found_scene) {
    d->found_scene = obs_scene_get_ref(scene);
  } else if (d->found_scene != scene) {
    d->multiple_scenes = true;
  }
}

static bool check_scene_item(obs_scene_t *scene, obs_sceneitem_t *item,
                             void *param) {
  struct find_scene_data *d = param;
  obs_source_t *src = obs_sceneitem_get_source(item);

  if (src == d->target) {
    record_found_scene(d, scene);
    return true;
  }

  /* Recurse into both scenes and groups. */
  obs_scene_t *child_scene = obs_group_or_scene_from_source(src);
  if (child_scene && child_scene != scene)
    obs_scene_enum_items(child_scene, check_scene_item, d);

  return true;
}

static obs_scene_t *find_parent_scene_for_source(obs_source_t *target,
                                                 bool *multiple_scenes) {
  DARRAY(obs_scene_t *) scenes;
  struct find_scene_data d = {0};

  da_init(scenes);

  /* Collect references while sources_mutex is held, then traverse scenes
   * after obs_enum_scenes releases that global mutex. */
  obs_enum_scenes(collect_scene_ref, &scenes);

  d.target = target;
  d.found_scene = NULL;
  d.multiple_scenes = false;

  for (size_t i = 0; i < scenes.num; i++) {
    find_source_in_scene(&d, obs_scene_get_source(scenes.array[i]));
    obs_scene_release(scenes.array[i]);
  }

  da_free(scenes);

  if (multiple_scenes)
    *multiple_scenes = d.multiple_scenes;

  return d.found_scene;
}

/* ------------------------------------------------------------------------- */
/* Render helpers */
/*   - transitions: NOT supported (instant switch) */
/*   - graphics resources are destroyed on the OBS graphics context */
/* ------------------------------------------------------------------------- */

static inline bool is_zero_crop(const struct obs_sceneitem_crop *c) {
  return (c->left == 0 && c->top == 0 && c->right == 0 && c->bottom == 0);
}

/* Snapshot scene-item state while the scene is locked. Rendering uses this
 * copy after the lock has been released, so filters and child sources are not
 * called while obs_scene_enum_items holds scene mutexes. */
struct adjustment_layer_item {
  obs_source_t *source;
  struct matrix4 transform;
  struct obs_sceneitem_crop crop;
  bool visible;
  enum obs_blending_method blend_method;
  enum obs_blending_type blend_type;
};

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
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                               GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_blend_op(GS_BLEND_OP_ADD);

    obs_source_video_render(src);

    gs_blend_state_pop();
    gs_texrender_end(ctx->sub_render);
  }

  return gs_texrender_get_texture(ctx->sub_render);
}

/* Composite a texture to main render with item transform + crop + item blend */
static void composite_texture_with_item(struct adjustment_layer_source *ctx,
                                        const struct adjustment_layer_item *item,
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

  uint32_t left = (left_i > 0) ? (uint32_t)left_i : 0u;
  uint32_t right = (right_i > 0) ? (uint32_t)right_i : 0u;
  uint32_t top = (top_i > 0) ? (uint32_t)top_i : 0u;
  uint32_t bottom = (bottom_i > 0) ? (uint32_t)bottom_i : 0u;

  if (left > tw)
    left = tw;
  if (right > tw - left)
    right = tw - left;
  if (top > th)
    top = th;
  if (bottom > th - top)
    bottom = th - top;

  const uint32_t cw = tw - left - right;
  const uint32_t ch = th - top - bottom;

  if (cw == 0 || ch == 0)
    return;

  gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  if (!draw_effect)
    return;

  gs_eparam_t *image = gs_effect_get_param_by_name(draw_effect, "image");
  if (!image)
    return;

  const bool srgb = item_wants_srgb(item->blend_method);

  if (srgb)
    gs_effect_set_texture_srgb(image, tex);
  else
    gs_effect_set_texture(image, tex);

  struct matrix4 transform;
  transform = item->transform;

  gs_matrix_push();
  gs_matrix_mul(&transform);

  gs_blend_state_push();

  const bool prev = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(srgb);

  apply_item_blending(item->blend_type);

  while (gs_effect_loop(draw_effect, "Draw")) {
    if (is_zero_crop(crop)) {
      gs_draw_sprite(tex, 0, tw, th);
    } else {
      gs_draw_sprite_subregion(tex, 0, left, top, cw, ch);
    }
  }

  restore_item_srgb_state(prev);

  gs_blend_state_pop();
  gs_matrix_pop();
}

/* Direct render path (ONLY safe for NORMAL + zero-crop) */
static void render_item_direct_normal_nocrop(
    const struct adjustment_layer_item *item) {
  if (!item || !item->source)
    return;

  uint32_t sw = obs_source_get_width(item->source);
  uint32_t sh = obs_source_get_height(item->source);
  if (sw == 0 || sh == 0)
    return;

  gs_matrix_push();
  gs_matrix_mul(&item->transform);

  gs_blend_state_push();

  bool prev_srgb = apply_item_srgb_state(item->blend_method);
  bool prev_linear = gs_set_linear_srgb(true);

  gs_enable_blending(true);
  gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
                             GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  gs_blend_op(GS_BLEND_OP_ADD);

  obs_source_video_render(item->source);

  gs_set_linear_srgb(prev_linear);
  restore_item_srgb_state(prev_srgb);

  gs_blend_state_pop();
  gs_matrix_pop();
}

static void render_item(struct adjustment_layer_source *ctx,
                        const struct adjustment_layer_item *item) {
  if (!ctx || !item || !item->source)
    return;

  if (!item->visible)
    return;

  uint32_t sw = obs_source_get_width(item->source);
  uint32_t sh = obs_source_get_height(item->source);
  if (sw == 0 || sh == 0)
    return;

  bool force_sub = should_force_sub_render(item->blend_type,
                                           item->blend_method);

  /* Fast path: NORMAL + no crop -> direct render */
  if (!force_sub && is_zero_crop(&item->crop)) {
    render_item_direct_normal_nocrop(item);
    return;
  }

  /* Otherwise: capture then composite (stable for non-NORMAL and/or crop) */
  gs_texture_t *sub_tex = capture_source_to_sub(ctx, item->source, sw, sh);
  if (!sub_tex)
    return;

  /* NOTE: sub_tex size == sw/sh */
  composite_texture_with_item(ctx, item, sub_tex, &item->crop);
}

/* ------------------------------------------------------------------------- */
/* Enum items below adjustment layer */
/* ------------------------------------------------------------------------- */

struct render_params {
  obs_source_t *target;
  bool found_self;
  DARRAY(struct adjustment_layer_item) items;
};

static bool collect_render_item(obs_scene_t *scene, obs_sceneitem_t *item,
                                void *param) {
  UNUSED_PARAMETER(scene);
  struct render_params *p = param;

  obs_source_t *src = obs_sceneitem_get_source(item);
  if (!src)
    return true;

  if (src == p->target) {
    p->found_self = true;
    return true;
  }

  /* Items BEFORE self treated as "below" */
  if (!p->found_self) {
    struct adjustment_layer_item snapshot = {0};
    snapshot.source = obs_source_get_ref(src);
    if (!snapshot.source)
      return true;

    snapshot.visible = obs_sceneitem_visible(item);
    snapshot.blend_method = obs_sceneitem_get_blending_method(item);
    snapshot.blend_type = obs_sceneitem_get_blending_mode(item);
    obs_sceneitem_get_crop(item, &snapshot.crop);
    obs_sceneitem_get_draw_transform(item, &snapshot.transform);
    da_push_back(p->items, &snapshot);
  }

  return true;
}

static void release_render_params(struct render_params *p) {
  for (size_t i = 0; i < p->items.num; i++)
    obs_source_release(p->items.array[i].source);

  da_free(p->items);
}

/* ------------------------------------------------------------------------- */
/* Shared-scene warning overlay                                               */
/* ------------------------------------------------------------------------- */

static const char *warning_texture_locale(void) {
  const char *locale = obs_get_locale();

  /* Keep the path restricted to the assets shipped by this plugin.  OBS
   * normally returns one of these exact locale IDs, but an unsupported or
   * empty locale falls back to English instead of becoming a path fragment. */
  if (locale && (strcmp(locale, "ja-JP") == 0 ||
                 strcmp(locale, "zh-CN") == 0 ||
                 strcmp(locale, "zh-TW") == 0 ||
                 strcmp(locale, "ko-KR") == 0 ||
                 strcmp(locale, "en-US") == 0))
    return locale;

  return "en-US";
}

static bool load_warning_texture_file(struct adjustment_layer_source *ctx,
                                      const char *locale) {
  if (!ctx || !locale)
    return false;

  char relative_path[128];
  int written = snprintf(relative_path, sizeof(relative_path),
                         "graphics/warnings/shared-scene/%s.png", locale);
  if (written < 0 || (size_t)written >= sizeof(relative_path))
    return false;

  char *path = obs_module_file(relative_path);
  if (!path)
    return false;

  /* Decode the PNG and premultiply alpha before creating the GPU texture.
   * The call is made from video_render, where the OBS graphics context is
   * current; gs_image_file4_free is likewise called on that context. */
  gs_image_file4_init(&ctx->warning_image, path,
                      GS_IMAGE_ALPHA_PREMULTIPLY_SRGB);
  bfree(path);

  if (!ctx->warning_image.image3.image2.image.loaded)
    return false;

  gs_image_file4_init_texture(&ctx->warning_image);
  if (!ctx->warning_image.image3.image2.image.texture) {
    gs_image_file4_free(&ctx->warning_image);
    return false;
  }

  return true;
}

static bool ensure_warning_texture(struct adjustment_layer_source *ctx) {
  if (!ctx)
    return false;

  if (ctx->warning_image_attempted)
    return ctx->warning_image.image3.image2.image.texture != NULL;

  ctx->warning_image_attempted = true;

  const char *locale = warning_texture_locale();
  if (load_warning_texture_file(ctx, locale))
    return true;

  /* If a localized asset is missing from a package, keep the warning useful
   * by trying the guaranteed English asset once. */
  if (strcmp(locale, "en-US") != 0) {
    gs_image_file4_free(&ctx->warning_image);
    if (load_warning_texture_file(ctx, "en-US")) {
      blog(LOG_WARNING,
           "[adjustment-layer] warning texture for locale '%s' was not "
           "available; using en-US",
           locale);
      return true;
    }
  }

  blog(LOG_WARNING,
       "[adjustment-layer] failed to load the shared-scene warning texture "
       "for locale '%s'",
       locale);
  return false;
}

static void draw_shared_scene_warning(struct adjustment_layer_source *ctx) {
  if (!ctx || ctx->width == 0 || ctx->height == 0 ||
      !ensure_warning_texture(ctx))
    return;

  const struct gs_image_file *image = &ctx->warning_image.image3.image2.image;
  gs_texture_t *texture = image->texture;
  const uint32_t image_width = image->cx;
  const uint32_t image_height = image->cy;
  if (!texture || image_width == 0 || image_height == 0)
    return;

  /* Keep the supplied 1080x1080 artwork at native size whenever it fits.  On
   * a smaller canvas, scale it down uniformly so the whole square remains
   * visible.  The resulting rectangle is always centered in the video frame. */
  float scale = 1.0f;
  if (image_width > ctx->width || image_height > ctx->height) {
    const float width_scale = (float)ctx->width / (float)image_width;
    const float height_scale = (float)ctx->height / (float)image_height;
    scale = width_scale < height_scale ? width_scale : height_scale;
  }
  if (scale <= 0.0f)
    return;

  const float draw_width = (float)image_width * scale;
  const float draw_height = (float)image_height * scale;
  const float x = ((float)ctx->width - draw_width) * 0.5f;
  const float y = ((float)ctx->height - draw_height) * 0.5f;

  gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  if (!draw_effect)
    return;

  gs_eparam_t *image_param =
      gs_effect_get_param_by_name(draw_effect, "image");
  if (!image_param)
    return;

  gs_effect_set_texture_srgb(image_param, texture);

  const bool previous_srgb = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(true);

  gs_matrix_push();
  gs_matrix_identity();
  gs_matrix_translate3f(x, y, 0.0f);
  gs_matrix_scale3f(scale, scale, 1.0f);

  gs_blend_state_push();
  gs_enable_blending(true);
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  gs_blend_op(GS_BLEND_OP_ADD);

  while (gs_effect_loop(draw_effect, "Draw"))
    gs_draw_sprite(texture, 0, image_width, image_height);

  gs_blend_state_pop();
  gs_matrix_pop();
  gs_enable_framebuffer_srgb(previous_srgb);
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

  if (ctx->render || ctx->sub_render || ctx->warning_image_attempted) {
    /* Source destruction already runs on OBS's destruction thread.  Acquire
     * the graphics context here so the resources cannot outlive this module
     * or a stopped graphics thread. */
    obs_enter_graphics();
    if (ctx->render)
      gs_texrender_destroy(ctx->render);
    if (ctx->sub_render)
      gs_texrender_destroy(ctx->sub_render);
    gs_image_file4_free(&ctx->warning_image);
    obs_leave_graphics();
  }

  if (ctx->cached_scene)
    obs_weak_source_release(ctx->cached_scene);

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

static bool draw_layer_texture(struct adjustment_layer_source *ctx) {
  if (!ctx || !ctx->render)
    return false;

  gs_texture_t *tex = gs_texrender_get_texture(ctx->render);
  if (!tex)
    return false;

  gs_effect_t *draw_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  if (!draw_effect)
    return false;

  gs_eparam_t *image = gs_effect_get_param_by_name(draw_effect, "image");
  if (!image)
    return false;

  gs_effect_set_texture(image, tex);

  gs_blend_state_push();
  gs_enable_blending(true);
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
  gs_blend_op(GS_BLEND_OP_ADD);

  gs_matrix_push();
  gs_matrix_identity();
  while (gs_effect_loop(draw_effect, "Draw"))
    gs_draw_sprite(tex, 0, ctx->width, ctx->height);
  gs_matrix_pop();

  gs_blend_state_pop();
  return true;
}

struct find_item_params {
  obs_source_t *target;
  obs_sceneitem_t *item;
};

static bool find_target_item(obs_scene_t *scene, obs_sceneitem_t *item,
                             void *param) {
  UNUSED_PARAMETER(scene);
  struct find_item_params *p = param;

  if (obs_sceneitem_get_source(item) != p->target)
    return true;

  p->item = item;
  obs_sceneitem_addref(item);
  return false;
}

static void adjustment_layer_video_render(void *data, gs_effect_t *effect) {
  UNUSED_PARAMETER(effect);

  struct adjustment_layer_source *ctx = data;
  if (!ctx)
    return;

  if (ctx->ambiguous_scene) {
    /* Shared placement cannot be rendered correctly because OBS does not
     * identify the parent scene here.  Show the actionable warning instead of
     * silently returning a transparent frame. */
    draw_shared_scene_warning(ctx);
    return;
  }

  if (!ctx->cached_scene)
    return;

  /* The layer renders child scenes/sources manually.  Refuse re-entry rather
   * than beginning the same texrender while it is already active. */
  if (ctx->rendering)
    return;

  obs_source_t *cached_scene_source =
      obs_weak_source_get_source(ctx->cached_scene);
  if (!cached_scene_source)
    return;

  obs_scene_t *cached_scene = obs_scene_from_source(cached_scene_source);
  if (!cached_scene) {
    obs_source_release(cached_scene_source);
    return;
  }

  const uint64_t frame_time = obs_get_video_frame_time();
  if (ctx->has_rendered_frame && ctx->rendered_frame_time == frame_time) {
    obs_source_release(cached_scene_source);
    draw_layer_texture(ctx);
    return;
  }

  ctx->rendering = true;

  struct render_params p = {0};
  p.target = ctx->source;
  da_init(p.items);

  /* Only snapshot item/source state while the scene mutexes are held.  The
   * actual source and Lua-filter rendering happens after this call returns. */
  obs_scene_enum_items(cached_scene, collect_render_item, &p);
  obs_source_release(cached_scene_source);

  if (!p.found_self) {
    release_render_params(&p);
    ctx->rendering = false;
    return;
  }

  ensure_texrenders(ctx);
  if (!ctx->render)
    goto cleanup;

  gs_texrender_reset(ctx->render);
  if (!gs_texrender_begin(ctx->render, ctx->width, ctx->height))
    goto cleanup;

  struct vec4 clear_color;
  vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);
  gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

  gs_ortho(0.0f, (float)ctx->width, 0.0f, (float)ctx->height, -100.0f,
           100.0f);

  for (size_t i = 0; i < p.items.num; i++)
    render_item(ctx, &p.items.array[i]);

  gs_texrender_end(ctx->render);

  if (draw_layer_texture(ctx)) {
    ctx->rendered_frame_time = frame_time;
    ctx->has_rendered_frame = true;
  }

cleanup:
  release_render_params(&p);
  ctx->rendering = false;
}

static void adjustment_layer_video_tick(void *data, float seconds) {
  UNUSED_PARAMETER(seconds);

  struct adjustment_layer_source *ctx = data;
  if (!ctx)
    return;

  ctx->has_rendered_frame = false;

  struct obs_video_info ovi;
  if (obs_get_video_info(&ovi)) {
    ctx->width = ovi.base_width;
    ctx->height = ovi.base_height;
  }

  bool multiple_scenes = false;
  obs_scene_t *new_scene =
      find_parent_scene_for_source(ctx->source, &multiple_scenes);
  obs_weak_source_t *new_cached_scene = NULL;

  if (multiple_scenes) {
    if (!ctx->warned_ambiguous_scene) {
      blog(LOG_WARNING,
           "[adjustment-layer] source '%s' is used in multiple scenes; "
           "adjustment processing is disabled for safety. Create a separate "
           "Adjustment Layer source for each scene",
           obs_source_get_name(ctx->source));
      ctx->warned_ambiguous_scene = true;
    }

    if (new_scene)
      obs_scene_release(new_scene);
    new_scene = NULL;
  } else {
    ctx->warned_ambiguous_scene = false;

    if (new_scene) {
      /* Scene-item setters can emit signals and update transforms.  Do this
       * during video_tick, never from video_render while scene rendering holds
       * the parent scene's video mutex. */
      struct find_item_params item_params = {
          .target = ctx->source,
          .item = NULL,
      };
      obs_scene_enum_items(new_scene, find_target_item, &item_params);

      obs_source_t *scene_source = obs_scene_get_source(new_scene);
      if (scene_source)
        new_cached_scene = obs_source_get_weak_source(scene_source);
      obs_scene_release(new_scene);
      new_scene = NULL;

      if (item_params.item) {
        enforce_item_state(item_params.item);
        obs_sceneitem_release(item_params.item);
      }
    }
  }

  obs_weak_source_t *old_scene = ctx->cached_scene;
  ctx->cached_scene = new_cached_scene;
  ctx->ambiguous_scene = multiple_scenes;
  if (old_scene)
    obs_weak_source_release(old_scene);
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
