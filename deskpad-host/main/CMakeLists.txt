# Auto-generate the LCD key JPEGs at configure time. Editing
# tools/gen_keyimages.py, or adding/removing/editing files in
# ../source_images/, triggers reconfigure on the next build via
# CMAKE_CONFIGURE_DEPENDS, so the user never has to run it manually.
idf_build_get_property(python PYTHON)
set(GEN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_keyimages.py")

# Tracking source_images/: the directory's mtime catches add/remove, individual
# files catch in-place edits. We deliberately *don't* use CONFIGURE_DEPENDS on
# file(GLOB) — that form is rejected when ESP-IDF re-evaluates this file in
# script mode to extract REQUIRES.
set(SOURCE_IMAGES_DIR "${CMAKE_CURRENT_LIST_DIR}/../source_images")
file(GLOB USER_SOURCE_IMAGES "${SOURCE_IMAGES_DIR}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${GEN_SCRIPT}" "${SOURCE_IMAGES_DIR}" ${USER_SOURCE_IMAGES})

# Pillow isn't in the IDF venv by default. Install it once if missing.
execute_process(
    COMMAND "${python}" -c "import PIL"
    RESULT_VARIABLE _pil_check
    OUTPUT_QUIET ERROR_QUIET)
if(NOT _pil_check EQUAL 0)
    message(STATUS "Installing pillow into IDF venv for key-image generation")
    execute_process(COMMAND "${python}" -m pip install --quiet pillow)
endif()

# Generate into the build tree, not the source tree, so the artifacts don't
# pollute the repo. EMBED_FILES accepts absolute paths.
set(KEYIMAGES_DIR "${CMAKE_CURRENT_BINARY_DIR}/keyimages")
execute_process(
    COMMAND "${python}" "${GEN_SCRIPT}" "${KEYIMAGES_DIR}"
    RESULT_VARIABLE _gen_result
    OUTPUT_VARIABLE _gen_output ERROR_VARIABLE _gen_output)
if(NOT _gen_result EQUAL 0)
    message(WARNING "gen_keyimages.py failed; embedding stale JPEGs:\n${_gen_output}")
endif()

# Build the embed-file list: 3 pages x 6 keys x (default + pressed) = 36 files.
set(KEYIMAGES "")
foreach(page RANGE 0 2)
    foreach(key RANGE 0 5)
        list(APPEND KEYIMAGES
            "${KEYIMAGES_DIR}/page${page}_key${key}.jpg"
            "${KEYIMAGES_DIR}/page${page}_key${key}_pressed.jpg")
    endforeach()
endforeach()

idf_component_register(
    SRCS
        # core
        "main.c" "actions.c" "config.c" "host_state.c" "secrets.c" "system_status.c"
        "notify.c"
        # ddc/
        "ddc/ddc.c" "ddc/kvm_detect.c"
        # hid/
        "hid/hid_link.c" "hid/hid_keys.c"
        # net/
        "net/network.c" "net/http_server.c" "net/mqtt.c" "net/ha_discovery.c"
        # renderers/
        "renderers/key_anim.c" "renderers/live_key.c" "renderers/jpeg_enc.c"
        "renderers/clock_render.c" "renderers/monitor_render.c" "renderers/bulb_render.c"
    INCLUDE_DIRS "." "ddc" "hid" "net" "renderers"
    REQUIRES driver usb akp03e esp_driver_i2c esp_driver_jpeg
             esp_timer lvgl nvs_flash json mqtt
             esp_eth esp_netif esp_event esp_http_server app_update
    EMBED_FILES ${KEYIMAGES} "spa_index.html"
)
