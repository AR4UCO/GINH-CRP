idf_component_register(SRCS "hello_world_main.c" "display.c" "buttons.c"
                            "audio.c" "transfer.c" "dictado.c" "ui.c"
                            "e2ee.c" "cli.c" "inbox.c" "peer_pick.c" "teclado.c"
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_gpio
                                     esp_driver_i2s esp_rom
                                     esp_driver_usb_serial_jtag
                                     esp_wifi esp_event nvs_flash mbedtls console
                                     esp_timer
                       INCLUDE_DIRS "")
