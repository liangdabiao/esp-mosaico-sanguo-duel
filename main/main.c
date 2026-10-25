// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "button_gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "game.h"
#include "game_config.h"
#include "iot_button.h"
#include "mosaico_game_app.h"

#include "bsp/esp_mosaico.h" /* BSP_BUTTON_BOOT_GPIO */

/* Owned by app_main for the application lifetime. Engine callbacks execute on
 * that same task; touch samples reach it through the engine event queue. */
static game_handle_t s_game;

/* ---- BOOT 键 = 返回上一层 ---------------------------------------------
 * BSP 只导出 AI 键（bsp_iot_button_create 的按钮表里只有 BSP_BUTTON_AI），
 * BOOT 键得自己建。按键回调跑在 button 组件自己的任务上；直接改游戏状态会和
 * 渲染/更新抢同一份 state，所以回调只往引擎事件队列投一条 BUTTON 事件，
 * 真正的状态变更统一在 on_event（游戏任务）里做 —— 与触摸输入同一条路径。 */
static void on_boot_button(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    const mosaico_device_event_t event = {
        .type = MOSAICO_DEVICE_EVENT_BUTTON,
        .value = 0,
        .pressed = true,
    };
    (void)MosaicoGamePostDeviceEvent(&event);
}

static void create_boot_button(void)
{
    const button_config_t button_config = { 0 };
    const button_gpio_config_t gpio_config = {
        .gpio_num = BSP_BUTTON_BOOT_GPIO,
        .active_level = 0, /* BOOT 键低电平有效，与 BSP_BUTTON_ACTIVE_LEVEL 一致 */
        .enable_power_save = false,
        .disable_pull = false, /* 用片内上拉 */
    };
    button_handle_t boot = NULL;
    esp_err_t err = iot_button_new_gpio_device(&button_config, &gpio_config, &boot);
    if (err != ESP_OK) {
        /* 按键建不出来只是少一个返回键，不该拦住整个应用。 */
        ESP_LOGW(GAME_NAME, "boot button create failed: %s; back key disabled",
                 esp_err_to_name(err));
        return;
    }
    err = iot_button_register_cb(boot, BUTTON_SINGLE_CLICK, NULL, on_boot_button, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(GAME_NAME, "boot button callback register failed: %s",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(GAME_NAME, "BOOT key registered as BACK; gpio=%d", (int)BSP_BUTTON_BOOT_GPIO);
}

static void on_event(const mosaico_device_event_t *event)
{
    if (!event) return;
    if (event->type == MOSAICO_DEVICE_EVENT_POINTER) {
        game_set_pointer(s_game, event->x, event->y, event->pressed);
    } else if (event->type == MOSAICO_DEVICE_EVENT_BUTTON && event->pressed) {
        game_request_back(s_game);
    }
}

static void on_update(void)
{
    game_update(s_game);
}

static void on_render(void)
{
    (void)game_render(s_game);
}

static esp_err_t on_start(void)
{
    /* 引擎事件队列此时已就绪（MosaicoGameInit 在 on_start 之前完成）。 */
    create_boot_button();
    return ESP_OK;
}

static const mosaico_game_app_config_t s_config = {
    .tag = GAME_NAME,
    .window_title = GAME_NAME,
    .canvas_bind = GSP_GAME_BIND_GAME_CANVAS,
    .touch_points = 1,
    .target_fps = GAME_TICK_HZ,
    .gsp_bundle = gsp_bundle_config,
    .on_event = on_event,
    .on_start = on_start,
    .on_update = on_update,
    .on_render = on_render,
};

void app_main(void)
{
    const game_config_t config = {GAME_WIDTH, GAME_HEIGHT};
    ESP_ERROR_CHECK(game_create(&config, &s_game) ? ESP_OK : ESP_ERR_NO_MEM);
    /* The engine starts iris_ota_support_start() and marks the image healthy
     * after presenting its first frame; the OTA writer remains in Recovery. */
    esp_err_t result = mosaico_game_app_run(&s_config);
    game_delete(s_game);
    s_game = NULL;
    ESP_ERROR_CHECK(result);
}
