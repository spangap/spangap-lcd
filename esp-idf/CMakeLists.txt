file(GLOB LCD_SRCS "src/lcd_ui/*.cpp" "src/lcd_ui/*.c")
# Vendored libvterm (neovim mirror, MIT) — the VT100/xterm state machine behind
# the on-device terminal. Plain C, no autoconf; compiled straight into this
# component. Its public header is private to us (only lcd_term.cpp uses it).
file(GLOB VTERM_SRCS "libvterm/src/*.c")

# Cross-straddle REQUIRES (today: just spangap-core) come through the
# generated spangap_requires.cmake; the file exists in every build, even
# for straddles with no optional_requires, so consumers don't need to
# special-case.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS ${LCD_SRCS} ${VTERM_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/lcd_ui" "libvterm/include" "libvterm/src"
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_event
        esp_timer
        esp_psram
)

# Vendored third-party C — don't let our warning flags (-Werror et al.) fail the
# build on upstream code we don't maintain.
set_source_files_properties(${VTERM_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

# Display stack — linked here (not in REQUIRES). The presence of this
# straddle in the staged set IS the gate (it's an optional_requires of
# every consumer that uses it); when staged we always want LVGL linked.
# `esp_lcd` (with the built-in ST7789 driver) ships with IDF; `lvgl` and
# `esp_lcd_ili9341` are pinned in our own idf_component.yml — we own the
# launcher built against LVGL v9 and the panel bring-up, so those versions
# are ours to choose.
#
# lvgl is linked PUBLIC because `lcd.h` (our public header) #include's
# `lvgl.h`, so every consumer that picks up spangap-lcd through
# ${SPANGAP_REQUIRES} also sees lvgl on its compile path. esp_lcd /
# esp_lcd_ili9341 only appear in lcd_panel.cpp — keep them PRIVATE so we
# don't push them into every consumer's include path. esp_lcd_ili9341 is
# always staged (it's in our manifest) but dead-stripped unless
# CONFIG_LCD_CONTROLLER_ILI9341 selects it.
foreach(_lcd_dep IN ITEMS lvgl__lvgl)
    idf_component_get_property(_lcd_lib ${_lcd_dep} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${_lcd_lib})
endforeach()
foreach(_lcd_dep IN ITEMS esp_lcd espressif__esp_lcd_ili9341)
    idf_component_get_property(_lcd_lib ${_lcd_dep} COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${_lcd_lib})
endforeach()
