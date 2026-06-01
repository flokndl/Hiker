#include <pebble.h>

#define DEBUG_SIMULATION 0  

#define CACHE_START_TIME_KEY 100
#define CACHE_ELAPSED_KEY    101
#define MAX_POINTS      1024
#define MAX_HISTORY_ITEMS 10 

typedef enum {
  VIEW_DISTANCE = 0, 
  VIEW_DURATION,     
  VIEW_TIME,         
  VIEW_SPEED,     
  VIEW_ASCENT,
  VIEW_DESCENT,
  NUM_VIEWS 
} TopBarView;

typedef struct {
  int32_t x; 
  int32_t y;
} HikePoint;

typedef struct {
  char title[32];
  char subtitle[32];
} HistoryItem;

// ---------------------------------------------------------------------------
// GLOBAL UI VARIABLES
// ---------------------------------------------------------------------------
static Window *s_start_window;
static Window *s_main_window;
static Window *s_finish_window;
static Window *s_history_window; 

static BitmapLayer *s_start_bitmap_layer;
static GBitmap *s_start_bitmap;
static TextLayer *s_start_hint_layer; 

static TextLayer *s_time_layer;
static ActionBarLayer *s_action_bar;
static Layer *s_canvas_layer;

static TextLayer *s_debug_layer;
static char s_debug_buffer[64] = "";

static GBitmap *s_switch_icon;
static GBitmap *s_play_icon;
static GBitmap *s_pause_icon;
static GBitmap *s_finish_icon;

static GBitmap *s_ptr_bitmaps[8];
static int s_current_heading_index = 0; 

static ScrollLayer *s_finish_scroll_layer;
static TextLayer *s_finish_title_layer;
static TextLayer *s_finish_labels[6];
static TextLayer *s_finish_values[6];

static MenuLayer *s_menu_layer;
static TextLayer *s_loading_layer;
static HistoryItem s_history_items[MAX_HISTORY_ITEMS];
static int s_history_count = 0;

static char s_val_time[16];
static char s_val_dist[16];
static char s_val_steps[16];
static char s_val_speed[32]; 
static char s_val_asc[16]; 
static char s_val_desc[16];

// ---------------------------------------------------------------------------
// GLOBAL STATE VARIABLES
// ---------------------------------------------------------------------------
static HikePoint s_points[MAX_POINTS];
static uint16_t s_point_count = 0;
static bool s_is_tracking = false;
static bool s_is_replaying = false; // NEW: Replay State!
static TopBarView s_current_view = VIEW_DISTANCE; 

static int32_t s_total_meters = 0;
static int32_t s_current_speed = 0; 
static int32_t s_total_ascent = 0;
static int32_t s_total_descent = 0;

static time_t s_trip_start_time = 0;
static uint32_t s_cached_elapsed_time = 0;

static void update_top_bar_display(struct tm *tick_time); // Forward declare

// ---------------------------------------------------------------------------
// COMMUNICATION & SENSORS
// ---------------------------------------------------------------------------
static void send_state_to_phone(int state_val) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_int(iter, MESSAGE_KEY_KEY_STATE, &state_val, sizeof(int), true);
    app_message_outbox_send();
  }
}

static void request_history_item(int index) {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_int(iter, MESSAGE_KEY_CMD_GET_ITEM, &index, sizeof(int), true);
    app_message_outbox_send();
  }
}

