idf_component_register(SRCS "hello_world_main.c" "e2ee.c"
                       PRIV_REQUIRES spi_flash esp_wifi esp_event nvs_flash mbedtls
                       INCLUDE_DIRS "")
