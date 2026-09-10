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

enum adjustment_layer_warning {
  ADJUSTMENT_LAYER_WARNING_SHARED_SCENE,
  ADJUSTMENT_LAYER_WARNING_GROUP_PLACEMENT,
  ADJUSTMENT_LAYER_WARNING_COUNT,
};

struct adjustment_layer_warning_texture {
  gs_image_file4_t image;
  bool attempted;
};

struct adjustment_layer_source {
  obs_source_t *source;
  /* Bundled localized PNGs used for warning overlays.  These are graphics
   * textures, not OBS scene/source items. */
  struct adjustment_layer_warning_texture
      warning_textures[ADJUSTMENT_LAYER_WARNING_COUNT];

  gs_texrender_t *render;     /* main accumulation */
  gs_texrender_t *sub_render; /* scratch capture */

  uint32_t width;
  uint32_t height;

  /* Track sub_render size; recreate only when needed */
  uint32_t sub_w;
  uint32_t sub_h;

  /* Weak scene/group source to avoid a parent-container/source reference
   * cycle.  The container is the immediate scene or group that owns this
   * adjustment-layer item. */
  obs_weak_source_t *cached_container;

  /* A source cannot safely be shared by multiple parent containers because
   * OBS does not pass the scene item to video_render. */
  bool ambiguous_container;
  bool warned_ambiguous_container;