static void in_received_handler(DictionaryIterator *iter, void *context) {
  
  // --- HISTORY MENU HANDLERS ---
  Tuple *count_tuple = dict_find(iter, MESSAGE_KEY_LIST_COUNT);
  Tuple *index_tuple = dict_find(iter, MESSAGE_KEY_ITEM_INDEX);

  if (count_tuple) {
    s_history_count = count_tuple->value->int32;
    if (s_history_count > 0) {
      request_history_item(0); 
    } else {
      text_layer_set_text(s_loading_layer, "No history saved!");
    }
  }

  if (index_tuple) {
    int idx = index_tuple->value->int32;
    Tuple *title_tuple = dict_find(iter, MESSAGE_KEY_ITEM_TITLE);
    Tuple *sub_tuple = dict_find(iter, MESSAGE_KEY_ITEM_SUBTITLE);

    if (title_tuple && sub_tuple && idx < MAX_HISTORY_ITEMS) {
      strncpy(s_history_items[idx].title, title_tuple->value->cstring, sizeof(s_history_items[idx].title) - 1);
      s_history_items[idx].title[sizeof(s_history_items[idx].title) - 1] = '\0';
      
      strncpy(s_history_items[idx].subtitle, sub_tuple->value->cstring, sizeof(s_history_items[idx].subtitle) - 1);
      s_history_items[idx].subtitle[sizeof(s_history_items[idx].subtitle) - 1] = '\0';

      if (idx + 1 < s_history_count) {
        request_history_item(idx + 1); 
      } else {
        layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
        layer_set_hidden(menu_layer_get_layer(s_menu_layer), false);
        menu_layer_reload_data(s_menu_layer);
      }
    }
  }

  // Catch generic Stats (Used by both Live Tracking and Replay)
  Tuple *dist_tuple = dict_find(iter, MESSAGE_KEY_KEY_DISTANCE);
  Tuple *speed_tuple = dict_find(iter, MESSAGE_KEY_KEY_SPEED);
  Tuple *ascent_tuple = dict_find(iter, MESSAGE_KEY_KEY_ASCENT);
  Tuple *descent_tuple = dict_find(iter, MESSAGE_KEY_KEY_DESCENT);
  
  if (dist_tuple) s_total_meters = dist_tuple->value->int32;
  if (speed_tuple) s_current_speed = speed_tuple->value->int32;
  if (ascent_tuple) s_total_ascent = ascent_tuple->value->int32;
  if (descent_tuple) s_total_descent = descent_tuple->value->int32;

  // --- REPLAY HANDLERS ---
  Tuple *replay_start = dict_find(iter, MESSAGE_KEY_REPLAY_START);
  if (replay_start) {
    s_point_count = 0; // Clear the canvas!
    Tuple *time_tup = dict_find(iter, MESSAGE_KEY_REPLAY_TIME);
    if (time_tup) s_cached_elapsed_time = time_tup->value->int32;
    
    // Refresh Top Bar immediately with the final stats
    if (s_time_layer) {
      time_t now = time(NULL);
      update_top_bar_display(localtime(&now));
    }
  }

  if (s_is_replaying) {
    Tuple *rx = dict_find(iter, MESSAGE_KEY_REPLAY_PT_X);
    Tuple *ry = dict_find(iter, MESSAGE_KEY_REPLAY_PT_Y);
    if (rx && ry) {
      if (s_point_count < MAX_POINTS) {
        s_points[s_point_count].x = rx->value->int32;
        s_points[s_point_count].y = ry->value->int32;
        s_point_count++;
        if (s_canvas_layer != NULL) layer_mark_dirty(s_canvas_layer);
      }
    }
    // Bypass live logic!
    return;
  }

  // --- LIVE TRACKING HANDLERS ---
  Tuple *debug_tuple = dict_find(iter, MESSAGE_KEY_KEY_DEBUG_MSG);
  if (debug_tuple) {
    snprintf(s_debug_buffer, sizeof(s_debug_buffer), "%s", debug_tuple->value->cstring);
    if (s_debug_layer != NULL) text_layer_set_text(s_debug_layer, s_debug_buffer);
  }

  if (DEBUG_SIMULATION || !s_is_tracking) return; 

  Tuple *x_tuple = dict_find(iter, MESSAGE_KEY_KEY_NEW_POINT_X);
  Tuple *y_tuple = dict_find(iter, MESSAGE_KEY_KEY_NEW_POINT_Y);

  if (s_total_meters >= 100000) {
    if (s_is_tracking) {
      s_cached_elapsed_time += (time(NULL) - s_trip_start_time);
      s_is_tracking = false;
      persist_delete(CACHE_START_TIME_KEY);
      persist_delete(CACHE_ELAPSED_KEY);
      send_state_to_phone(2); 
      window_stack_push(s_finish_window, true);
      return; 
    }
  }

  if (x_tuple && y_tuple) {
    if (s_point_count >= MAX_POINTS) {
      memmove(&s_points[0], &s_points[1], sizeof(HikePoint) * (MAX_POINTS - 1));
      s_point_count = MAX_POINTS - 1;
    }
    s_points[s_point_count].x = x_tuple->value->int32;
    s_points[s_point_count].y = y_tuple->value->int32;
    s_point_count++;
    
    if (s_canvas_layer != NULL) layer_mark_dirty(s_canvas_layer);
  }
}

