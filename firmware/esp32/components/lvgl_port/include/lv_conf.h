// Increase LVGL memory pool (if using built-in memory management)
#define LV_MEM_SIZE    (256 * 1024U)  // 256KB for LVGL

// Memory allocation settings
#define LV_MEM_BUF_MAX_NUM     16
#define LV_MEMCPY_MEMSET_STD   1

// Optimize font and image caching
#define LV_FONT_GLYPH_CACHE_SIZE 256
#define LV_IMG_CACHE_DEF_SIZE   4

// Reduce animation memory usage
#define LV_USE_ANIMATION        1
#define LV_ANIM_DEF_TIME        200 