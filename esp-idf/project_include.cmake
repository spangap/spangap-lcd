# spangap-lcd helper functions, available to consumer CMakeLists.txt after
# project() returns. ESP-IDF includes this file automatically when this
# component is in the build.

# spangap_lcd_icons([SRC_DIR <dir>] [SIZES "36x36"] [PARTITION fixed_a])
#
# Rasterizes launcher icon SOURCES (*.svg / *.png) into LVGL RGB565A8 .bin
# files — one per size bucket — written into the factory-image staging tree at
# data_merged/lcd/icons/<WxH>/<name>.bin, so they ship to
# /fixed/lcd/icons/<WxH>/<name>.bin. The lcd module loads them by basename at a
# fixed resolution (LAUNCHER_ICON_RES, 36x36, the launcher tile size), rendered
# native — so build at least that bucket. Sources live outside data/ so only the
# generated .bin ships.
#
# Two source dirs are merged at build time: spangap-lcd's own
# assets/lcd-icons/ (platform defaults — gear/log/cli) always, plus the
# consumer's SRC_DIR if given. On a basename collision the consumer wins — the
# build-time analogue of the data/ merge. SRC_DIR is therefore optional; a
# consumer with no icons of its own still inherits the platform defaults.
#
# Best-effort: the helper script warns and skips if the host lacks Pillow /
# cairosvg or LVGLImage.py — the build still succeeds (tiles show their label
# until icons are present). Requires spangap_create_factory_image() first and
# CONFIG_SPANGAP_LCD=y (so the `lvgl` component, which ships LVGLImage.py, is
# in the build).
function(spangap_lcd_icons)
    cmake_parse_arguments(_DLI "" "SRC_DIR;PARTITION" "SIZES" ${ARGN})
    if(NOT _DLI_SIZES)
        set(_DLI_SIZES "36x36")
    endif()
    if(NOT _DLI_PARTITION)
        set(_DLI_PARTITION "fixed_a")
    endif()
    if(NOT TARGET spangap_data_merge)
        message(FATAL_ERROR "spangap_lcd_icons: call spangap_create_factory_image() first")
    endif()

    # Resolve the spangap-lcd component dir (may be the staged or the
    # workspace-local form).
    set(_lcd_dir "")
    foreach(_l IN ITEMS spangap-lcd spangap__spangap-lcd)
        idf_component_get_property(_d ${_l} COMPONENT_DIR)
        if(_d)
            set(_lcd_dir "${_d}")
            break()
        endif()
    endforeach()
    if(NOT _lcd_dir)
        message(FATAL_ERROR "spangap_lcd_icons: spangap-lcd component not in build")
    endif()

    # LVGL ships LVGLImage.py. The component is "lvgl__lvgl" (managed) or
    # "lvgl" (vendored) — resolve whichever is in the build; if neither, leave
    # the path empty and the helper script skips with a warning.
    idf_build_get_property(_all_comps BUILD_COMPONENTS)
    set(_lvgl_image_py "")
    foreach(_lc IN ITEMS lvgl__lvgl lvgl)
        if(_lc IN_LIST _all_comps)
            idf_component_get_property(_lvgl_dir ${_lc} COMPONENT_DIR)
            set(_lvgl_image_py "${_lvgl_dir}/scripts/LVGLImage.py")
            break()
        endif()
    endforeach()

    set(_data_merged "${CMAKE_BINARY_DIR}/data_merged")
    string(REPLACE ";" "," _sizes_csv "${_DLI_SIZES}")

    # Platform defaults first (low precedence), consumer SRC_DIR second so it
    # overrides on basename. SRC_DIR is optional.
    set(_src_args --src "${_lcd_dir}/assets/lcd-icons")
    if(_DLI_SRC_DIR)
        list(APPEND _src_args --src "${_DLI_SRC_DIR}")
    endif()

    add_custom_target(spangap_lcd_icons ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_data_merged}/lcd/icons"
        COMMAND python3 "${_lcd_dir}/scripts/lcd-icons.py"
            ${_src_args}
            --out "${_data_merged}/lcd/icons"
            --sizes "${_sizes_csv}"
            --lvgl-image-py "${_lvgl_image_py}"
        COMMENT "Rasterizing lcd icons (${_sizes_csv}) -> /fixed/lcd/icons"
        VERBATIM)

    # Run after the data merge populates data_merged, before the LittleFS image.
    add_dependencies(spangap_lcd_icons spangap_data_merge)
    add_dependencies(littlefs_${_DLI_PARTITION}_bin spangap_lcd_icons)
endfunction()