#if defined(PBL_COMPASS)
static void compass_handler(CompassHeadingData heading_data) {
  if (DEBUG_SIMULATION || heading_data.compass_status == CompassStatusDataInvalid) return;
  s_current_heading_index = ((heading_data.magnetic_heading + 4096) % TRIG_MAX_ANGLE) / 8192;
  if (window_stack_get_top_window() == s_main_window) {
    if (s_canvas_layer != NULL) layer_mark_dirty(s_canvas_layer);
  }
}
#endif

// ---------------------------------------------------------------------------
// HISTORY SCREEN (MENU)
// ---------------------------------------------------------------------------
static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  return s_history_count;
}

static void menu_draw_row_callback(GContext* ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  menu_cell_basic_draw(ctx, cell_layer, s_history_items[cell_index->row].title, s_history_items[cell_index->row].subtitle, NULL);
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  int index = cell_index->row;
  
  // Ask phone to stream this specific hike
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_int(iter, MESSAGE_KEY_CMD_GET_HIKE, &index, sizeof(int), true);
    app_message_outbox_send();
  }

  // Pre-configure the main window for Replay mode
  s_is_replaying = true;
  s_point_count = 0;
  s_total_meters = 0;
  s_cached_elapsed_time = 0;
  s_total_ascent = 0;
  s_total_descent = 0;
  s_current_speed = 0;
  
  window_stack_push(s_main_window, true);
}

static void history_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_loading_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 10, bounds.size.w, 20));
  text_layer_set_text_alignment(s_loading_layer, GTextAlignmentCenter);
  text_layer_set_text(s_loading_layer, "Loading History...");
  text_layer_set_text_color(s_loading_layer, GColorWhite);
  text_layer_set_background_color(s_loading_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_loading_layer));

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_get_num_rows_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  
  layer_set_hidden(menu_layer_get_layer(s_menu_layer), true); 
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void history_window_unload(Window *window) {
  text_layer_destroy(s_loading_layer);
  menu_layer_destroy(s_menu_layer);
}

// ---------------------------------------------------------------------------
// FINISH SCREEN (SUMMARY)
// ---------------------------------------------------------------------------
static void finish_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(false); 
  window_stack_pop(true);  
}

static void finish_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, finish_select_click_handler);
}

