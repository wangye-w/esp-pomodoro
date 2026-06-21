#include "app_ui.h"
#include "lcd_screen.h"

#include <sys/lock.h>
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

static const char *TAG = "app_ui";

_lock_t lvgl_api_lock;     // LVGL 线程安全互斥锁

/* 进度弧 */
#define ARC_CX          100
#define ARC_CY          98
#define ARC_R           74      /* 外弧半径 */
#define ARC_R_INNER     64      /* 45min 内虚线环 */
#define ARC_W_15        4
#define ARC_W_25        5
#define ARC_W_45        6
#define ARC_REDRAW_DEG  6       /* 角度变化阈值，减少墨水屏刷新 */

/* 颜色（墨水屏纯黑白） */
#define C_INK           lv_color_black()
#define C_PAPER         lv_color_white()
#define C_GRAY_DARK     lv_color_make(0x44, 0x44, 0x44)
#define C_GRAY_MID      lv_color_make(0x88, 0x88, 0x88)
#define C_GRAY_LIGHT    lv_color_make(0xCC, 0xCC, 0xCC)
#define C_GRAY_RULE     lv_color_make(0xDD, 0xDD, 0xD5)

/* 时长 */
#define DUR_15  (15 * 60)
#define DUR_25  (25 * 60)
#define DUR_45  (45 * 60)
#define DUR_BRK ( 5 * 60)
#define MAX_DOTS 4

/* ═══════════════════════════════════════════════════════════
 *  枚举 & 结构体
 * ═══════════════════════════════════════════════════════════ */
typedef enum {
    FACE_CLOCK = 0,
    FACE_15,
    FACE_25,
    FACE_45,
    FACE_COUNT
} FaceId;

typedef enum {
    POMO_IDLE = 0,
    POMO_RUNNING,
    POMO_PAUSED,
    POMO_BREAK,
    POMO_DONE
} PomoState;

typedef struct {
    uint8_t  hour, min, sec;
    uint8_t  wday;          /* 0=Mon..6=Sun */
    uint8_t  mday;
    uint8_t  month;         /* 1-12 */
    uint16_t year;
    uint8_t  batt_pct;      /* 0-100 */
} ClockData;

/* ═══════════════════════════════════════════════════════════
 *  UI 句柄
 * ═══════════════════════════════════════════════════════════ */
static struct {
    lv_obj_t *scr;

    /* ── 时钟面 ── */
    struct {
        lv_obj_t *root;
        lv_obj_t *lbl_time;      /* HH:MM */
        lv_obj_t *lbl_date;      /* JUNE 09 */
        lv_obj_t *lbl_year;      /* TUE · 2026 */
        lv_obj_t *wday_labels[7];/* M T W T F S S */
        lv_obj_t *wday_bar;      /* 当天下划线 */
        lv_obj_t *lbl_batt;      /* 68% */
        lv_obj_t *lbl_steps;     /* 6,240 steps */
        lv_obj_t *lbl_pomo_stat; /* ●●●○ 3 POMODOROS */
    } clk;

    /* ── 番茄面（三个面共用同一套控件，切面时更新内容）── */
    struct {
        lv_obj_t *root;
        lv_obj_t *arc_bg;        /* 灰色轨道弧 */
        lv_obj_t *arc_fg;        /* 黑色进度弧 */
        lv_obj_t *arc_inner;     /* 45min 专用内虚线环 */
        lv_obj_t *lbl_mode;      /* SPRINT / FOCUS / DEEP WORK */
        lv_obj_t *lbl_time;      /* MM:SS */
        lv_obj_t *lbl_total;     /* / MM:SS */
        lv_obj_t *dots[MAX_DOTS];
        lv_obj_t *btn_hint;      /* PAUSE / RESUME */
    } pomo;

} ui;

/* ═══════════════════════════════════════════════════════════
 *  运行时状态
 * ═══════════════════════════════════════════════════════════ */
static struct {
    FaceId    face;
    PomoState state;
    uint32_t  dur_sec;
    uint32_t  rem_sec;
    uint8_t   tomatoes;     /* 今日完成番茄数 */
    int16_t   last_deg;     /* 上次弧角，用于阈值过滤 */
    lv_timer_t *tick;
    ClockData  clock;
} rt;

static void hide(lv_obj_t *o) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void show_obj(lv_obj_t *o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); }

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                           lv_color_t color, int letter_space)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    if (letter_space > 0)
        lv_obj_set_style_text_letter_space(l, letter_space, 0);
    return l;
}

