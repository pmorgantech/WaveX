set(SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_class.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_id_builtin.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_pos.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_property.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_scroll.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_style.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_style_gen.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_obj_tree.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_refr.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_refr_private.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_group.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_indev.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_indev_scroll.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/core/lv_event.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/display/lv_display.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_fmt_txt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_loader.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_montserrat_12.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_montserrat_14.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_montserrat_16.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_montserrat_18.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/font/lv_font_montserrat_20.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/lv_widget.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/arc/lv_arc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/button/lv_button.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/label/lv_label.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/menu/lv_menu.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/slider/lv_slider.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/switch/lv_switch.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/textarea/lv_textarea.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/widgets/image/lv_image.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_anim.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_area.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_async.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_color.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_fs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_ll.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_log.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_math.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_palette.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_style.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_style_gen.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_timer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_txt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/misc/lv_utils.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_arc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_buf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_image.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_label.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_line.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_mask.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_rect.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_draw_triangle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_image_cache.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/draw/lv_image_decoder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/layouts/lv_layout.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/layouts/flex/lv_flex.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/themes/lv_theme.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/themes/default/lv_theme_default.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/stdlib/lv_mem.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/stdlib/lv_string.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/lv_init.c"
)

# Automatically find all .c files in the LVGL source directory
file(GLOB_RECURSE LVGL_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src/*.c"
)

idf_component_register(
    SRCS ${LVGL_SOURCES}
    INCLUDE_DIRS 
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/lvgl"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    REQUIRES freertos esp_timer esp_common
)

# Add compiler definitions for LVGL
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LV_CONF_INCLUDE_SIMPLE=1
    LV_USE_FREERTOS=1
) 