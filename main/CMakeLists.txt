idf_component_register(
    SRCS "main.c" "library_app.c" "device_api.c" "english_app.c" "audio_bus.c" "star_game.c" "notification_center.c" "launcher.c" "wifi_setup.c" "companion_apps.c" "ble_remote.c" "voice_input.c" "music_input.c" "wallpaper_input.c" "avatar_store.c" "avatar_face.c" "screenshot_bridge.c" "fonts/round_clock_cn_16.c" "fonts/round_clock_cn_20.c" "fonts/round_clock_cn_28.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt json mbedtls esp_wifi esp_http_client esp_http_server esp_netif nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_usb_serial_jtag spiffs espressif__zlib
    EMBED_FILES "assets/default.rgb565" "wifi_setup.html"
    EMBED_TXTFILES
        "certs/device-ca.pem"
        "certs/device-client.pem"
        "certs/device-client.key"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
target_sources(${COMPONENT_LIB} PRIVATE "content_assets.c" "kids_apps.c" "story_app.c" "pet_dialogue.c")
esptool_py_flash_to_partition(flash "content" "${CMAKE_CURRENT_SOURCE_DIR}/assets/content.bin")