static void ui_build(void)
{
    _lock_acquire(&lvgl_api_lock);
    /* ── 根屏幕 ── */
    ui.scr = lv_obj_create(NULL);
    lv_obj_set_size(ui.scr, LCD_V_RES, LCD_H_RES);
    lv_obj_set_style_bg_color(ui.scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ui.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui.scr, 0, 0);
    lv_obj_set_style_border_width(ui.scr, 0, 0);
    lv_scr_load(ui.scr);

    /* ══════════════════════════════════════════
     *  时钟面容器
     * ══════════════════════════════════════════ */
    ui.clk.root = lv_obj_create(ui.scr);
    lv_obj_set_size(ui.clk.root, LCD_V_RES, LCD_H_RES);
    lv_obj_set_pos(ui.clk.root, 0, 0);
    lv_obj_set_style_bg_opa(ui.clk.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.clk.root, 0, 0);
    lv_obj_set_style_pad_all(ui.clk.root, 0, 0);

    /* 顶部细线 (y=28) → 用 lv_line 或直接写在 canvas，这里用宽为屏的 obj */
    {
        lv_obj_t *rule = lv_obj_create(ui.clk.root);
        lv_obj_set_size(rule, 160, 1);
        lv_obj_set_style_bg_color(rule, lv_color_make(0xCC, 0xCC, 0xCC), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_pad_all(rule, 0, 0);
        lv_obj_set_pos(rule, 20, 28);
    }

    /* 年份/星期副标题 */
    ui.clk.lbl_year = mk_label(ui.clk.root, &lv_font_montserrat_10, lv_color_make(0x88, 0x88, 0x88), 3);
    lv_label_set_text(ui.clk.lbl_year, "TUE . 2026");
    lv_obj_align(ui.clk.lbl_year, LV_ALIGN_TOP_MID, 0, 14);

    /* 大时间 */
    ui.clk.lbl_time = mk_label(ui.clk.root, &lv_font_montserrat_48, lv_color_black(), 0);
    lv_label_set_text(ui.clk.lbl_time, "09:41");
    lv_obj_set_pos(ui.clk.lbl_time, 0, 0);
    lv_obj_align(ui.clk.lbl_time, LV_ALIGN_TOP_MID, -2, 38);

    /* 中间分割线 */
    {
        lv_obj_t *rule = lv_obj_create(ui.clk.root);
        lv_obj_set_size(rule, 160, 1);
        lv_obj_set_style_bg_color(rule, lv_color_make(0xDD, 0xDD, 0xD5), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_pad_all(rule, 0, 0);
        lv_obj_set_pos(rule, 20, 100);
    }

    /* 日期 */
    ui.clk.lbl_date = mk_label(ui.clk.root, &lv_font_montserrat_14, lv_color_make(0x44, 0x44, 0x44), 1);
    lv_label_set_text(ui.clk.lbl_date, "JUNE  09");
    lv_obj_align(ui.clk.lbl_date, LV_ALIGN_TOP_MID, 0, 108);

    /* 星期行 */
    static const char *WDAY_STR[] = {"M","T","W","T","F","S","S"};
    static const int   WDAY_X[]   = {18, 38, 58, 78, 98, 118, 138};
    for (int i = 0; i < 7; i++) {
        ui.clk.wday_labels[i] = mk_label(ui.clk.root, &lv_font_montserrat_10,
                                          lv_color_make(0xCC, 0xCC, 0xCC), 0);
        lv_label_set_text(ui.clk.wday_labels[i], WDAY_STR[i]);
        lv_obj_set_pos(ui.clk.wday_labels[i], WDAY_X[i] - 4, 138);
    }
    /* 当天下划线条 */
    ui.clk.wday_bar = lv_obj_create(ui.clk.root);
    lv_obj_set_size(ui.clk.wday_bar, 16, 2);
    lv_obj_set_style_bg_color(ui.clk.wday_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui.clk.wday_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.clk.wday_bar, 0, 0);
    lv_obj_set_style_pad_all(ui.clk.wday_bar, 0, 0);

    /* 电量/步数分割线 */
    {
        lv_obj_t *rule = lv_obj_create(ui.clk.root);
        lv_obj_set_size(rule, 160, 1);
        lv_obj_set_style_bg_color(rule, lv_color_make(0xDD, 0xDD, 0xD5), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_pad_all(rule, 0, 0);
        lv_obj_set_pos(rule, 20, 162);
    }

    /* 电量标签 */
    ui.clk.lbl_batt = mk_label(ui.clk.root, &lv_font_montserrat_10, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_label_set_text(ui.clk.lbl_batt, "68%");
    lv_obj_set_pos(ui.clk.lbl_batt, 40, 168);

    /* 步数 */
    ui.clk.lbl_steps = mk_label(ui.clk.root, &lv_font_montserrat_10, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_label_set_text(ui.clk.lbl_steps, "6,240 steps");
    lv_obj_align(ui.clk.lbl_steps, LV_ALIGN_TOP_RIGHT, -20, 168);

    /* 底部番茄统计分割线 */
    {
        lv_obj_t *rule = lv_obj_create(ui.clk.root);
        lv_obj_set_size(rule, 160, 1);
        lv_obj_set_style_bg_color(rule, lv_color_make(0xDD, 0xDD, 0xD5), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_pad_all(rule, 0, 0);
        lv_obj_set_pos(rule, 20, 185);
    }

    /* 今日番茄统计 */
    ui.clk.lbl_pomo_stat = mk_label(ui.clk.root, &lv_font_montserrat_10, lv_color_make(0xCC, 0xCC, 0xCC), 1);
    lv_label_set_text(ui.clk.lbl_pomo_stat, "TODAY  3 POMODOROS");
    lv_obj_align(ui.clk.lbl_pomo_stat, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* ══════════════════════════════════════════
     *  番茄面容器（三个面共用）
     * ══════════════════════════════════════════ */
    ui.pomo.root = lv_obj_create(ui.scr);
    lv_obj_set_size(ui.pomo.root, LCD_V_RES, LCD_H_RES);
    lv_obj_set_pos(ui.pomo.root, 0, 0);
    lv_obj_set_style_bg_opa(ui.pomo.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.pomo.root, 0, 0);
    lv_obj_set_style_pad_all(ui.pomo.root, 0, 0);

    /* 背景弧 */
    ui.pomo.arc_bg = lv_arc_create(ui.pomo.root);
    lv_obj_set_size(ui.pomo.arc_bg, ARC_R * 2, ARC_R * 2);
    lv_obj_set_pos(ui.pomo.arc_bg, ARC_CX - ARC_R, ARC_CY - ARC_R);
    lv_arc_set_rotation(ui.pomo.arc_bg, 270);
    lv_arc_set_range(ui.pomo.arc_bg, 0, 360);
    lv_arc_set_value(ui.pomo.arc_bg, 360);
    lv_arc_set_bg_angles(ui.pomo.arc_bg, 0, 360);
    lv_obj_remove_style(ui.pomo.arc_bg, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(ui.pomo.arc_bg, C_GRAY_LIGHT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.pomo.arc_bg, C_GRAY_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.pomo.arc_bg, ARC_W_25, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ui.pomo.arc_bg, ARC_W_25, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.pomo.arc_bg, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ui.pomo.arc_bg, LV_OBJ_FLAG_CLICKABLE);

    /* 前景弧 */
    ui.pomo.arc_fg = lv_arc_create(ui.pomo.root);
    lv_obj_set_size(ui.pomo.arc_fg, ARC_R * 2, ARC_R * 2);
    lv_obj_set_pos(ui.pomo.arc_fg, ARC_CX - ARC_R, ARC_CY - ARC_R);
    lv_arc_set_rotation(ui.pomo.arc_fg, 270);
    lv_arc_set_range(ui.pomo.arc_fg, 0, 360);
    lv_arc_set_value(ui.pomo.arc_fg, 360);
    lv_arc_set_bg_angles(ui.pomo.arc_fg, 0, 0);
    lv_obj_remove_style(ui.pomo.arc_fg, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(ui.pomo.arc_fg, C_INK, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ui.pomo.arc_fg, ARC_W_25, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui.pomo.arc_fg, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ui.pomo.arc_fg, LV_OBJ_FLAG_CLICKABLE);

    /* 45min 内虚线环（默认隐藏） */
    /* 45min 内细线环（细实线，低透明度区分，LVGL v8 无 arc_dash API） */
    ui.pomo.arc_inner = lv_arc_create(ui.pomo.root);
    lv_obj_set_size(ui.pomo.arc_inner, ARC_R_INNER * 2, ARC_R_INNER * 2);
    lv_obj_set_pos(ui.pomo.arc_inner, ARC_CX - ARC_R_INNER, ARC_CY - ARC_R_INNER);
    lv_arc_set_bg_angles(ui.pomo.arc_inner, 0, 360);
    lv_arc_set_range(ui.pomo.arc_inner, 0, 360);
    lv_arc_set_value(ui.pomo.arc_inner, 0);
    lv_obj_remove_style(ui.pomo.arc_inner, NULL, LV_PART_KNOB);
    lv_obj_remove_style(ui.pomo.arc_inner, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.pomo.arc_inner, C_GRAY_RULE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.pomo.arc_inner, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ui.pomo.arc_inner, LV_OPA_40, LV_PART_MAIN); /* 低透明度 */
    lv_obj_set_style_bg_opa(ui.pomo.arc_inner, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ui.pomo.arc_inner, LV_OBJ_FLAG_CLICKABLE);
    // hide(ui.pomo.arc_inner);

    /* 模式标签 */
    ui.pomo.lbl_mode = mk_label(ui.pomo.root, &lv_font_montserrat_10, C_GRAY_MID, 3);
    lv_label_set_text(ui.pomo.lbl_mode, "FOCUS");
    lv_obj_align(ui.pomo.lbl_mode, LV_ALIGN_TOP_MID, 0, ARC_CY - ARC_R + 22);

    /* 大时间数字 */
    ui.pomo.lbl_time = mk_label(ui.pomo.root, &lv_font_montserrat_48, C_INK, 0);
    lv_label_set_text(ui.pomo.lbl_time, "25:00");
    lv_obj_align(ui.pomo.lbl_time, LV_ALIGN_TOP_MID, -2, ARC_CY - 28);

    /* 总时长副标签 */
    ui.pomo.lbl_total = mk_label(ui.pomo.root, &lv_font_montserrat_10, C_GRAY_LIGHT, 0);
    lv_label_set_text(ui.pomo.lbl_total, "/ 25:00");
    lv_obj_align(ui.pomo.lbl_total, LV_ALIGN_TOP_MID, 0, ARC_CY + 12);

    /* 番茄点 */
    int dot_y = ARC_CY + 50;
    for (int i = 0; i < MAX_DOTS; i++) {
        ui.pomo.dots[i] = lv_obj_create(ui.pomo.root);
        lv_obj_set_size(ui.pomo.dots[i], 9, 9);
        lv_obj_set_style_radius(ui.pomo.dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(ui.pomo.dots[i], C_GRAY_DARK, 0);
        lv_obj_set_style_border_width(ui.pomo.dots[i], 1, 0);
        lv_obj_set_style_bg_color(ui.pomo.dots[i], C_PAPER, 0);
        lv_obj_set_style_bg_opa(ui.pomo.dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(ui.pomo.dots[i], 0, 0);
        /* 4 点居中 x = 82, 94, 106, 118 */
        lv_obj_set_pos(ui.pomo.dots[i], 78 + i * 12, dot_y);
    }

    /* PAUSE 按钮提示 */
    ui.pomo.btn_hint = mk_label(ui.pomo.root, &lv_font_montserrat_10, C_GRAY_DARK, 2);
    lv_label_set_text(ui.pomo.btn_hint, "PAUSE");
    lv_obj_set_style_border_color(ui.pomo.btn_hint, C_GRAY_LIGHT, 0);
    lv_obj_set_style_border_width(ui.pomo.btn_hint, 1, 0);
    lv_obj_set_style_pad_hor(ui.pomo.btn_hint, 6, 0);
    lv_obj_set_style_pad_ver(ui.pomo.btn_hint, 3, 0);
    lv_obj_set_style_radius(ui.pomo.btn_hint, 3, 0);
    lv_obj_set_style_bg_opa(ui.pomo.btn_hint, LV_OPA_TRANSP, 0);
    lv_obj_align(ui.pomo.btn_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    _lock_release(&lvgl_api_lock);
}

/* ═══════════════════════════════════════════════════════════
 *  时钟面更新
 * ═══════════════════════════════════════════════════════════ */
static void clock_refresh(void)
{
    const ClockData *c = &rt.clock;

    /* 时间 */
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", c->hour, c->min);
    lv_label_set_text(ui.clk.lbl_time, tbuf);

    /* 年份/星期 */
    static const char *WDAY_ABBR[] = {"MON","TUE","WED","THU","FRI","SAT","SUN"};
    char ybuf[20];
    snprintf(ybuf, sizeof(ybuf), "%s . %d", WDAY_ABBR[c->wday % 7], c->year);
    lv_label_set_text(ui.clk.lbl_year, ybuf);

    /* 日期 */
    static const char *MON_STR[] = {
        "","JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };
    char dbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%s  %02d", MON_STR[c->month % 13], c->mday);
    lv_label_set_text(ui.clk.lbl_date, dbuf);

    /* 星期高亮 & 下划线 */
    static const lv_color_t C_WD_HI  = C_INK;
    static const lv_color_t C_WD_DIM = C_GRAY_LIGHT;
    static const int WDAY_X[] = {18, 38, 58, 78, 98, 118, 138};
    uint8_t today = c->wday % 7;
    for (int i = 0; i < 7; i++) {
        lv_obj_set_style_text_color(ui.clk.wday_labels[i],
                                    i == today ? C_WD_HI : C_WD_DIM, 0);
        lv_obj_set_style_text_font(ui.clk.wday_labels[i],
                                   i == today ? &lv_font_montserrat_12
                                              : &lv_font_montserrat_10, 0);
    }
    lv_obj_set_pos(ui.clk.wday_bar, WDAY_X[today] - 8, 153);

    /* 电量 */
    char bbuf[8];
    snprintf(bbuf, sizeof(bbuf), "%d%%", c->batt_pct);
    lv_label_set_text(ui.clk.lbl_batt, bbuf);

    /* 番茄统计 */
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "TODAY  %d POMODORO%s",
             rt.tomatoes, rt.tomatoes != 1 ? "S" : "");
    lv_label_set_text(ui.clk.lbl_pomo_stat, pbuf);
}

/* ═══════════════════════════════════════════════════════════
 *  面切换
 * ═══════════════════════════════════════════════════════════ */
static void face_show(FaceId f)
{
    rt.face = f;

    if (f == FACE_CLOCK) {
        show_obj(ui.clk.root);
        hide(ui.pomo.root);
        clock_refresh();
        return;
    }

    hide(ui.clk.root);
    show_obj(ui.pomo.root);

    /* 根据面调整弧宽、模式文字、是否显示内环 */
    uint16_t arc_w;
    const char *mode_str;
    const char *total_str;

    if (f == FACE_15) {
        arc_w     = ARC_W_15;
        mode_str  = "SPRINT";
        total_str = "/ 15:00";
        hide(ui.pomo.arc_inner);
    } else if (f == FACE_25) {
        arc_w     = ARC_W_25;
        mode_str  = "FOCUS";
        total_str = "/ 25:00";
        hide(ui.pomo.arc_inner);
    } else {   /* FACE_45 */
        arc_w     = ARC_W_45;
        mode_str  = "DEEP WORK";
        total_str = "/ 45:00";
        show_obj(ui.pomo.arc_inner);
    }

    /* 背景弧宽同步 */
    lv_obj_set_style_arc_width(ui.pomo.arc_bg, arc_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ui.pomo.arc_bg, arc_w, LV_PART_MAIN);
    /* 前景弧宽 */
    lv_obj_set_style_arc_width(ui.pomo.arc_fg, arc_w, LV_PART_INDICATOR);

    lv_label_set_text(ui.pomo.lbl_mode,  mode_str);
    lv_label_set_text(ui.pomo.lbl_total, total_str);
}

/** 初始化，在 lv_init() 和 display driver 注册后调用一次 */
void pomodoro_4face_init(void)
{
    memset(&rt, 0, sizeof(rt));
    rt.face      = FACE_CLOCK;
    rt.state     = POMO_IDLE;
    rt.last_deg  = -1;

    /* 默认时间（由 RTC 更新） */
    rt.clock.hour  = 9;
    rt.clock.min   = 41;
    rt.clock.wday  = 1;  /* Tuesday */
    rt.clock.mday  = 9;
    rt.clock.month = 6;
    rt.clock.year  = 2026;
    rt.clock.batt_pct = 68;

    ui_build();
    face_show(FACE_CLOCK);
}

void ui_task(void *pvParameters) {
    ESP_LOGI(TAG, "start UItask");
    pomodoro_4face_init();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }
}