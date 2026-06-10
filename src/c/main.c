#include <pebble.h>
#include <string.h>
#include <stdio.h>

// ─── Constants ───────────────────────────────────────────────
#define MAX_ITEMS    50
#define STR_LEN      128
#define SETTINGS_KEY 1
#define REFRESH_MS   30000

#define CMD_ACTIVITY 1
#define CMD_HISTORY  2
#define CMD_RECENT   3
#define CMD_METADATA 4

#define MARQUEE_STEP_MS  30    // ms between pixel steps
#define MARQUEE_PX       2     // pixels per step
#define MARQUEE_HOLD_MS  1200  // pause at start and end of each cycle
#define MARQUEE_MAX_PX   400   // max scroll distance per cycle

// ─── Types ───────────────────────────────────────────────────
typedef struct {
  char host[64];
  char port[8];
  char path[32];
  char api_key[36];
} AppSettings;

// ─── State ───────────────────────────────────────────────────
static AppSettings s_settings;

// List data
static char s_titles[MAX_ITEMS][STR_LEN];
static char s_subtitles[MAX_ITEMS][STR_LEN];
static char s_details[MAX_ITEMS][STR_LEN];
static char s_rating_keys[MAX_ITEMS][16];
static int  s_item_count     = 0;  // total items loaded so far
static int  s_expected_count = 0;  // items expected in current batch
static int  s_load_offset    = 0;  // array offset for current batch (pagination)
static bool s_loading        = false;
static bool s_loading_more   = false;
static bool s_has_more       = false;
static char s_error[STR_LEN] = "";
static int  s_current_cmd    = CMD_ACTIVITY;
static char s_list_title[32] = "Tautulli";
static int  s_selected_idx   = 0;

// Detail window buffers (global so inbox_received can update them)
static char s_det_title_buf[STR_LEN];
static char s_det_info_buf[1024];
static bool s_detail_pending_meta = false;

// Windows
static Window *s_main_window;
static Window *s_list_window;
static Window *s_detail_window;

// Layers
static MenuLayer *s_main_menu_layer;
static MenuLayer *s_list_menu_layer;
static TextLayer *s_det_title_layer;
static TextLayer *s_det_info_layer;
static ScrollLayer *s_det_scroll_layer;

// Timer
static AppTimer *s_refresh_timer = NULL;

// Subtitle marquee
static int        s_marquee_offset  = 0;
static bool       s_marquee_at_end  = false;
static AppTimer  *s_marquee_timer   = NULL;

// Main menu icons
static GBitmap *s_icons[3];

// ─── Forward Declarations ────────────────────────────────────
static void prv_show_list(int cmd, const char *title);
static void prv_send_command(int type, const char *param);
static void prv_refresh_detail(void);

// ─── Settings ────────────────────────────────────────────────
static void prv_default_settings(void) {
  strncpy(s_settings.host,    "https://tautulli.server.com", sizeof(s_settings.host)    - 1);
  strncpy(s_settings.port,    "",                             sizeof(s_settings.port)    - 1);
  strncpy(s_settings.path,    "",                             sizeof(s_settings.path)    - 1);
  s_settings.api_key[0] = '\0';
}

static void prv_load_settings(void) {
  prv_default_settings();
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

static void prv_save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
}

// ─── Commands ────────────────────────────────────────────────
static void prv_send_command(int type, const char *param) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_int32(iter, MESSAGE_KEY_CMD_TYPE, type);
  if (param && param[0]) {
    dict_write_cstring(iter, MESSAGE_KEY_CMD_PARAM, param);
  }
  app_message_outbox_send();
}

