idf_component_register(
    SRCS "electronic_nose_gateway.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_http_client nvs_flash json mbedtls
)