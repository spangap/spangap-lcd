file(GLOB LCD_SRCS "src/lcd_ui/*.cpp" "src/lcd_ui/*.c")

idf_component_register(
    SRCS ${LCD_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/lcd_ui"
    REQUIRES
        spangap-core
        driver
        esp_event
        esp_timer
        esp_psram
)

# Display stack — linked here (not via REQUIRES) because the dependency is
# Kconfig-gated and REQUIRES can't see CONFIG_* (early expansion). CONFIG_*
# IS available now. `esp_lcd` ships with IDF; `lvgl`/`esp_lcd_touch` come
# from the consumer's managed dependencies, so a non-display consumer never
# pulls LVGL. Linking the component libs propagates their public include
# dirs to our lcd_ui sources and resolves lv_*/esp_lcd_* symbols at link.
#
# lvgl is linked PUBLIC because `lcd.h` (our public header) #include's
# `lvgl.h`, so every consumer that REQUIRES spangap-lcd (spangap-net,
# spangap-web, upnp, duckdns, wg, acme, …) must also see lvgl on its
# compile path. esp_lcd / esp_lcd_touch only appear in our .cpp impls —
# keep them PRIVATE so we don't push them into every consumer's
# include path.
foreach(_lcd_dep IN ITEMS lvgl__lvgl)
    idf_component_get_property(_lcd_lib ${_lcd_dep} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${_lcd_lib})
endforeach()
foreach(_lcd_dep IN ITEMS esp_lcd espressif__esp_lcd_touch)
    idf_component_get_property(_lcd_lib ${_lcd_dep} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${_lcd_lib})
endforeach()
