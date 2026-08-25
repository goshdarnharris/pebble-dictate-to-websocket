#pragma once

#include <pebble.h>

typedef struct MessageDialog MessageDialog;

typedef enum {
  MESSAGE_DIALOG_TONE_PROGRESS,
  MESSAGE_DIALOG_TONE_SUCCESS,
  MESSAGE_DIALOG_TONE_FAILURE,
} MessageDialogTone;

typedef enum {
  MESSAGE_DIALOG_ICON_WORKING,
  MESSAGE_DIALOG_ICON_SUCCESS,
  MESSAGE_DIALOG_ICON_WARNING,
} MessageDialogIcon;

typedef void (*MessageDialogTransitionComplete)(void *context);

MessageDialog *message_dialog_create(Layer *parent, GRect bounds,
                                     const char *initial_text);
void message_dialog_destroy(MessageDialog *dialog);

void message_dialog_show_status(MessageDialog *dialog, const char *text,
                                MessageDialogTone tone,
                                MessageDialogIcon icon, bool animated,
                                MessageDialogTransitionComplete completion,
                                void *completion_context);

bool message_dialog_show_result(MessageDialog *dialog, Window *window,
                                const char *body, bool success,
                                ClickConfigProvider click_config_provider);