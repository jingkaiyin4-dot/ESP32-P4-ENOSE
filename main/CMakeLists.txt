set(E_NOSE_SRCS main.cpp lvgl_adapter_init.c ui.cpp ui_font_xiaozhi_14.c app_video.c app_system.c web_dashboard.cpp sd_card_bg.c uart_receiver.cpp cloud_sync.cpp model_mgr.cpp xiaozhi_ai_service.c)
set(E_NOSE_REQUIRES bme680_sensor bsp_extra esp_timer nvs_flash esp_wifi esp_netif esp_http_client esp_http_server esp_websocket_client json espressif__esp32_p4_function_ev_board_noglib mbedtls efuse esp_hw_support micro_opus espressif__esp_new_jpeg esp_driver_ppa)

idf_component_register(
    SRCS ${E_NOSE_SRCS}
    INCLUDE_DIRS .)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE 
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB} 
        PRIVATE 
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        -finput-charset=UTF-8
        -fexec-charset=UTF-8
)

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
