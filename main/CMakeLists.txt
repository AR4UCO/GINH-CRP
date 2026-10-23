idf_component_register(SRCS "hello_world_main.c" "e2ee.c" "cli.c"
                       PRIV_REQUIRES spi_flash esp_wifi esp_event nvs_flash mbedtls console
                       INCLUDE_DIRS "")