static void finish_window_load(Window *window) {
  window_set_background_color(window, GColorBlack); 
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_finish_scroll_layer = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_finish_scroll_layer, window);
  scroll_layer_set_callbacks(s_finish_scroll_layer, (ScrollLayerCallbacks){
    .click_config_provider = finish_click_config_provider
  });
  layer_add_child(window_layer, scroll_layer_get_layer(s_finish_scroll_layer));

  s_finish_title_layer = text_layer_create(GRect(10, 10, bounds.size.w - 20, 35));
  text_layer_set_text(s_finish_title_layer, s_is_replaying ? "Hike Summary" : "Well done!");
  text_layer_set_font(s_finish_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_color(s_finish_title_layer, GColorWhite);
  text_layer_set_background_color(s_finish_title_layer, GColorClear);
  scroll_layer_add_child(s_finish_scroll_layer, text_layer_get_layer(s_finish_title_layer));

  uint32_t final_time = s_cached_elapsed_time > 0 ? s_cached_elapsed_time : 1;
  uint32_t hrs = final_time / 3600;
  uint32_t mins = (final_time % 3600) / 60;
  uint32_t secs = final_time % 60;
  snprintf(s_val_time, sizeof(s_val_time), "%lu:%02lu:%02lu", hrs, mins, secs);

  int km = s_total_meters / 1000;
  if (km < 10) {
    int f_km = (s_total_meters % 1000) / 10;
    snprintf(s_val_dist, sizeof(s_val_dist), "%d.%02d km", km, f_km);
  } else if (km < 100) {
    int f_km = (s_total_meters % 1000) / 100;
    snprintf(s_val_dist, sizeof(s_val_dist), "%d.%d km", km, f_km);
  } else {
    snprintf(s_val_dist, sizeof(s_val_dist), "%d km", km);
  }

  int steps = 0;
  #if defined(PBL_HEALTH)
    HealthMetric metric = HealthMetricStepCount;
    time_t end = time(NULL);
    time_t start = end - final_time;
    steps = (int)health_service_sum(metric, start, end);
    if (DEBUG_SIMULATION) steps = 450; 
  #endif
  snprintf(s_val_steps, sizeof(s_val_steps), "%d", steps);

  int avg_speed_x10 = (s_total_meters * 36) / final_time;
  snprintf(s_val_speed, sizeof(s_val_speed), "%d.%d km/h", avg_speed_x10 / 10, avg_speed_x10 % 10);

  snprintf(s_val_asc, sizeof(s_val_asc), "+%dm", (int)s_total_ascent);
  snprintf(s_val_desc, sizeof(s_val_desc), "-%dm", (int)s_total_descent);

  const char* label_texts[6] = {"Time", "Distance", "Steps", "Avg Speed", "Ascent", "Descent"};
  char* value_texts[6] = {s_val_time, s_val_dist, s_val_steps, s_val_speed, s_val_asc, s_val_desc};

  int current_y = 50;
  for (int i = 0; i < 6; i++) {
    s_finish_labels[i] = text_layer_create(GRect(10, current_y, bounds.size.w - 20, 20));
    text_layer_set_text(s_finish_labels[i], label_texts[i]);
    text_layer_set_font(s_finish_labels[i], fonts_get_system_font(FONT_KEY_GOTHIC_18));
    text_layer_set_text_color(s_finish_labels[i], GColorWhite);
    text_layer_set_background_color(s_finish_labels[i], GColorClear);
    scroll_layer_add_child(s_finish_scroll_layer, text_layer_get_layer(s_finish_labels[i]));
    current_y += 20;

    s_finish_values[i] = text_layer_create(GRect(10, current_y, bounds.size.w - 20, 28));
    text_layer_set_text(s_finish_values[i], value_texts[i]);
    text_layer_set_font(s_finish_values[i], fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_color(s_finish_values[i], GColorWhite);
    text_layer_set_background_color(s_finish_values[i], GColorClear);
    scroll_layer_add_child(s_finish_scroll_layer, text_layer_get_layer(s_finish_values[i]));
    current_y += 35; 
  }

  scroll_layer_set_content_size(s_finish_scroll_layer, GSize(bounds.size.w, current_y + 20));
}

static void finish_window_unload(Window *window) {
  text_layer_destroy(s_finish_title_layer);
  for (int i = 0; i < 6; i++) {
    text_layer_destroy(s_finish_labels[i]);
    text_layer_destroy(s_finish_values[i]);
  }
  scroll_layer_destroy(s_finish_scroll_layer);
}

// ---------------------------------------------------------------------------
// MAIN SCREEN (MAP & TRACKING)
// ---------------------------------------------------------------------------
static void update_top_bar_display(struct tm *tick_time) {
  static char s_metric_buffer[32];
  
  switch(s_current_view) {
    case VIEW_DISTANCE: {
      int km = s_total_meters / 1000;
      if (km < 10) {
        int f_km = (s_total_meters % 1000) / 10;
        snprintf(s_metric_buffer, sizeof(s_metric_buffer), "%d.%02d km", km, f_km);
      } else if (km < 100) {
        int f_km = (s_total_meters % 1000) / 100;
        snprintf(s_metric_buffer, sizeof(s_metric_buffer), "%d.%d km", km, f_km);
      } else {
        snprintf(s_metric_buffer, sizeof(s_metric_buffer), "%d km", km);
      }
      break;
    }
    case VIEW_DURATION: {
      uint32_t display_time = s_cached_elapsed_time;
      if (s_is_tracking && s_trip_start_time > 0 && !s_is_replaying) {
        display_time += (time(NULL) - s_trip_start_time);
      }
      uint32_t hrs = display_time / 3600;
      uint32_t mins = (display_time % 3600) / 60;
      uint32_t secs = display_time % 60;
      snprintf(s_metric_buffer, sizeof(s_metric_buffer), "%lu:%02lu:%02lu", hrs, mins, secs);
      break;
    }
    case VIEW_TIME:
      strftime(s_metric_buffer, sizeof(s_metric_buffer), clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
      break;
    case VIEW_SPEED: {
      int sp_w = s_current_speed / 10;
      int sp_f = s_current_speed % 10;
      snprintf(s_metric_buffer, sizeof(s_metric_buffer), "%d.%d km/h", sp_w, sp_f);
      break;
    }
    case VIEW_ASCENT:
      snprintf(s_metric_buffer, sizeof(s_metric_buffer), "Ascent: +%dm", (int)s_total_ascent);
      break;
    case VIEW_DESCENT:
      snprintf(s_metric_buffer, sizeof(s_metric_buffer), "Descent: -%dm", (int)s_total_descent);
      break;
    default:
      snprintf(s_metric_buffer, sizeof(s_metric_buffer), "---");
      break;
  }
  
  if (s_time_layer != NULL) {
    text_layer_set_text(s_time_layer, s_metric_buffer);
  }

  if (s_debug_layer != NULL) {
    // Hide debug layer if we are not on distance view OR if we are replaying
    bool should_hide = (s_current_view != VIEW_DISTANCE) || s_is_replaying;
    layer_set_hidden(text_layer_get_layer(s_debug_layer), should_hide);
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, bounds);

  if (s_point_count == 0) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(bounds.size.w / 2, bounds.size.h / 2), 3);
    return;
  }

  int32_t min_x = s_points[0].x;
  int32_t max_x = s_points[0].x;
  int32_t min_y = s_points[0].y;
  int32_t max_y = s_points[0].y;

  for (uint16_t i = 1; i < s_point_count; i++) {
    if (s_points[i].x < min_x) min_x = s_points[i].x;
    if (s_points[i].x > max_x) max_x = s_points[i].x;
    if (s_points[i].y < min_y) min_y = s_points[i].y;
    if (s_points[i].y > max_y) max_y = s_points[i].y;
  }

  int32_t data_w = max_x - min_x;
  int32_t data_h = max_y - min_y;

  if (data_w < 50) {
    int32_t pad = (50 - data_w) / 2;
    min_x -= pad; max_x += pad;
    data_w = 50;
  }
  if (data_h < 50) {
    int32_t pad = (50 - data_h) / 2;
    min_y -= pad; max_y += pad;
    data_h = 50;
  }

  int32_t data_cx = min_x + (data_w / 2);
  int32_t data_cy = min_y + (data_h / 2);

  int16_t avail_w = bounds.size.w - 16; 
  int16_t avail_h = bounds.size.h - 16;
  int16_t screen_cx = bounds.size.w / 2;
  int16_t screen_cy = bounds.size.h / 2;

  int32_t zoom_x = (avail_w * 1024) / data_w;
  int32_t zoom_y = (avail_h * 1024) / data_h;
  int32_t zoom_scale = (zoom_x < zoom_y) ? zoom_x : zoom_y; 

  graphics_context_set_stroke_width(ctx, 2);
  graphics_context_set_stroke_color(ctx, GColorWhite);

  GPoint prev_pixel = GPoint(0, 0);
  GPoint curr_pixel = GPoint(0, 0);
  
  for (uint16_t i = 0; i < s_point_count; i++) {
    int16_t px = screen_cx + (((s_points[i].x - data_cx) * zoom_scale) >> 10);
    int16_t py = screen_cy - (((s_points[i].y - data_cy) * zoom_scale) >> 10); 
    curr_pixel = GPoint(px, py);
    
    if (i == 0) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_circle(ctx, curr_pixel, 3);
    } else {
      graphics_draw_line(ctx, prev_pixel, curr_pixel);
    }
    prev_pixel = curr_pixel;
  }

  GBitmap *ptr_bmp = s_ptr_bitmaps[s_current_heading_index];
  if (ptr_bmp) {
    GRect bmp_bounds = gbitmap_get_bounds(ptr_bmp);
    GRect dst_rect = GRect(curr_pixel.x - bmp_bounds.size.w / 2,
                           curr_pixel.y - bmp_bounds.size.h / 2,
                           bmp_bounds.size.w, bmp_bounds.size.h);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, ptr_bmp, dst_rect);
  }
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_current_view = (s_current_view + 1) % NUM_VIEWS;
  time_t now = time(NULL);
  update_top_bar_display(localtime(&now));
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_is_replaying) return; // Disable pausing during replay!
  
  s_is_tracking = !s_is_tracking;
  if (s_is_tracking) {
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_pause_icon);
    s_trip_start_time = time(NULL);
    persist_write_int(CACHE_START_TIME_KEY, s_trip_start_time);
    if (!DEBUG_SIMULATION) send_state_to_phone(1); 
  } else {
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_play_icon);
    s_cached_elapsed_time += (time(NULL) - s_trip_start_time);
    persist_write_int(CACHE_ELAPSED_KEY, s_cached_elapsed_time);
    if (!DEBUG_SIMULATION) send_state_to_phone(0); 
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_is_replaying) {
    window_stack_push(s_finish_window, true);
    return;
  }

  if (s_is_tracking) {
    s_cached_elapsed_time += (time(NULL) - s_trip_start_time);
  }
  s_is_tracking = false;
  
  persist_delete(CACHE_START_TIME_KEY);
  persist_delete(CACHE_ELAPSED_KEY);
  
  if (!DEBUG_SIMULATION) send_state_to_phone(2); 
  window_stack_push(s_finish_window, true);
}

