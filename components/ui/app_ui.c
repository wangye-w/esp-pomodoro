#include "app_ui.h"

#include <sys/lock.h>
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

static const char *TAG = "app_ui";

struct pomodoro_ctx {
    lv_obj_t *screen;
    lv_obj_t *label_title;
    lv_obj_t *label_timer;
    lv_obj_t *bar_progress;
    lv_timer_t *tick_timer;
    int total_seconds;
    int remaining_seconds;
    bool running;
    int last_displayed_min;
};

_lock_t lvgl_api_lock;     // LVGL 线程安全互斥锁

/* ---------- Screen creation ---------- */
pomodoro_ctx_t *pomodoro_screen_create(void)
{
    pomodoro_ctx_t *ctx = calloc(1, sizeof(pomodoro_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to alloc pomodoro context");
        return NULL;
    }

    _lock_acquire(&lvgl_api_lock);
    /* Create a plain screen */
    ctx->screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ctx->screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->screen, LV_OPA_COVER, LV_PART_MAIN);

    /* Title label — top center, Montserrat 14 */
    ctx->label_title = lv_label_create(ctx->screen);
    lv_label_set_text(ctx->label_title, "READY");
    lv_obj_set_style_text_font(ctx->label_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->label_title, lv_color_black(), LV_PART_MAIN);
    lv_obj_align(ctx->label_title, LV_ALIGN_TOP_MID, 0, 20);

    /* Timer label — center, big Montserrat 24 */
    ctx->label_timer = lv_label_create(ctx->screen);
    lv_label_set_text(ctx->label_timer, "--:--");
    lv_obj_set_style_text_font(ctx->label_timer, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->label_timer, lv_color_black(), LV_PART_MAIN);
    lv_obj_align(ctx->label_timer, LV_ALIGN_CENTER, 0, -10);

    /* Progress bar — bottom, full width minus margins */
    ctx->bar_progress = lv_bar_create(ctx->screen);
    lv_obj_set_size(ctx->bar_progress, 160, 10);
    lv_obj_align(ctx->bar_progress, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_bar_set_range(ctx->bar_progress, 0, 100);
    lv_bar_set_value(ctx->bar_progress, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ctx->bar_progress, lv_color_hex(0xDDDDDD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->bar_progress, LV_OPA_COVER, LV_PART_MAIN);
    _lock_release(&lvgl_api_lock);

    ESP_LOGI(TAG, "Pomodoro screen created");
    return ctx;
}

/* ---------- Display update (triggers e-ink refresh) ---------- */
static void update_display(pomodoro_ctx_t *ctx)
{
    int mins = ctx->remaining_seconds / 60;
    int secs = ctx->remaining_seconds % 60;

    _lock_acquire(&lvgl_api_lock);
    lv_label_set_text_fmt(ctx->label_timer, "%02d:%02d", mins, secs);

    int progress = (ctx->total_seconds > 0)
        ? (ctx->remaining_seconds * 100 / ctx->total_seconds)
        : 0;
    lv_bar_set_value(ctx->bar_progress, progress, LV_ANIM_OFF);
    _lock_release(&lvgl_api_lock);

    ESP_LOGI(TAG, "Display updated: %02d:%02d (%d%%)", mins, secs, progress);
}

/* ---------- LVGL tick callback (runs inside lv_timer_handler, mutex held) ---------- */
static void tick_cb(lv_timer_t * timer)
{
    pomodoro_ctx_t *ctx = (pomodoro_ctx_t *)timer->user_data;
    if (!ctx->running) {
        return;
    }

    ctx->remaining_seconds--;

    int current_min = ctx->remaining_seconds / 60;
    bool timer_done = (ctx->remaining_seconds <= 0);

    /* Only trigger expensive e-ink refresh on minute boundary or completion */
    if (current_min != ctx->last_displayed_min || timer_done) {
        update_display(ctx);
        ctx->last_displayed_min = current_min;
    }

    if (timer_done) {
        ctx->running = false;
        _lock_acquire(&lvgl_api_lock);
        lv_label_set_text(ctx->label_title, "DONE!");
        _lock_release(&lvgl_api_lock);
        ESP_LOGI(TAG, "Pomodoro finished!");
    }
}

/* ---------- Start / restart timer ---------- */
void pomodoro_start(pomodoro_ctx_t *ctx, int total_seconds, const char *title)
{
    if (!ctx) return;

    /* Delete previous tick timer if any */
    if (ctx->tick_timer) {
        lv_timer_del(ctx->tick_timer);
        ctx->tick_timer = NULL;
    }

    ctx->total_seconds = total_seconds;
    ctx->remaining_seconds = total_seconds;
    ctx->running = (total_seconds > 0);
    ctx->last_displayed_min = total_seconds / 60;

    _lock_acquire(&lvgl_api_lock);
    lv_label_set_text(ctx->label_title, title);
    _lock_release(&lvgl_api_lock);
    update_display(ctx);

    if (total_seconds > 0) {
        ESP_LOGI(TAG, "Install LVGL tick timer");
        ctx->tick_timer = lv_timer_create(tick_cb, 1000, ctx);
    }

    ESP_LOGI(TAG, "Pomodoro started: %s, %d sec", title, total_seconds);
}

/* ---------- Stop timer ---------- */
void pomodoro_stop(pomodoro_ctx_t *ctx)
{
    if (!ctx) return;

    ctx->running = false;

    if (ctx->tick_timer) {
        lv_timer_del(ctx->tick_timer);
        ctx->tick_timer = NULL;
    }

    lv_label_set_text(ctx->label_title, "PAUSED");
    lv_label_set_text(ctx->label_timer, "--:--");
    lv_bar_set_value(ctx->bar_progress, 0, LV_ANIM_OFF);

    ESP_LOGI(TAG, "Pomodoro stopped");
}

/* ---------- Screen accessor ---------- */
lv_obj_t *pomodoro_get_screen(pomodoro_ctx_t *ctx)
{
    return ctx ? ctx->screen : NULL;
}



void ui_task(void *pvParameters) {
    ESP_LOGI(TAG, "start UItask");

    /* Create pomodoro screen once, guarded by LVGL mutex */
    pomodoro_ctx_t *pomodoro = NULL;
    ESP_LOGI(TAG, "start pomodoro");
    pomodoro = pomodoro_screen_create();
    if (pomodoro) {
        lv_scr_load(pomodoro_get_screen(pomodoro));
    }
    pomodoro_start(pomodoro, 610, "title");
    

    while (1) {
        // /* Block until IMU detects a new face orientation */
        // if (xQueueReceive(face_state_queue, &active_face, portMAX_DELAY) == pdTRUE) {
        //     /* Skip if same face (debounced by IMU task already, but double-guard) */
        //     if (active_face == last_face) continue;
        //     last_face = active_face;

        //     ESP_LOGI(TAG, "Face changed: %d", active_face);

        //     /* All LVGL API calls must hold the mutex */
        //     if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        //         if (active_face == FACE_Z_DOWN) {
        //             /* Face-down = pause / stop */
        //             pomodoro_stop(pomodoro);
        //         } else {
        //             /* Other faces = start corresponding pomodoro timer */
        //             int seconds = pomodoro_get_duration_for_face(active_face);
        //             const char *title = pomodoro_get_title_for_face(active_face);
        //             pomodoro_start(pomodoro, seconds, title);
        //         }
        //         if (pomodoro) {
        //             lv_scr_load(pomodoro_get_screen(pomodoro));
        //         }
        //         xSemaphoreGive(lvgl_mutex);
        //     }
        // }
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }
}

void gui_refresh_task()
{
    ESP_LOGI(TAG, "Starting LVGL task");
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}