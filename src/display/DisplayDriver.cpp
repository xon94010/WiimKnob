#include "DisplayDriver.h"
#include "PanelInit.h"
#include "Pins.h"

#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

namespace display {

namespace {

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

lv_disp_draw_buf_t drawBuf;
lv_disp_drv_t dispDrv;
lv_color_t *buf1 = nullptr;
lv_color_t *buf2 = nullptr;

// A tall partial buffer keeps LVGL's redraw cost low without needing a full 360x360 frame
// (which would be 253KB per buffer -- fine in PSRAM, but unnecessary for this UI).
constexpr int kBufRows = 60;

void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  lv_disp_flush_ready(drv);
}

} // namespace

void begin() {
  pinMode(PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LCD_BACKLIGHT, LOW); // keep the panel dark until it's initialised

  bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);

  // Rotation index 2 makes Arduino_ST77916::setRotation() write MADCTL=0xC0, which is this
  // panel's verified mounting correction (see PanelInit.h and Pins.h for why).
  gfx = new Arduino_ST77916(
      bus, PIN_LCD_RST, /*r=*/2, /*ips=*/true, /*w=*/360, /*h=*/360,
      0, 0, 0, 0,
      st77916_knob_init_operations, sizeof(st77916_knob_init_operations));

  gfx->begin(80000000); // 80MHz QSPI clock -- verified stable on this panel

  setBacklight(255);

  lv_init();

  size_t bufPixels = (size_t)360 * kBufRows;
  buf1 = (lv_color_t *)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t *)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, bufPixels);

  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = 360;
  dispDrv.ver_res = 360;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);
}

void tick() { lv_timer_handler(); }

void setBacklight(uint8_t brightness) {
  // Simple on/off for now -- see DisplayDriver.h if you want to add LEDC PWM dimming later.
  digitalWrite(PIN_LCD_BACKLIGHT, brightness > 0 ? HIGH : LOW);
}

} // namespace display