static void main_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

static int s_sim_angle = 0;
static int s_sim_radius = 5;

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  if (DEBUG_SIMULATION && s_is_tracking) {
    if (s_point_count == 0 || tick_time->tm_sec % 3 == 0) {
      s_sim_angle = (s_sim_angle + 35) % 360;
      s_sim_radius += 4;
      
      int32_t sin_a = sin_lookup(s_sim_angle * TRIG_MAX_ANGLE / 360);
      int32_t cos_a = cos_lookup(s_sim_angle * TRIG_MAX_ANGLE / 360);
      
      if (s_point_count >= MAX_POINTS) {
        memmove(&s_points[0], &s_points[1], sizeof(HikePoint) * (MAX_POINTS - 1));
        s_point_count = MAX_POINTS - 1;
      }
      
      s_points[s_point_count].x = (cos_a * s_sim_radius) / TRIG_MAX_RATIO;
      s_points[s_point_count].y = (sin_a * s_sim_radius) / TRIG_MAX_RATIO;
      s_point_count++;
      
      s_total_meters += 18;
      s_current_speed = 48; 
      s_total_ascent += 1;
      if (s_point_count % 3 == 0) s_total_descent += 1;
      
      s_current_heading_index = (s_current_heading_index + 1) % 8;
      
      if (s_canvas_layer != NULL) {
        layer_mark_dirty(s_canvas_layer);
      }

      if (s_total_meters >= 100000) {
        s_cached_elapsed_time += (time(NULL) - s_trip_start_time);
        s_is_tracking = false;
        persist_delete(CACHE_START_TIME_KEY);
        persist_delete(CACHE_ELAPSED_KEY);
        window_stack_push(s_finish_window, true);
      }
    }
  }

  if (window_stack_get_top_window() == s_main_window) {
    update_top_bar_display(tick_time);
  }
}