// ─── Detail window refresh ───────────────────────────────────
static void prv_fit_scroll(void) {
  if (!s_det_scroll_layer || !s_det_info_layer) return;
  GRect sb = layer_get_bounds(scroll_layer_get_layer(s_det_scroll_layer));
  // Expand text layer height before measuring so content_size isn't capped
  GRect tf = layer_get_frame(text_layer_get_layer(s_det_info_layer));
  tf.size.h = 4000;
  layer_set_frame(text_layer_get_layer(s_det_info_layer), tf);
  GSize content = text_layer_get_content_size(s_det_info_layer);
  int h = content.h + 40;
  if (h < sb.size.h) h = sb.size.h;
  tf.size.h = h;
  layer_set_frame(text_layer_get_layer(s_det_info_layer), tf);
  scroll_layer_set_content_size(s_det_scroll_layer, GSize(sb.size.w, h));
}

static void prv_refresh_detail(void) {
  if (!s_det_title_layer || !s_det_info_layer) return;
  text_layer_set_text(s_det_title_layer, s_det_title_buf);
  text_layer_set_text(s_det_info_layer, s_det_info_buf);
  prv_fit_scroll();
}

// ─── AppMessage ───────────────────────────────────────────────
static void prv_inbox_received(DictionaryIterator *iterator, void *context) {
  Tuple *t;
  bool settings_changed = false;

  // Settings from Clay
  if ((t = dict_find(iterator, MESSAGE_KEY_TautulliHost))) {
    strncpy(s_settings.host, t->value->cstring, sizeof(s_settings.host) - 1);
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_TautulliPort))) {
    strncpy(s_settings.port, t->value->cstring, sizeof(s_settings.port) - 1);
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_TautulliPath))) {
    strncpy(s_settings.path, t->value->cstring, sizeof(s_settings.path) - 1);
    settings_changed = true;
  }
  if ((t = dict_find(iterator, MESSAGE_KEY_TautulliApiKey))) {
    strncpy(s_settings.api_key, t->value->cstring, sizeof(s_settings.api_key) - 1);
    settings_changed = true;
  }
  if (settings_changed) {
    prv_save_settings();
    if (s_main_menu_layer) menu_layer_reload_data(s_main_menu_layer);
    return;
  }

  // Error
  if ((t = dict_find(iterator, MESSAGE_KEY_DATA_ERROR))) {
    s_loading      = false;
    s_loading_more = false;
    strncpy(s_error, t->value->cstring, STR_LEN - 1);
    if (!s_loading_more) s_item_count = s_load_offset;  // keep existing items on error
    if (s_list_menu_layer) menu_layer_reload_data(s_list_menu_layer);
    return;
  }

  // "Has more" flag — arrives alongside DATA_COUNT (ignored for metadata responses)
  if ((t = dict_find(iterator, MESSAGE_KEY_DATA_HAS_MORE)) && !s_detail_pending_meta) {
    s_has_more = (t->value->int32 == 1);
  }

  // Item count for this batch
  if ((t = dict_find(iterator, MESSAGE_KEY_DATA_COUNT))) {
    int batch = (int)t->value->int32;

    // Metadata response — don't touch list state at all, just record expected count
    if (s_detail_pending_meta) {
      s_expected_count = batch;
      return;
    }

    s_expected_count = batch;
    s_error[0] = '\0';

    if (!s_loading_more) {
      // Fresh load — reset list
      s_item_count  = 0;
      s_load_offset = 0;
    }

    if (batch == 0) {
      s_loading      = false;
      s_loading_more = false;
      if (s_item_count == 0) strncpy(s_error, "No items found", STR_LEN - 1);
    }
    if (s_list_menu_layer) menu_layer_reload_data(s_list_menu_layer);
    return;
  }

  // Data item
  if ((t = dict_find(iterator, MESSAGE_KEY_DATA_INDEX))) {
    int rel_idx = (int)t->value->int32;

    // Metadata response — update detail window buffers directly, don't touch list
    if (s_detail_pending_meta) {
      if (rel_idx == 0) {
        Tuple *tt = dict_find(iterator, MESSAGE_KEY_DATA_TITLE);
        Tuple *st = dict_find(iterator, MESSAGE_KEY_DATA_SUBTITLE);
        Tuple *dt = dict_find(iterator, MESSAGE_KEY_DATA_DETAIL);
        if (tt) strncpy(s_det_title_buf, tt->value->cstring, STR_LEN - 1);
        if (st || dt) {
          snprintf(s_det_info_buf, sizeof(s_det_info_buf), "%s\n\n%s",
                   st ? st->value->cstring : "",
                   dt ? dt->value->cstring : "");
        }
        s_detail_pending_meta = false;
        prv_refresh_detail();
      }
      return;
    }

    // Normal list item
    int abs_idx = s_load_offset + rel_idx;
    if (abs_idx < 0 || abs_idx >= MAX_ITEMS) return;

    Tuple *tt = dict_find(iterator, MESSAGE_KEY_DATA_TITLE);
    Tuple *st = dict_find(iterator, MESSAGE_KEY_DATA_SUBTITLE);
    Tuple *dt = dict_find(iterator, MESSAGE_KEY_DATA_DETAIL);
    Tuple *rt = dict_find(iterator, MESSAGE_KEY_DATA_RATING_KEY);

    strncpy(s_titles[abs_idx],      tt ? tt->value->cstring : "",  STR_LEN - 1);
    strncpy(s_subtitles[abs_idx],   st ? st->value->cstring : "",  STR_LEN - 1);
    strncpy(s_details[abs_idx],     dt ? dt->value->cstring : "",  STR_LEN - 1);
    strncpy(s_rating_keys[abs_idx], rt ? rt->value->cstring : "",  15);

    if (abs_idx + 1 > s_item_count) s_item_count = abs_idx + 1;

    if ((rel_idx + 1) >= s_expected_count && s_expected_count > 0) {
      s_loading      = false;
      s_loading_more = false;
      if (s_list_menu_layer) menu_layer_reload_data(s_list_menu_layer);
    }
  }
}

