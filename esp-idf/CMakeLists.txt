# The on-device UI is the phone-style shell: the LcdApp object model + chrome
# (shell/) and the built-in apps (apps/), plus the retained display/LVGL/panel/
# icon/text-view/terminal/settings core in src/lcd_ui/.
file(GLOB LCD_SRCS
    "src/lcd_ui/*.cpp" "src/lcd_ui/*.c"
    "src/lcd_ui/shell/*.cpp" "src/lcd_ui/apps/*.cpp")

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
    PRIV_INCLUDE_DIRS "src/lcd_ui" "src/lcd_ui/shell" "libvterm/include" "libvterm/src"
                      "third_party/nanosvg"
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_event
        esp_timer
        esp_psram
)

# Vendored third-party C — don't let our warning flags (-Werror et al.) fail the
# build on upstream code we don't maintain. libvterm + nanosvg (the runtime SVG
# icon rasterizer, compiled once in nanosvg_impl.c).
set_source_files_properties(${VTERM_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/lcd_ui/nanosvg_impl.c"
                            PROPERTIES COMPILE_OPTIONS "-w")

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

# FreeType link glue (the LCD_FONT_FREETYPE engine). LVGL's esp.cmake compiles
# lv_freetype.c when CONFIG_LV_USE_FREETYPE=y but never links FreeType — its
# PRIV_REQUIRES is only `esp_timer log (+fatfs)`, so lv_freetype.c can't find
# ft2build.h. Wire it ourselves, exactly as esp_lvgl_port does: hang the
# espressif__freetype lib off the lvgl component lib so its header + archive
# reach lv_freetype.c. espressif__freetype is always staged (unconditional dep)
# but only referenced here when the engine selects it, so BITMAP/TINY_TTF images
# dead-strip it.
if(CONFIG_LCD_FONT_FREETYPE)
    idf_component_get_property(_lvgl_lib lvgl__lvgl          COMPONENT_LIB)
    idf_component_get_property(_ft_lib   espressif__freetype COMPONENT_LIB)
    target_link_libraries(${_lvgl_lib} PRIVATE ${_ft_lib})
endif()

# LVGL custom-malloc backend (src/lcd_ui/lv_mem_spangap.cpp, gated on
# CONFIG_LV_USE_CUSTOM_MALLOC) is referenced ONLY by the lvgl component, which is
# linked before this one — single-pass linking therefore won't pull our object in
# to satisfy lvgl's lv_mem_init/lv_*_core. Force it: -u makes the linker treat
# lv_mem_init as needed from the start, pulling our backend (and with it all the
# other lv_*_core symbols) regardless of link order.
if(CONFIG_LV_USE_CUSTOM_MALLOC)
    target_link_options(${COMPONENT_LIB} INTERFACE "-u" "lv_mem_init")
endif()