static void main_window_load(Window *window) {
  window_set_background_color(window, GColorBlack); 
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_action_bar = action_bar_layer_create();
  action_bar_layer_add_to_window(s_action_bar, window);
  action_bar_layer_set_click_config_provider(s_action_bar, main_click_config_provider);

  s_switch_icon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_SWITCH);
  s_play_icon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PLAY);
  s_pause_icon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PAUSE);
  s_finish_icon = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_FINISH);

  s_ptr_bitmaps[0] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_N);
  s_ptr_bitmaps[1] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_NE);
  s_ptr_bitmaps[2] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_E);
  s_ptr_bitmaps[3] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_SE);
  s_ptr_bitmaps[4] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_S);
  s_ptr_bitmaps[5] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_SW);
  s_ptr_bitmaps[6] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_W);
  s_ptr_bitmaps[7] = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PTR_NW);

  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_switch_icon);
  
  // Disable the Start/Pause icon visually if we are just replaying a hike
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_is_replaying ? NULL : s_pause_icon);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_finish_icon);

  int available_width = bounds.size.w - ACTION_BAR_WIDTH;
  
  s_time_layer = text_layer_create(GRect(4, 0, available_width - 8, 20));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentLeft);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_background_color(s_time_layer, GColorClear);
  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

  s_canvas_layer = layer_create(GRect(0, 20, available_width + 2, bounds.size.h - 20));
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(window_layer, s_canvas_layer);

  s_debug_layer = text_layer_create(GRect(0, 4, available_width + 2, 20));
  text_layer_set_text_alignment(s_debug_layer, GTextAlignmentRight);
  text_layer_set_font(s_debug_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_color(s_debug_layer, GColorWhite);
  text_layer_set_background_color(s_debug_layer, GColorClear); 
  text_layer_set_text(s_debug_layer, s_debug_buffer);
  layer_add_child(window_layer, text_layer_get_layer(s_debug_layer));

  #if defined(PBL_COMPASS)
    if (!DEBUG_SIMULATION) {
      compass_service_subscribe(compass_handler);
      compass_service_set_heading_filter(TRIG_MAX_ANGLE / 36); 
    }
  #endif

  time_t now = time(NULL);
  update_top_bar_display(localtime(&now));
}

