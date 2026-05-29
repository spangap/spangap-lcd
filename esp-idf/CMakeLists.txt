file(GLOB LCD_SRCS "src/lcd_ui/*.cpp" "src/lcd_ui/*.c")

# Cross-straddle REQUIRES (today: just spangap-core) come through the
# generated spangap_requires.cmake; the file exists in every build, even
# for straddles with no optional_requires, so consumers don't need to
# special-case.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS ${LCD_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/lcd_ui"
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_event
        esp_timer
        esp_psram
)

# Display stack — linked here (not in REQUIRES). The presence of this
# straddle in the staged set IS the gate (it's an optional_requires of
# every consumer that uses it); when staged we always want LVGL linked.
# `esp_lcd` ships with IDF; `lvgl` is pinned in our own idf_component.yml
# (we own the launcher built against LVGL v9, so the version is ours to
# choose); `esp_lcd_touch` comes in transitively via the consumer's
# specific touch driver (e.g. esp_lcd_touch_gt911).
#
# lvgl is linked PUBLIC because `lcd.h` (our public header) #include's
# `lvgl.h`, so every consumer that picks up spangap-lcd through
# ${SPANGAP_REQUIRES} also sees lvgl on its compile path. esp_lcd /
# esp_lcd_touch only appear in our .cpp impls — keep them PRIVATE so
# we don't push them into every consumer's include path.
foreach(_lcd_dep IN ITEMS lvgl__lvgl)
    idf_component_get_property(_lcd_lib ${_lcd_dep} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${_lcd_lib})
endforeach()
foreach(_lcd_dep IN ITEMS esp_lcd espressif__esp_lcd_touch)
    idf_component_get_property(_lcd_lib ${_lcd_dep} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${_lcd_lib})
endforeach()
