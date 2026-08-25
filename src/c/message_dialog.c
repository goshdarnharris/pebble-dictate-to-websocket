#include "message_dialog.h"

#include <stdlib.h>

#define DIALOG_MARGIN 16
#define DIALOG_ICON_SIZE 48
#define DIALOG_ANIMATION_DISTANCE 10
#define DIALOG_ANIMATION_DURATION_MS 100
#define DIALOG_TEXT_MEASUREMENT_HEIGHT 2000

typedef enum {
  MESSAGE_DIALOG_ANIMATION_NONE,
  MESSAGE_DIALOG_ANIMATION_OUT,
  MESSAGE_DIALOG_ANIMATION_IN,
} MessageDialogAnimationPhase;

struct MessageDialog {
  Layer *background_layer;
  Layer *icon_layer;
  TextLayer *text_layer;
  ScrollLayer *result_scroll_layer;
  TextLayer *result_body_layer;
  PropertyAnimation *property_animation;
  Animation *animation;
  GRect bounds;
  GRect status_frame;
  MessageDialogTone tone;
  MessageDialogIcon icon;
  MessageDialogTone pending_tone;
  MessageDialogIcon pending_icon;
  MessageDialogAnimationPhase animation_phase;
  const char *pending_text;
  MessageDialogTransitionComplete completion;
  void *completion_context;
  bool destroying;
};

static GColor prv_tone_color(MessageDialogTone tone) {
  switch (tone) {
    case MESSAGE_DIALOG_TONE_SUCCESS:
      return GColorGreen;
    case MESSAGE_DIALOG_TONE_FAILURE:
      return GColorSunsetOrange;
    case MESSAGE_DIALOG_TONE_PROGRESS:
    default:
      return GColorVividCerulean;
  }
}

static void prv_background_update_proc(Layer *layer, GContext *context) {
  MessageDialog *dialog = *(MessageDialog **)layer_get_data(layer);
  light_set_color(prv_tone_color(dialog->tone));
  graphics_context_set_fill_color(context, prv_tone_color(dialog->tone));
  graphics_fill_rect(context, layer_get_bounds(layer), 0, GCornerNone);
}

static void prv_icon_update_proc(Layer *layer, GContext *context) {
  MessageDialog *dialog = *(MessageDialog **)layer_get_data(layer);
  const GRect bounds = layer_get_bounds(layer);
  const GPoint center = grect_center_point(&bounds);

  graphics_context_set_antialiased(context, true);
  graphics_context_set_stroke_color(context, GColorBlack);
  graphics_context_set_fill_color(context, GColorBlack);
  graphics_context_set_stroke_width(context, 3);

  switch (dialog->icon) {
    case MESSAGE_DIALOG_ICON_SUCCESS:
      graphics_draw_circle(context, center, 20);
      graphics_draw_line(context, GPoint(12, 25), GPoint(21, 34));
      graphics_draw_line(context, GPoint(21, 34), GPoint(38, 15));
      break;

    case MESSAGE_DIALOG_ICON_WARNING:
      graphics_draw_line(context, GPoint(24, 3), GPoint(45, 43));
      graphics_draw_line(context, GPoint(45, 43), GPoint(3, 43));
      graphics_draw_line(context, GPoint(3, 43), GPoint(24, 3));
      graphics_draw_line(context, GPoint(24, 15), GPoint(24, 30));
      graphics_fill_circle(context, GPoint(24, 37), 2);
      break;

    case MESSAGE_DIALOG_ICON_WORKING:
    default:
      graphics_fill_circle(context, GPoint(17, 15), 8);
      graphics_fill_circle(context, GPoint(31, 15), 8);
      graphics_fill_circle(context, GPoint(13, 24), 8);
      graphics_fill_circle(context, GPoint(35, 24), 8);
      graphics_fill_circle(context, GPoint(17, 33), 8);
      graphics_fill_circle(context, GPoint(31, 33), 8);
      graphics_fill_circle(context, center, 13);

      graphics_context_set_stroke_color(context,
                                        prv_tone_color(dialog->tone));
      graphics_draw_line(context, GPoint(24, 9), GPoint(24, 39));
      graphics_draw_line(context, GPoint(13, 19), GPoint(20, 22));
      graphics_draw_line(context, GPoint(20, 22), GPoint(17, 27));
      graphics_draw_line(context, GPoint(14, 30), GPoint(20, 30));
      graphics_draw_line(context, GPoint(20, 30), GPoint(19, 36));
      graphics_draw_line(context, GPoint(35, 19), GPoint(28, 22));
      graphics_draw_line(context, GPoint(28, 22), GPoint(31, 27));
      graphics_draw_line(context, GPoint(34, 30), GPoint(28, 30));
      graphics_draw_line(context, GPoint(28, 30), GPoint(29, 36));
      break;
  }
}

