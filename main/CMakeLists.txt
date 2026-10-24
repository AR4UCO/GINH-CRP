idf_component_register(
    SRCS "buttons.c" "cli.c" "display.c" "e2ee.c" "inbox.c" "main.c" "peer_pick.c" "teclado.c	"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_i2s esp_driver_gpio spi_flash esp_wifi esp_event nvs_flash mbedtls console
)
