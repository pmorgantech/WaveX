#include "ui_lgfx/lgfx_device.h"
#include "hardware_pins.h"
#include "esp_log.h"

#ifdef CONFIG_UI_LGFX_ENABLE_INIT
#include <LovyanGFX.hpp>
#define WAVEX_HAVE_LGFX 1
#endif

#ifdef WAVEX_HAVE_LGFX
class LGFX_DeviceFinal : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ST7796 _panel;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

public:
  LGFX_DeviceFinal() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = WAVEX_SPI_HOST;      // SPI3_HOST
      cfg.spi_mode = 0;
      // Increase SPI write clock; test at 80MHz (reduce if artifacts appear)
      cfg.freq_write = 80000000; // 80MHz
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = WAVEX_LCD_GPIO_SCLK;
      cfg.pin_mosi = WAVEX_LCD_GPIO_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc   = WAVEX_LCD_GPIO_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
      ESP_LOGI("LGFX_Device", "Display SPI config: host=%d, sclk=%d, mosi=%d, dc=%d", cfg.spi_host, cfg.pin_sclk, cfg.pin_mosi, cfg.pin_dc);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs  = WAVEX_LCD_GPIO_CS;
      cfg.pin_rst = WAVEX_LCD_GPIO_RST;
      cfg.pin_busy = -1;
      // ST7796 native panel is 320x480 (portrait). Use native dims and rotate later.
      cfg.panel_width  = 320;
      cfg.panel_height = 480;
      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.readable = false;
      cfg.invert   = false;
      // If colors appear swapped (blue -> red), set this to false
      cfg.rgb_order = false; // BGR=false, RGB=true depending on panel; try false first
      _panel.config(cfg);
      ESP_LOGI("LGFX_Device", "Display Panel config: cs=%d, rst=%d", cfg.pin_cs, cfg.pin_rst);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = WAVEX_LCD_GPIO_BL;
      cfg.invert = (WAVEX_LCD_BL_ON_LEVEL == 0);
      cfg.freq   = 5000;
      cfg.pwm_channel = 0;
      _light.config(cfg);
      _panel.setLight(&_light);
      ESP_LOGI("LGFX_Device", "Display Light config: bl=%d", cfg.pin_bl);
    }
    setPanel(&_panel);
    {
      auto tcfg = _touch.config();
      tcfg.x_min = 0;
      tcfg.y_min = 0;
      // FT5x06 on this panel reports ~320x480 in the sensor's native portrait axes
      tcfg.x_max = 319;
      tcfg.y_max = 479;
      tcfg.pin_int = (int)WAVEX_CTP_GPIO_INT;   // may be -1 if unused
      tcfg.pin_rst = (int)WAVEX_CTP_GPIO_RST;
      tcfg.bus_shared = false;
      // Keep touch in its native orientation; rely on panel setRotation(3)
      tcfg.offset_rotation = 0;
      tcfg.i2c_port = 0;
      tcfg.i2c_addr = 0x38;
      tcfg.pin_sda = (int)WAVEX_CTP_GPIO_SDA;
      tcfg.pin_scl = (int)WAVEX_CTP_GPIO_SCL;
      tcfg.freq = 400000;
      _touch.config(tcfg);
      _panel.setTouch(&_touch);
      ESP_LOGI("LGFX_Device", "Touch config: I2C port=%d, addr=0x%02X, SDA=%d, SCL=%d, INT=%d, RST=%d", tcfg.i2c_port, tcfg.i2c_addr, tcfg.pin_sda, tcfg.pin_scl, tcfg.pin_int, tcfg.pin_rst);
    }
  }
};

static LGFX_DeviceFinal s_lgfx;
static const char *LGFX_TAG = "lgfx";
#endif

#ifdef WAVEX_HAVE_LGFX
extern "C" lgfx::LGFX_Device* lgfxInternal() {
  return &s_lgfx;
}
#endif

extern "C" void lgfx_device_init(void) {
#ifdef WAVEX_HAVE_LGFX
  ESP_LOGI(LGFX_TAG, "LGFX init begin");
  if (s_lgfx.init()) {
    ESP_LOGI(LGFX_TAG, "LGFX init successful.");
  } else {
    ESP_LOGE(LGFX_TAG, "LGFX init failed!");
    return;
  }

  // Check if touch is available
  if (s_lgfx.touch()) {
    ESP_LOGI(LGFX_TAG, "Touch controller detected and initialized.");
  } else {
    ESP_LOGW(LGFX_TAG, "Touch controller NOT detected!");
  }

  // Rotate to correct on-hardware orientation (180° from previous)
  s_lgfx.setRotation(3);
  s_lgfx.setBrightness(179); // Default to 70% instead of 100%
  s_lgfx.fillScreen(0x0000);
  ESP_LOGI(LGFX_TAG, "LGFX init done");
#endif
}

extern "C" void lgfx_device_set_brightness(int brightness) {
#ifdef WAVEX_HAVE_LGFX
  if (brightness < 0) brightness = 0;
  if (brightness > 255) brightness = 255;
  s_lgfx.setBrightness((uint8_t)brightness);
#else
  (void)brightness;
#endif
}

extern "C" void lgfx_device_draw_splash(void) {
#ifdef WAVEX_HAVE_LGFX
  auto &lcd = s_lgfx;
  lcd.fillScreen(0x0000);
  // Simple bars + text rectangle to confirm output without font deps
  lcd.fillRect(0, 0, 480, 24, 0x7BEF);      // grey top bar
  lcd.fillRect(0, 296, 480, 24, 0x7BEF);    // grey bottom bar
  lcd.fillRect(20, 60, 440, 200, 0x001F);   // blue block
  lcd.drawRect(18, 58, 444, 204, 0xFFFF);   // white outline
  ESP_LOGI(LGFX_TAG, "LGFX splash drawn");
#endif
}

extern "C" bool lgfx_device_get_touch(unsigned short* x, unsigned short* y) {
#ifdef WAVEX_HAVE_LGFX
  if (!x || !y) return false;
  bool touched = s_lgfx.getTouch(x, y);
  if (touched) {
    ESP_LOGI(LGFX_TAG, "Touch event: x=%u, y=%u", *x, *y);
  }
  if (!touched) return false;
  // Clamp to display bounds to avoid wrap-around when type-converted
  int32_t xi = static_cast<int32_t>(*x);
  int32_t yi = static_cast<int32_t>(*y);
  if (xi < 0) xi = 0;
  if (yi < 0) yi = 0;
  if (xi >= WAVEX_LCD_H_RES) xi = WAVEX_LCD_H_RES - 1;
  if (yi >= WAVEX_LCD_V_RES) yi = WAVEX_LCD_V_RES - 1;
  *x = static_cast<unsigned short>(xi);
  *y = static_cast<unsigned short>(yi);
  return true;
#else
  (void)x; (void)y; return false;
#endif
}


