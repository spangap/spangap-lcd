# spangap-lcd helper functions, available to consumer CMakeLists.txt after
# project() returns. ESP-IDF includes this file automatically when this
# component is in the build.

# spangap_lcd_icons([SRC_DIR <dir>] [PARTITION fixed_a])
#
# Stages launcher icon SOURCES (*.svg) into the factory-image tree at
# data_merged/icons/<name>.svg, so they ship to /fixed/icons/<name>.svg. The lcd
# module (lcd_icons.cpp) rasterizes them on the device at the tile size with
# nanosvg — there is no build-time raster pipeline and no size buckets. Sources
# live outside data/ so only what's merged here ships.
#
# Two source dirs are merged at build time: spangap-lcd's own
# assets/lcd-icons/ (platform defaults — gear/log/cli) always, plus the
# consumer's SRC_DIR if given. On a basename collision the consumer wins — the
# build-time analogue of the data/ merge. SRC_DIR is therefore optional; a
# consumer with no icons of its own still inherits the platform defaults.
#
# Requires spangap_create_factory_image() first.
function(spangap_lcd_icons)
    cmake_parse_arguments(_DLI "" "SRC_DIR;PARTITION" "" ${ARGN})
    if(NOT _DLI_PARTITION)
        # Default to whatever bootstrap.cmake picked — fixed_a (OTA on) or fixed (OTA off).
        set(_DLI_PARTITION "${SPANGAP_FIXED_PARTITION}")
    endif()
    if(NOT _DLI_PARTITION)
        message(FATAL_ERROR
            "spangap_lcd_icons: no PARTITION specified and SPANGAP_FIXED_PARTITION "
            "isn't set (bootstrap.cmake didn't run?)")
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

    set(_data_merged "${CMAKE_BINARY_DIR}/data_merged")

    # Platform defaults first (low precedence), consumer SRC_DIR second so it
    # overrides on basename. SRC_DIR is optional.
    set(_src_args --src "${_lcd_dir}/assets/lcd-icons")
    if(_DLI_SRC_DIR)
        list(APPEND _src_args --src "${_DLI_SRC_DIR}")
    endif()

    add_custom_target(spangap_lcd_icons ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_data_merged}/icons"
        COMMAND python3 "${_lcd_dir}/scripts/lcd-icons.py"
            ${_src_args}
            --out "${_data_merged}/icons"
        COMMENT "Staging lcd icon SVGs -> /fixed/icons"
        VERBATIM)

    # Run after the data merge populates data_merged, before the /fixed image.
    add_dependencies(spangap_lcd_icons spangap_data_merge)
    add_dependencies(spanfs_${_DLI_PARTITION}_bin spangap_lcd_icons)
endfunction()

# spangap_lcd_fonts([SRC_DIR <dir>] [PARTITION fixed_a])
#
# Subsets spangap-lcd's shipped vector faces (fonts/ui.ttf, ui-semibold.ttf,
# mono.ttf) with fonttools' pyftsubset into the factory-image staging tree at
# data_merged/fonts/<face>.ttf, so they ship RAW (uncompressed) to
# /fixed/fonts/<face>.ttf and mmap in place for FreeType / tiny_ttf. The
# LCD_FONT_ENGINE choice (lcd_fonts.cpp) opens them by that path.
#
# SRC_DIR overrides the default source dir (spangap-lcd's own fonts/), letting a
# consumer swap in different faces of the same names. Best-effort: the helper
# script copies a face verbatim if fonttools isn't importable, so the build
# still succeeds (the /fixed image is just larger). In LCD_FONT_BITMAP builds no
# vector face is opened at runtime, but shipping them is harmless — a board that
# never selects a vector engine can drop this call.
function(spangap_lcd_fonts)
    cmake_parse_arguments(_DLF "" "SRC_DIR;PARTITION" "" ${ARGN})
    if(NOT _DLF_PARTITION)
        set(_DLF_PARTITION "${SPANGAP_FIXED_PARTITION}")
    endif()
    if(NOT _DLF_PARTITION)
        message(FATAL_ERROR
            "spangap_lcd_fonts: no PARTITION and SPANGAP_FIXED_PARTITION unset "
            "(bootstrap.cmake didn't run?)")
    endif()
    if(NOT TARGET spangap_data_merge)
        message(FATAL_ERROR "spangap_lcd_fonts: call spangap_create_factory_image() first")
    endif()

    set(_lcd_dir "")
    foreach(_l IN ITEMS spangap-lcd spangap__spangap-lcd)
        idf_component_get_property(_d ${_l} COMPONENT_DIR)
        if(_d)
            set(_lcd_dir "${_d}")
            break()
        endif()
    endforeach()
    if(NOT _lcd_dir)
        message(FATAL_ERROR "spangap_lcd_fonts: spangap-lcd component not in build")
    endif()

    set(_src "${_lcd_dir}/fonts")
    if(_DLF_SRC_DIR)
        set(_src "${_DLF_SRC_DIR}")
    endif()
    set(_data_merged "${CMAKE_BINARY_DIR}/data_merged")

    add_custom_target(spangap_lcd_fonts ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_data_merged}/fonts"
        COMMAND python3 "${_lcd_dir}/scripts/lcd-fonts.py"
            --src "${_src}"
            --out "${_data_merged}/fonts"
        COMMENT "Subsetting lcd vector fonts -> /fixed/fonts"
        VERBATIM)

    add_dependencies(spangap_lcd_fonts spangap_data_merge)
    add_dependencies(spanfs_${_DLF_PARTITION}_bin spangap_lcd_fonts)
endfunction()
