<<<<<<< HEAD
idf_component_register(
    SRCS "main.c" "display.c" "buttons.c" "teclado.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_i2s esp_driver_gpio spi_flash esp_wifi esp_event nvs_flash mbedtls console
)
=======
idf_component_register(SRCS "hello_world_main.c" "e2ee.c" "cli.c"
                            "display.c" "buttons.c" "teclado.c"
                            "inbox.c" "peer_pick.c"
                       PRIV_REQUIRES spi_flash esp_wifi esp_event nvs_flash mbedtls console
                                     esp_lcd esp_driver_spi esp_driver_gpio
                       INCLUDE_DIRS "")
>>>>>>> origin/crypto