  /* Adjustment processing is intentionally unsupported in groups.  A group
   * is transparent and cannot replace items that OBS has already composited,
   * so alpha-changing filters would expose the original unfiltered image. */
  bool inside_group;
  bool warned_group_container;

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
/* Container discovery. The result owns one scene/group reference until the
 * caller turns it into a weak source reference. */
/* ------------------------------------------------------------------------- */

struct find_container_data {
  obs_source_t *target;
  obs_scene_t *found_container;
  obs_sceneitem_t *found_item;
  DARRAY(obs_scene_t *) visited_containers;
  bool ambiguous;
};

static bool check_container_item(obs_scene_t *container,
                                 obs_sceneitem_t *item, void *param);

/* A public scene can also be reached recursively through another scene.
 * Retain every visited scene/group so pointer identity remains valid for the
 * entire traversal and each container is enumerated exactly once. */
static bool mark_container_visited(struct find_container_data *d,
                                   obs_scene_t *container) {
  for (size_t i = 0; i < d->visited_containers.num; i++) {
    if (d->visited_containers.array[i] == container)
      return false;
  }

  obs_scene_t *container_ref = obs_scene_get_ref(container);
  if (!container_ref)
    return false;

  da_push_back(d->visited_containers, &container_ref);
  return true;
}

static void scan_container(struct find_container_data *d,
                           obs_scene_t *container) {
  if (!container || !mark_container_visited(d, container))
    return;

  obs_scene_enum_items(container, check_container_item, d);
}

static bool find_source_in_container(void *param,
                                     obs_source_t *container_source) {
  struct find_container_data *d = param;
  obs_scene_t *container = obs_group_or_scene_from_source(container_source);
  if (!container)
    return true;

  scan_container(d, container);
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

static void record_found_container(struct find_container_data *d,
                                   obs_scene_t *container,
                                   obs_sceneitem_t *item) {
  if (!container || !item)
    return;

  if (!d->found_item) {
    d->found_container = obs_scene_get_ref(container);
    if (!d->found_container)
      return;

    d->found_item = item;
    obs_sceneitem_addref(d->found_item);
  } else if (d->found_item != item) {
    /* Distinct scene items are genuinely ambiguous: video_render receives the
     * source, but not the scene item instance that invoked it.  Encountering
     * the same item again through a nested-scene path is not a duplicate. */
    d->ambiguous = true;
  }
}

static bool check_container_item(obs_scene_t *container, obs_sceneitem_t *item,
                                 void *param) {
  struct find_container_data *d = param;
  obs_source_t *src = obs_sceneitem_get_source(item);

  if (src == d->target) {
    record_found_container(d, container, item);
    return true;
  }

  /* Recurse into both scenes and groups. */
  obs_scene_t *child_container = obs_group_or_scene_from_source(src);
  if (child_container && child_container != container)
    scan_container(d, child_container);

  return true;
}

static obs_scene_t *find_parent_container_for_source(obs_source_t *target,
                                                      bool *ambiguous) {
  DARRAY(obs_scene_t *) scenes;
  struct find_container_data d = {0};

  da_init(scenes);

  /* Collect references while sources_mutex is held, then traverse scenes
   * after obs_enum_scenes releases that global mutex. */
  obs_enum_scenes(collect_scene_ref, &scenes);

  d.target = target;
  d.found_container = NULL;
  d.found_item = NULL;
  da_init(d.visited_containers);
  d.ambiguous = false;

  for (size_t i = 0; i < scenes.num; i++) {
    find_source_in_container(&d, obs_scene_get_source(scenes.array[i]));
    obs_scene_release(scenes.array[i]);
  }

  da_free(scenes);

  if (d.found_item)
    obs_sceneitem_release(d.found_item);

  for (size_t i = 0; i < d.visited_containers.num; i++)
    obs_scene_release(d.visited_containers.array[i]);
  da_free(d.visited_containers);

  if (ambiguous)
    *ambiguous = d.ambiguous;

  return d.found_container;
}

/* ------------------------------------------------------------------------- */
/* Render helpers */
/*   - scene-item show/hide transitions are rendered by their OBS transition
 *     sources; this plugin never starts or advances them itself */
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
  /* Strong reference to the currently running show/hide transition, or NULL
   * during normal visible/hidden states. */
  obs_source_t *transition;
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
    const struct adjustment_layer_item *item, obs_source_t *render_source) {
  if (!item || !render_source)
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

  obs_source_video_render(render_source);

  gs_set_linear_srgb(prev_linear);
  restore_item_srgb_state(prev_srgb);

  gs_blend_state_pop();
  gs_matrix_pop();
}

static void render_item(struct adjustment_layer_source *ctx,
                        const struct adjustment_layer_item *item) {
  if (!ctx || !item || !item->source)
    return;

  /* OBS renders the transition source instead of the item source while a
   * show/hide transition is running.  A hidden item remains renderable only
   * for the lifetime of its hide transition. */
  obs_source_t *render_source = item->transition;
  if (!render_source) {
    if (!item->visible)
      return;
    render_source = item->source;
  }

  uint32_t sw = obs_source_get_width(item->source);
  uint32_t sh = obs_source_get_height(item->source);
  if (sw == 0 || sh == 0)
    return;

  /* Match libobs scene rendering.  The transition owns its timing and child
   * sources; the adjustment layer only supplies the current item dimensions
   * before asking OBS to render it. */
  if (item->transition)
    obs_transition_set_size(item->transition, sw, sh);

  bool force_sub = should_force_sub_render(item->blend_type,
                                           item->blend_method);

  /* Fast path: NORMAL + no crop -> direct render */
  if (!force_sub && is_zero_crop(&item->crop)) {
    render_item_direct_normal_nocrop(item, render_source);
    return;
  }

  /* Otherwise: capture then composite (stable for non-NORMAL and/or crop) */
  gs_texture_t *sub_tex = capture_source_to_sub(ctx, render_source, sw, sh);
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

/* Scene-item transitions are private OBS sources.  Public libobs does not
 * expose its internal transitioning_video flag, so identify the active phase
 * from the expected endpoint plus OBS's video clock.  Scene-item transitions
 * always use automatic timing. */
static obs_source_t *get_active_item_transition(obs_sceneitem_t *item,
                                                bool visible) {
  obs_source_t *transition = obs_sceneitem_get_transition(item, visible);
  if (!transition)
    return NULL;

  const enum obs_transition_target endpoint =
      visible ? OBS_TRANSITION_SOURCE_B : OBS_TRANSITION_SOURCE_A;
  obs_source_t *child = obs_transition_get_source(transition, endpoint);
  if (!child)
    return NULL;
  obs_source_release(child);

  if (obs_transition_get_time(transition) >= 1.0f)
    return NULL;

  return obs_source_get_ref(transition);
}

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
    snapshot.transition =
        get_active_item_transition(item, snapshot.visible);
    snapshot.blend_method = obs_sceneitem_get_blending_method(item);
    snapshot.blend_type = obs_sceneitem_get_blending_mode(item);
    obs_sceneitem_get_crop(item, &snapshot.crop);
    obs_sceneitem_get_draw_transform(item, &snapshot.transform);
    da_push_back(p->items, &snapshot);
  }

  return true;
}

static void release_render_params(struct render_params *p) {
  for (size_t i = 0; i < p->items.num; i++) {
    if (p->items.array[i].transition)
      obs_source_release(p->items.array[i].transition);
    obs_source_release(p->items.array[i].source);
  }

  da_free(p->items);
}

/* ------------------------------------------------------------------------- */
/* Warning overlays                                                          */
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