static void prv_inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", (int)reason);
}

// ─── Detail Window ───────────────────────────────────────────
static void detail_window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  window_set_background_color(window, GColorOxfordBlue);

  // Title layer (pinned at top, not inside scroll)
  GRect title_frame = GRect(4, 8, bounds.size.w - 8, 66);
  s_det_title_layer = text_layer_create(title_frame);
  text_layer_set_text(s_det_title_layer, s_det_title_buf);
  text_layer_set_text_color(s_det_title_layer, GColorWhite);
  text_layer_set_background_color(s_det_title_layer, GColorClear);
  text_layer_set_font(s_det_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_overflow_mode(s_det_title_layer, GTextOverflowModeWordWrap);
  layer_add_child(root, text_layer_get_layer(s_det_title_layer));

  // Scroll layer fills the area below the title
  GRect scroll_frame = GRect(0, 78, bounds.size.w, bounds.size.h - 78);
  s_det_scroll_layer = scroll_layer_create(scroll_frame);
  scroll_layer_set_click_config_onto_window(s_det_scroll_layer, window);
  layer_add_child(root, scroll_layer_get_layer(s_det_scroll_layer));

  // Text layer inside scroll — start tall enough for any content
  GRect info_frame = GRect(4, 4, bounds.size.w - 8, 2000);
  s_det_info_layer = text_layer_create(info_frame);
  text_layer_set_text(s_det_info_layer, s_det_info_buf);
  text_layer_set_text_color(s_det_info_layer, GColorLightGray);
  text_layer_set_background_color(s_det_info_layer, GColorClear);
  text_layer_set_font(s_det_info_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_det_info_layer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_det_scroll_layer, text_layer_get_layer(s_det_info_layer));

  // Size scroll content; prv_fit_scroll resets height to 4000 before measuring
  // so the content_size calculation isn't capped by the current frame height
  prv_fit_scroll();
}

static void detail_window_unload(Window *window) {
  text_layer_destroy(s_det_info_layer);
  s_det_info_layer = NULL;
  scroll_layer_destroy(s_det_scroll_layer);
  s_det_scroll_layer = NULL;
  text_layer_destroy(s_det_title_layer);
  s_det_title_layer = NULL;
  s_detail_pending_meta = false;
}

// ─── List Window ─────────────────────────────────────────────
static uint16_t list_num_sections(MenuLayer *l, void *ctx) { return 1; }