static void prv_destroy_result_layers(MessageDialog *dialog) {
  if (dialog->result_body_layer) {
    text_layer_destroy(dialog->result_body_layer);
    dialog->result_body_layer = NULL;
  }
  if (dialog->result_scroll_layer) {
    scroll_layer_destroy(dialog->result_scroll_layer);
    dialog->result_scroll_layer = NULL;
  }
}

static void prv_apply_status(MessageDialog *dialog) {
  dialog->tone = dialog->pending_tone;
  dialog->icon = dialog->pending_icon;
  text_layer_set_text(dialog->text_layer, dialog->pending_text);
  layer_mark_dirty(dialog->background_layer);
  layer_mark_dirty(dialog->icon_layer);
}

static void prv_complete_transition(MessageDialog *dialog) {
  MessageDialogTransitionComplete completion = dialog->completion;
  void *completion_context = dialog->completion_context;
  dialog->completion = NULL;
  dialog->completion_context = NULL;
  dialog->pending_text = NULL;
  if (completion) {
    completion(completion_context);
  }
}

static bool prv_start_animation(MessageDialog *dialog, GRect start,
                                GRect finish,
                                MessageDialogAnimationPhase phase);

static void prv_animation_stopped(Animation *animation, bool finished,
                                  void *context) {
  MessageDialog *dialog = context;
  if (animation != dialog->animation) {
    return;
  }

  PropertyAnimation *property_animation = dialog->property_animation;
  const MessageDialogAnimationPhase phase = dialog->animation_phase;
  dialog->property_animation = NULL;
  dialog->animation = NULL;
  dialog->animation_phase = MESSAGE_DIALOG_ANIMATION_NONE;
  property_animation_destroy(property_animation);

  if (!finished || dialog->destroying) {
    layer_set_frame(text_layer_get_layer(dialog->text_layer),
                    dialog->status_frame);
    dialog->completion = NULL;
    dialog->completion_context = NULL;
    dialog->pending_text = NULL;
    return;
  }

  if (phase == MESSAGE_DIALOG_ANIMATION_OUT) {
    prv_apply_status(dialog);
    const GRect start = GRect(
        dialog->status_frame.origin.x + DIALOG_ANIMATION_DISTANCE,
        dialog->status_frame.origin.y, dialog->status_frame.size.w,
        dialog->status_frame.size.h);
    if (!prv_start_animation(dialog, start, dialog->status_frame,
                             MESSAGE_DIALOG_ANIMATION_IN)) {
      layer_set_frame(text_layer_get_layer(dialog->text_layer),
                      dialog->status_frame);
      prv_complete_transition(dialog);
    }
    return;
  }

  layer_set_frame(text_layer_get_layer(dialog->text_layer),
                  dialog->status_frame);
  prv_complete_transition(dialog);
}

static bool prv_start_animation(MessageDialog *dialog, GRect start,
                                GRect finish,
                                MessageDialogAnimationPhase phase) {
  Layer *text_layer = text_layer_get_layer(dialog->text_layer);
  PropertyAnimation *property_animation =
      property_animation_create_layer_frame(text_layer, &start, &finish);
  if (!property_animation) {
    return false;
  }

  Animation *animation =
      property_animation_get_animation(property_animation);
  animation_set_duration(animation, DIALOG_ANIMATION_DURATION_MS);
  animation_set_curve(animation, AnimationCurveEaseInOut);
  animation_set_handlers(
      animation,
      (AnimationHandlers){
          .stopped = prv_animation_stopped,
      },
      dialog);

  dialog->property_animation = property_animation;
  dialog->animation = animation;
  dialog->animation_phase = phase;
  if (!animation_schedule(animation)) {
    dialog->property_animation = NULL;
    dialog->animation = NULL;
    dialog->animation_phase = MESSAGE_DIALOG_ANIMATION_NONE;
    property_animation_destroy(property_animation);
    return false;
  }
  return true;
}