static const char *warning_texture_directory(
    enum adjustment_layer_warning warning) {
  switch (warning) {
  case ADJUSTMENT_LAYER_WARNING_SHARED_SCENE:
    return "shared-scene";
  case ADJUSTMENT_LAYER_WARNING_GROUP_PLACEMENT:
    return "group-placement";
  case ADJUSTMENT_LAYER_WARNING_COUNT:
    break;
  }

  return NULL;
}

static bool load_warning_texture_file(
    struct adjustment_layer_warning_texture *warning_texture,
    const char *directory, const char *locale) {
  if (!warning_texture || !directory || !locale)
    return false;

  char relative_path[128];
  int written = snprintf(relative_path, sizeof(relative_path),
                         "graphics/warnings/%s/%s.png", directory, locale);
  if (written < 0 || (size_t)written >= sizeof(relative_path))
    return false;

  char *path = obs_module_file(relative_path);
  if (!path)
    return false;

  /* Decode the PNG and premultiply alpha before creating the GPU texture.
   * The call is made from video_render, where the OBS graphics context is
   * current; gs_image_file4_free is likewise called on that context. */
  gs_image_file4_init(&warning_texture->image, path,
                      GS_IMAGE_ALPHA_PREMULTIPLY_SRGB);
  bfree(path);

  if (!warning_texture->image.image3.image2.image.loaded)
    return false;

  gs_image_file4_init_texture(&warning_texture->image);
  if (!warning_texture->image.image3.image2.image.texture) {
    gs_image_file4_free(&warning_texture->image);
    return false;
  }

  return true;
}

static bool ensure_warning_texture(
    struct adjustment_layer_source *ctx,
    enum adjustment_layer_warning warning) {
  if (!ctx || warning >= ADJUSTMENT_LAYER_WARNING_COUNT)
    return false;

  struct adjustment_layer_warning_texture *warning_texture =
      &ctx->warning_textures[warning];
  if (warning_texture->attempted)
    return warning_texture->image.image3.image2.image.texture != NULL;

  warning_texture->attempted = true;

  const char *locale = warning_texture_locale();
  const char *directory = warning_texture_directory(warning);
  if (load_warning_texture_file(warning_texture, directory, locale))
    return true;

  /* If a localized asset is missing from a package, keep the warning useful
   * by trying the guaranteed English asset once. */
  if (strcmp(locale, "en-US") != 0) {
    gs_image_file4_free(&warning_texture->image);
    if (load_warning_texture_file(warning_texture, directory, "en-US")) {
      blog(LOG_WARNING,
           "[adjustment-layer] %s warning texture for locale '%s' was not "
           "available; using en-US",
           directory, locale);
      return true;
    }
  }

  /* group-placement artwork is supplied independently from the plugin code.
   * Until it is packaged, use the existing localized warning instead of
   * silently rendering an empty source. */
  if (warning == ADJUSTMENT_LAYER_WARNING_GROUP_PLACEMENT) {
    gs_image_file4_free(&warning_texture->image);
    if (load_warning_texture_file(warning_texture, "shared-scene", locale)) {
      blog(LOG_WARNING,
           "[adjustment-layer] group-placement warning artwork is not "
           "available; using the shared-scene warning artwork temporarily");
      return true;
    }

    if (strcmp(locale, "en-US") != 0) {
      gs_image_file4_free(&warning_texture->image);
      if (load_warning_texture_file(warning_texture, "shared-scene",
                                    "en-US")) {
        blog(LOG_WARNING,
             "[adjustment-layer] group-placement warning artwork is not "
             "available; using the en-US shared-scene warning artwork "
             "temporarily");
        return true;
      }
    }
  }

  blog(LOG_WARNING,
       "[adjustment-layer] failed to load the %s warning texture for locale "
       "'%s'",
       directory, locale);
  return false;
}

