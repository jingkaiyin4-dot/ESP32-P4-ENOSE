idf_component_register(
    SRCS "bme680.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer freertos
)
