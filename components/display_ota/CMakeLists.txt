idf_component_register(
    SRCS "display_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ble_client
        app_update
        esp_event
        esp_http_client
        esp_netif
        esp-tls
        esp_wifi
        mbedtls
)