static void main_window_unload(Window *window) {
  #if defined(PBL_COMPASS)
    if (!DEBUG_SIMULATION) compass_service_unsubscribe();
  #endif

  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_debug_layer); 
  action_bar_layer_destroy(s_action_bar);
  layer_destroy(s_canvas_layer);
  
  s_time_layer = NULL;
  s_debug_layer = NULL;
  s_canvas_layer = NULL;

  gbitmap_destroy(s_switch_icon);
  gbitmap_destroy(s_play_icon);
  gbitmap_destroy(s_pause_icon);
  gbitmap_destroy(s_finish_icon);
  
  for(int i = 0; i < 8; i++) {
    gbitmap_destroy(s_ptr_bitmaps[i]);
  }
}

// ---------------------------------------------------------------------------
// START SCREEN (HOME)
// ---------------------------------------------------------------------------
static void start_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_is_replaying = false; // Reset replay state
  s_point_count = 0;
  s_total_meters = 0;
  s_current_speed = 0;
  s_total_ascent = 0;
  s_total_descent = 0;
  s_sim_angle = 0;
  s_sim_radius = 5;
  
  s_current_view = VIEW_DISTANCE;
  s_is_tracking = true;

  s_trip_start_time = time(NULL);
  s_cached_elapsed_time = 0;
  persist_write_int(CACHE_START_TIME_KEY, s_trip_start_time);
  persist_write_int(CACHE_ELAPSED_KEY, s_cached_elapsed_time);

  if (!DEBUG_SIMULATION) send_state_to_phone(1);
  window_stack_push(s_main_window, true);
}

static void start_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  window_stack_push(s_history_window, true);
  
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    int dummy = 1;
    dict_write_int(iter, MESSAGE_KEY_CMD_GET_COUNT, &dummy, sizeof(int), true);
    app_message_outbox_send();
  }
}

static void start_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, start_select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, start_down_click_handler); 
}

static void start_window_load(Window *window) {
  window_set_background_color(window, GColorBlack); 
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  window_set_click_config_provider(window, start_click_config_provider);

  s_start_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_STARTSCREEN);
  s_start_bitmap_layer = bitmap_layer_create(bounds);
  bitmap_layer_set_bitmap(s_start_bitmap_layer, s_start_bitmap);
  bitmap_layer_set_alignment(s_start_bitmap_layer, GAlignCenter);
  layer_add_child(window_layer, bitmap_layer_get_layer(s_start_bitmap_layer));

  s_start_hint_layer = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w - 5, 20));
  text_layer_set_text(s_start_hint_layer, "History ->");
  text_layer_set_text_alignment(s_start_hint_layer, GTextAlignmentRight);
  text_layer_set_background_color(s_start_hint_layer, GColorClear);
  text_layer_set_text_color(s_start_hint_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_start_hint_layer));
}

static void start_window_unload(Window *window) {
  text_layer_destroy(s_start_hint_layer);
  bitmap_layer_destroy(s_start_bitmap_layer);
  gbitmap_destroy(s_start_bitmap);
}

// ---------------------------------------------------------------------------
// CORE APP LIFECYCLE
// ---------------------------------------------------------------------------
static void init() {
  if (persist_exists(CACHE_START_TIME_KEY)) {
    s_trip_start_time = persist_read_int(CACHE_START_TIME_KEY);
  }
  if (persist_exists(CACHE_ELAPSED_KEY)) {
    s_cached_elapsed_time = persist_read_int(CACHE_ELAPSED_KEY);
  }

  s_start_window = window_create();
  window_set_window_handlers(s_start_window, (WindowHandlers) {
    .load = start_window_load,
    .unload = start_window_unload
  });

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  s_finish_window = window_create();
  window_set_window_handlers(s_finish_window, (WindowHandlers) {
    .load = finish_window_load,
    .unload = finish_window_unload
  });

  s_history_window = window_create();
  window_set_window_handlers(s_history_window, (WindowHandlers) {
    .load = history_window_load,
    .unload = history_window_unload
  });

  window_stack_push(s_start_window, true);
  
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  
  app_message_register_inbox_received(in_received_handler);
  app_message_open(512, 128); 
}

static void deinit() {
  window_destroy(s_start_window);
  window_destroy(s_main_window);
  window_destroy(s_finish_window);
  window_destroy(s_history_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}