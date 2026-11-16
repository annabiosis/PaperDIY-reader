set(APP_SRCS
    "main.cpp"

    # board, input and the developer console
    "board/board.cpp"
    "input/gestures.cpp"
    "input/input_queue.cpp"
    "debug/console.cpp"

    # text pipeline
    "core/utf8.cpp"
    "core/encoding.cpp"
    "core/byte_source.cpp"
    "text/font.cpp"
    "text/layout.cpp"
    "text/hyphenate.cpp"

    # formats
    "formats/formats.cpp"
    "formats/txt.cpp"
    "formats/markdown.cpp"
    "formats/fb2.cpp"
    "formats/epub.cpp"
    "formats/pdf.cpp"

    # storage and ui
    "store/progress.cpp"
    "ui/draw.cpp"
    "ui/icon_draw.cpp"
    "ui/icons_atlas.cpp"
    "ui/app.cpp"

    # generated font atlases
    "text/atlas/atlas_14.cpp"
    "text/atlas/atlas_18.cpp"
    "text/atlas/atlas_24.cpp"
    "text/atlas/atlas_28.cpp"
    "text/atlas/atlas_32.cpp"
    "text/atlas/atlas_catalog.cpp"
)

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS
        "."
        "board"
        "core"
        "debug"
        "formats"
        "input"
        "store"
        "text"
        "ui"
    REQUIRES
        app_update
        M5GFX
        M5Unified
        json
        fatfs
        sdmmc
        esp_driver_spi
        esp_driver_sdspi
        esp_driver_gpio
        esp_driver_usb_serial_jtag
        nvs_flash
)

# Build stamp for `rdr version`: a release has to be able to say when it was built, and
# esp_app_desc only records when CMake was configured.
string(TIMESTAMP BUILD_STAMP "%Y-%m-%d %H:%M")
target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_STAMP="${BUILD_STAMP}")