static void draw_warning(struct adjustment_layer_source *ctx,
                         enum adjustment_layer_warning warning) {
  if (!ctx || ctx->width == 0 || ctx->height == 0 ||
      !ensure_warning_texture(ctx, warning))
    return;

  const struct gs_image_file *image =
      &ctx->warning_textures[warning].image.image3.image2.image;
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

  bool has_warning_resource = false;
  for (size_t i = 0; i < ADJUSTMENT_LAYER_WARNING_COUNT; i++)
    has_warning_resource |= ctx->warning_textures[i].attempted;

  if (ctx->render || ctx->sub_render || has_warning_resource) {
    /* Source destruction already runs on OBS's destruction thread.  Acquire
     * the graphics context here so the resources cannot outlive this module
     * or a stopped graphics thread. */
    obs_enter_graphics();
    if (ctx->render)
      gs_texrender_destroy(ctx->render);
    if (ctx->sub_render)
      gs_texrender_destroy(ctx->sub_render);
    for (size_t i = 0; i < ADJUSTMENT_LAYER_WARNING_COUNT; i++)
      gs_image_file4_free(&ctx->warning_textures[i].image);
    obs_leave_graphics();
  }

  if (ctx->cached_container)
    obs_weak_source_release(ctx->cached_container);

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

  if (ctx->ambiguous_container) {
    /* Shared placement cannot be rendered correctly because OBS does not
     * identify the parent scene/group item here.  Show the actionable warning
     * instead of silently returning a transparent frame. */
    draw_warning(ctx, ADJUSTMENT_LAYER_WARNING_SHARED_SCENE);
    return;
  }

  if (ctx->inside_group) {
    /* A transparent group cannot safely use the overlay-based adjustment
     * technique: alpha-reducing filters reveal the already-rendered original
     * items.  Stop before enumerating or rendering any child source. */
    draw_warning(ctx, ADJUSTMENT_LAYER_WARNING_GROUP_PLACEMENT);
    return;
  }

  if (!ctx->cached_container)
    return;

  /* The layer renders child scenes/sources manually.  Refuse re-entry rather
   * than beginning the same texrender while it is already active. */
  if (ctx->rendering)
    return;

  obs_source_t *cached_container_source =
      obs_weak_source_get_source(ctx->cached_container);
  if (!cached_container_source)
    return;

  /* Resolve the cached owner after acquiring a temporary strong reference.
   * Groups have already been rejected above, but the combined helper keeps
   * this lifetime-sensitive conversion valid if the placement changes near a
   * video tick boundary. */
  obs_scene_t *cached_container =
      obs_group_or_scene_from_source(cached_container_source);
  if (!cached_container) {
    obs_source_release(cached_container_source);
    return;
  }

  const uint64_t frame_time = obs_get_video_frame_time();
  if (ctx->has_rendered_frame && ctx->rendered_frame_time == frame_time) {
    obs_source_release(cached_container_source);
    draw_layer_texture(ctx);
    return;
  }

  ctx->rendering = true;

  struct render_params p = {0};
  p.target = ctx->source;
  da_init(p.items);

  /* Only snapshot item/source state while the scene mutexes are held.  The
   * actual source and Lua-filter rendering happens after this call returns. */
  obs_scene_enum_items(cached_container, collect_render_item, &p);
  obs_source_release(cached_container_source);

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

  bool ambiguous_container = false;
  bool inside_group = false;
  obs_scene_t *new_container =
      find_parent_container_for_source(ctx->source, &ambiguous_container);
  obs_weak_source_t *new_cached_container = NULL;

  if (ambiguous_container) {
    ctx->warned_group_container = false;

    if (!ctx->warned_ambiguous_container) {
      blog(LOG_WARNING,
           "[adjustment-layer] source '%s' is used by multiple scene/group "
           "items; adjustment processing is disabled for safety. Create a "
           "separate Adjustment Layer source for each container",
           obs_source_get_name(ctx->source));
      ctx->warned_ambiguous_container = true;
    }

    if (new_container)
      obs_scene_release(new_container);
    new_container = NULL;
  } else {
    ctx->warned_ambiguous_container = false;

    if (new_container) {
      inside_group = obs_scene_is_group(new_container);

      if (inside_group) {
        if (!ctx->warned_group_container) {
          blog(LOG_WARNING,
               "[adjustment-layer] source '%s' is inside a group; adjustment "
               "processing is disabled. Move the Adjustment Layer directly "
               "under a scene",
               obs_source_get_name(ctx->source));
          ctx->warned_group_container = true;
        }
      } else {
        ctx->warned_group_container = false;
      }

      /* Scene-item setters can emit signals and update transforms.  Do this
       * during video_tick, never from video_render while the parent
       * scene/group's video mutex is held. */
      struct find_item_params item_params = {
          .target = ctx->source,
          .item = NULL,
      };
      obs_scene_enum_items(new_container, find_target_item, &item_params);

      obs_source_t *container_source = obs_scene_get_source(new_container);
      if (container_source)
        new_cached_container = obs_source_get_weak_source(container_source);
      obs_scene_release(new_container);
      new_container = NULL;

      if (item_params.item) {
        enforce_item_state(item_params.item);
        obs_sceneitem_release(item_params.item);
      }
    } else {
      ctx->warned_group_container = false;
    }
  }

  obs_weak_source_t *old_container = ctx->cached_container;
  ctx->cached_container = new_cached_container;
  ctx->ambiguous_container = ambiguous_container;
  ctx->inside_group = inside_group;
  if (old_container)
    obs_weak_source_release(old_container);
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