static uint16_t list_num_rows(MenuLayer *l, uint16_t section, void *ctx) {
  if (s_loading && s_item_count == 0) return 1;  // "Loading…" row
  if (s_item_count == 0) return 1;                // error/empty row
  return (uint16_t)(s_item_count + (s_has_more ? 1 : 0));
}

static int16_t list_row_height(MenuLayer *l, MenuIndex *idx, void *ctx) {
  return 50;
}

static int16_t list_header_height(MenuLayer *l, uint16_t section, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

// ─── Subtitle Marquee ────────────────────────────────────────
static void prv_marquee_tick(void *ctx);

static void prv_stop_marquee(void) {
  if (s_marquee_timer) { app_timer_cancel(s_marquee_timer); s_marquee_timer = NULL; }
  s_marquee_offset = 0;
  s_marquee_at_end = false;
}

static void prv_start_marquee(void) {
  prv_stop_marquee();
  // Initial hold before first scroll begins
  s_marquee_timer = app_timer_register(MARQUEE_HOLD_MS, prv_marquee_tick, NULL);
}

static void prv_marquee_tick(void *ctx) {
  s_marquee_timer = NULL;
  if (s_marquee_at_end) {
    // End-of-cycle hold elapsed — reset to start
    s_marquee_at_end = false;
    s_marquee_offset = 0;
    s_marquee_timer  = app_timer_register(MARQUEE_HOLD_MS, prv_marquee_tick, NULL);
  } else {
    s_marquee_offset += MARQUEE_PX;
    if (s_marquee_offset >= MARQUEE_MAX_PX) {
      s_marquee_offset = MARQUEE_MAX_PX;
      s_marquee_at_end = true;
      s_marquee_timer  = app_timer_register(MARQUEE_HOLD_MS, prv_marquee_tick, NULL);
    } else {
      s_marquee_timer = app_timer_register(MARQUEE_STEP_MS, prv_marquee_tick, NULL);
    }
  }
  if (s_list_menu_layer) layer_mark_dirty(menu_layer_get_layer(s_list_menu_layer));
}

static void list_draw_header(GContext *ctx, const Layer *cell_layer,
                              uint16_t section, void *cb_ctx) {
  menu_cell_basic_header_draw(ctx, cell_layer, s_list_title);
}

static void list_draw_row(GContext *ctx, const Layer *cell_layer,
                           MenuIndex *idx, void *cb_ctx) {
  int i = idx->row;

  // Loading / empty state
  if (s_item_count == 0) {
    const char *msg = (s_loading)    ? "Loading\xe2\x80\xa6" :
                      (s_error[0])   ? s_error : "No items";
    menu_cell_basic_draw(ctx, cell_layer, msg, NULL, NULL);
    return;
  }

  // "Load More" sentinel row
  if (s_has_more && i == s_item_count) {
    menu_cell_basic_draw(ctx, cell_layer,
                         s_loading_more ? "Loading\xe2\x80\xa6" : "\xe2\x80\x95 Load More \xe2\x80\x95",
                         NULL, NULL);
    return;
  }

  if (i < 0 || i >= s_item_count) return;

  GRect bounds = layer_get_bounds(cell_layer);
  bool is_sel  = s_list_menu_layer &&
                 (int)menu_layer_get_selected_index(s_list_menu_layer).row == i;

  if (!is_sel || s_marquee_offset == 0) {
    // Non-selected or marquee not yet started — standard draw (handles all colors)
    menu_cell_basic_draw(ctx, cell_layer, s_titles[i], s_subtitles[i], NULL);
    return;
  }

  // Selected row with active marquee.
  // Do NOT call menu_cell_basic_draw with NULL subtitle — that vertically centers
  // the title, shifting it ~10px lower than the title+subtitle layout. Draw manually
  // at the same y positions menu_cell_basic_draw uses internally (title y=4, sub y=26).
  int w = bounds.size.w;
  graphics_context_set_text_color(ctx, GColorWhite);

  GRect title_rect = GRect(4, 4, w - 8, 22);
  graphics_draw_text(ctx, s_titles[i],
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     title_rect,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);

  // Subtitle shifted left by marquee offset; layer bounds clip the left edge naturally
  GRect sub_box = GRect(4 - s_marquee_offset, 26, 600, 16);
  graphics_draw_text(ctx, s_subtitles[i],
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     sub_box,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

static void list_select_click(MenuLayer *l, MenuIndex *idx, void *ctx) {
  int i = idx->row;

  // "Load More" row
  if (s_has_more && i == s_item_count) {
    if (s_loading_more) return;
    s_loading_more = true;
    s_load_offset  = s_item_count;
    char offset_str[8];
    snprintf(offset_str, sizeof(offset_str), "%d", s_item_count);
    if (s_list_menu_layer) menu_layer_reload_data(s_list_menu_layer);
    prv_send_command(s_current_cmd, offset_str);
    return;
  }

  if (s_loading || s_item_count == 0 || i < 0 || i >= s_item_count) return;

  s_selected_idx = i;

  // Pre-fill detail buffers with what we already have
  strncpy(s_det_title_buf, s_titles[i], STR_LEN - 1);
  snprintf(s_det_info_buf, sizeof(s_det_info_buf), "%s\n\n%s",
           s_subtitles[i], s_details[i]);

  // For Recently Added, also fetch rich metadata (summary, cast, studio)
  if (s_current_cmd == CMD_RECENT && s_rating_keys[i][0]) {
    s_detail_pending_meta = true;
    snprintf(s_det_info_buf, sizeof(s_det_info_buf), "%s\n\n%s\n\nLoading details\xe2\x80\xa6",
             s_subtitles[i], s_details[i]);
    prv_send_command(CMD_METADATA, s_rating_keys[i]);
  }

  window_stack_push(s_detail_window, true);
}

static void list_selection_changed(MenuLayer *l, MenuIndex new_idx,
                                    MenuIndex old_idx, void *ctx) {
  prv_start_marquee();
}

static void list_window_appear(Window *window)    { prv_start_marquee(); }
static void list_window_disappear(Window *window) { prv_stop_marquee(); }

static void list_window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  s_list_menu_layer = menu_layer_create(bounds);
  menu_layer_set_click_config_onto_window(s_list_menu_layer, window);
  menu_layer_set_callbacks(s_list_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = list_num_sections,
    .get_num_rows      = list_num_rows,
    .get_cell_height   = list_row_height,
    .get_header_height = list_header_height,
    .draw_header       = list_draw_header,
    .draw_row          = list_draw_row,
    .select_click      = list_select_click,
    .selection_changed = list_selection_changed,
  });
  layer_add_child(root, menu_layer_get_layer(s_list_menu_layer));
}

static void list_window_unload(Window *window) {
  prv_stop_marquee();
  menu_layer_destroy(s_list_menu_layer);
  s_list_menu_layer = NULL;
}

// ─── Main Menu ───────────────────────────────────────────────
static const char *MAIN_LABELS[] = {"Now Playing", "History", "Recently Added"};
static const int   MAIN_CMDS[]   = {CMD_ACTIVITY,  CMD_HISTORY, CMD_RECENT};

static uint16_t main_num_sections(MenuLayer *l, void *ctx)            { return 1; }
static uint16_t main_num_rows(MenuLayer *l, uint16_t s, void *ctx)    { return 3; }
static int16_t  main_row_height(MenuLayer *l, MenuIndex *idx, void *ctx) { return 44; }
static int16_t  main_header_height(MenuLayer *l, uint16_t s, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void main_draw_header(GContext *ctx, const Layer *cell_layer,
                              uint16_t section, void *cb_ctx) {
  menu_cell_basic_header_draw(ctx, cell_layer, "Tautulli");
}

static void main_draw_row(GContext *ctx, const Layer *cell_layer,
                           MenuIndex *idx, void *cb_ctx) {
  const char *subtitle = NULL;
  if (idx->row == 0 && !s_settings.api_key[0]) {
    subtitle = "Open phone settings first";
  }
  menu_cell_basic_draw(ctx, cell_layer, MAIN_LABELS[idx->row], subtitle, s_icons[idx->row]);
}

static void main_select_click(MenuLayer *l, MenuIndex *idx, void *ctx) {
  int row = idx->row;
  if (row >= 0 && row < 3) prv_show_list(MAIN_CMDS[row], MAIN_LABELS[row]);
}

static void main_window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  s_icons[0] = gbitmap_create_with_resource(RESOURCE_ID_ICON_NOW_PLAYING);
  s_icons[1] = gbitmap_create_with_resource(RESOURCE_ID_ICON_HISTORY);
  s_icons[2] = gbitmap_create_with_resource(RESOURCE_ID_ICON_RECENTLY_ADDED);

  s_main_menu_layer = menu_layer_create(bounds);
  menu_layer_set_click_config_onto_window(s_main_menu_layer, window);
  menu_layer_set_callbacks(s_main_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = main_num_sections,
    .get_num_rows      = main_num_rows,
    .get_cell_height   = main_row_height,
    .get_header_height = main_header_height,
    .draw_header       = main_draw_header,
    .draw_row          = main_draw_row,
    .select_click      = main_select_click,
  });
  layer_add_child(root, menu_layer_get_layer(s_main_menu_layer));
}