static void prv_cancel_animation(MessageDialog *dialog) {
  if (dialog->animation) {
    animation_unschedule(dialog->animation);
  }
}

MessageDialog *message_dialog_create(Layer *parent, GRect bounds,
                                     const char *initial_text) {
  MessageDialog *dialog = calloc(1, sizeof(*dialog));
  if (!dialog) {
    return NULL;
  }

  dialog->bounds = bounds;
  dialog->status_frame = GRect(
      DIALOG_MARGIN, 92, bounds.size.w - 2 * DIALOG_MARGIN,
      bounds.size.h - 92 - DIALOG_MARGIN);
  dialog->tone = MESSAGE_DIALOG_TONE_PROGRESS;
  dialog->icon = MESSAGE_DIALOG_ICON_WORKING;

  dialog->background_layer =
      layer_create_with_data(bounds, sizeof(MessageDialog *));
  dialog->icon_layer = layer_create_with_data(
      GRect(DIALOG_MARGIN, DIALOG_MARGIN, DIALOG_ICON_SIZE, DIALOG_ICON_SIZE),
      sizeof(MessageDialog *));
  dialog->text_layer = text_layer_create(dialog->status_frame);
  if (!dialog->background_layer || !dialog->icon_layer ||
      !dialog->text_layer) {
    message_dialog_destroy(dialog);
    return NULL;
  }

  *(MessageDialog **)layer_get_data(dialog->background_layer) = dialog;
  *(MessageDialog **)layer_get_data(dialog->icon_layer) = dialog;
  layer_set_update_proc(dialog->background_layer,
                        prv_background_update_proc);
  layer_set_update_proc(dialog->icon_layer, prv_icon_update_proc);

  text_layer_set_background_color(dialog->text_layer, GColorClear);
  text_layer_set_text_color(dialog->text_layer, GColorBlack);
  text_layer_set_font(dialog->text_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(dialog->text_layer, GTextAlignmentLeft);
  text_layer_set_overflow_mode(dialog->text_layer,
                               GTextOverflowModeWordWrap);
  text_layer_set_text(dialog->text_layer, initial_text);

  layer_add_child(parent, dialog->background_layer);
  layer_add_child(parent, dialog->icon_layer);
  layer_add_child(parent, text_layer_get_layer(dialog->text_layer));
  return dialog;
}

void message_dialog_destroy(MessageDialog *dialog) {
  if (!dialog) {
    return;
  }

  dialog->destroying = true;
  prv_cancel_animation(dialog);
  prv_destroy_result_layers(dialog);
  if (dialog->text_layer) {
    text_layer_destroy(dialog->text_layer);
  }
  if (dialog->icon_layer) {
    layer_destroy(dialog->icon_layer);
  }
  if (dialog->background_layer) {
    layer_destroy(dialog->background_layer);
  }
  free(dialog);
}

void message_dialog_show_status(MessageDialog *dialog, const char *text,
                                MessageDialogTone tone,
                                MessageDialogIcon icon, bool animated,
                                MessageDialogTransitionComplete completion,
                                void *completion_context) {
  if (!dialog || !text) {
    if (completion) {
      completion(completion_context);
    }
    return;
  }

  prv_cancel_animation(dialog);
  prv_destroy_result_layers(dialog);
  dialog->status_frame = GRect(
      DIALOG_MARGIN, 92, dialog->bounds.size.w - 2 * DIALOG_MARGIN,
      dialog->bounds.size.h - 92 - DIALOG_MARGIN);
  layer_set_frame(text_layer_get_layer(dialog->text_layer),
                  dialog->status_frame);
  layer_set_hidden(text_layer_get_layer(dialog->text_layer), false);
  dialog->pending_text = text;
  dialog->pending_tone = tone;
  dialog->pending_icon = icon;
  dialog->completion = completion;
  dialog->completion_context = completion_context;

  if (!animated) {
    prv_apply_status(dialog);
    prv_complete_transition(dialog);
    return;
  }

  const GRect finish = GRect(
      dialog->status_frame.origin.x - DIALOG_ANIMATION_DISTANCE,
      dialog->status_frame.origin.y, dialog->status_frame.size.w,
      dialog->status_frame.size.h);
  if (!prv_start_animation(dialog, dialog->status_frame, finish,
                           MESSAGE_DIALOG_ANIMATION_OUT)) {
    prv_apply_status(dialog);
    layer_set_frame(text_layer_get_layer(dialog->text_layer),
                    dialog->status_frame);
    prv_complete_transition(dialog);
  }
}

bool message_dialog_show_result(MessageDialog *dialog, Window *window,
                                const char *body, bool success,
                                ClickConfigProvider click_config_provider) {
  if (!dialog || !window || !body) {
    return false;
  }

  prv_cancel_animation(dialog);
  prv_destroy_result_layers(dialog);
  dialog->tone = success ? MESSAGE_DIALOG_TONE_SUCCESS
                         : MESSAGE_DIALOG_TONE_FAILURE;
  const GColor result_color = prv_tone_color(dialog->tone);
  dialog->icon = success ? MESSAGE_DIALOG_ICON_SUCCESS
                         : MESSAGE_DIALOG_ICON_WARNING;
  window_set_background_color(window, result_color);
  
  layer_set_hidden(text_layer_get_layer(dialog->text_layer), true);
  layer_mark_dirty(dialog->background_layer);
  layer_mark_dirty(dialog->icon_layer);

  const int16_t scroll_y = 80;
  const GRect scroll_frame = GRect(
      DIALOG_MARGIN, scroll_y,
      dialog->bounds.size.w - 2 * DIALOG_MARGIN,
      dialog->bounds.size.h - scroll_y - DIALOG_MARGIN);
  dialog->result_scroll_layer = scroll_layer_create(scroll_frame);
  dialog->result_body_layer = text_layer_create(
      GRect(0, 0, scroll_frame.size.w, DIALOG_TEXT_MEASUREMENT_HEIGHT));
  if (!dialog->result_scroll_layer || !dialog->result_body_layer) {
    prv_destroy_result_layers(dialog);
    return false;
  }

  text_layer_set_background_color(dialog->result_body_layer, result_color);
  text_layer_set_text_color(dialog->result_body_layer, GColorBlack);
  text_layer_set_font(dialog->result_body_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_overflow_mode(dialog->result_body_layer,
                               GTextOverflowModeWordWrap);
  text_layer_set_text_alignment(dialog->result_body_layer,
                                GTextAlignmentLeft);
  text_layer_set_text(dialog->result_body_layer, body);

  GSize content_size = text_layer_get_content_size(dialog->result_body_layer);
  content_size.w = scroll_frame.size.w;
  if (content_size.h < scroll_frame.size.h) {
    content_size.h = scroll_frame.size.h;
  }
  layer_set_frame(text_layer_get_layer(dialog->result_body_layer),
                  GRect(0, 0, scroll_frame.size.w, content_size.h));
  scroll_layer_add_child(dialog->result_scroll_layer,
                         text_layer_get_layer(dialog->result_body_layer));
  scroll_layer_set_content_size(dialog->result_scroll_layer, content_size);
  scroll_layer_set_content_offset(dialog->result_scroll_layer, GPointZero,
                                  false);
  scroll_layer_set_shadow_hidden(dialog->result_scroll_layer, true);
  scroll_layer_set_callbacks(
      dialog->result_scroll_layer,
      (ScrollLayerCallbacks){
          .click_config_provider = click_config_provider,
      });
  scroll_layer_set_click_config_onto_window(dialog->result_scroll_layer,
                                             window);
  layer_add_child(window_get_root_layer(window),
                  scroll_layer_get_layer(dialog->result_scroll_layer));
  return true;
}