static void main_window_unload(Window *window) {
  menu_layer_destroy(s_main_menu_layer);
  s_main_menu_layer = NULL;
  for (int i = 0; i < 3; i++) {
    if (s_icons[i]) { gbitmap_destroy(s_icons[i]); s_icons[i] = NULL; }
  }
}

// ─── List Navigation ─────────────────────────────────────────
static void prv_show_list(int cmd, const char *title) {
  s_current_cmd    = cmd;
  s_loading        = true;
  s_loading_more   = false;
  s_has_more       = false;
  s_item_count     = 0;
  s_expected_count = 0;
  s_load_offset    = 0;
  s_error[0]       = '\0';
  strncpy(s_list_title, title, sizeof(s_list_title) - 1);

  if (s_list_menu_layer) menu_layer_reload_data(s_list_menu_layer);
  window_stack_push(s_list_window, true);
  prv_send_command(cmd, NULL);
}

// ─── Auto-refresh ────────────────────────────────────────────
static void prv_refresh_tick(void *ctx) {
  s_refresh_timer = NULL;
  if (s_current_cmd == CMD_ACTIVITY &&
      window_stack_get_top_window() == s_list_window &&
      !s_loading_more) {
    s_loading        = true;
    s_loading_more   = false;
    s_has_more       = false;
    s_item_count     = 0;
    s_load_offset    = 0;
    s_error[0]       = '\0';
    if (s_list_menu_layer) menu_layer_reload_data(s_list_menu_layer);
    prv_send_command(CMD_ACTIVITY, NULL);
  }
  s_refresh_timer = app_timer_register(REFRESH_MS, prv_refresh_tick, NULL);
}

// ─── Init / Deinit ───────────────────────────────────────────
static void init(void) {
  prv_load_settings();

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_inbox_dropped(prv_inbox_dropped);
  app_message_register_outbox_failed(prv_outbox_failed);
  app_message_open(2048, 128);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load   = main_window_load,
    .unload = main_window_unload,
  });

  s_list_window = window_create();
  window_set_window_handlers(s_list_window, (WindowHandlers){
    .load      = list_window_load,
    .appear    = list_window_appear,
    .disappear = list_window_disappear,
    .unload    = list_window_unload,
  });

  s_detail_window = window_create();
  window_set_window_handlers(s_detail_window, (WindowHandlers){
    .load   = detail_window_load,
    .unload = detail_window_unload,
  });

  window_stack_push(s_main_window, true);

  s_refresh_timer = app_timer_register(REFRESH_MS, prv_refresh_tick, NULL);
}

static void deinit(void) {
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
    s_refresh_timer = NULL;
  }
  window_destroy(s_main_window);
  window_destroy(s_list_window);
  window_destroy(s_detail_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
