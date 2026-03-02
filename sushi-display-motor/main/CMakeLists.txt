idf_component_register(SRCS "gc9a01.c" "ssd1306.c" "main.c"
                       PRIV_REQUIRES spi_flash spiffs driver esp_lcd esp_timer)
spiffs_create_partition_image(spiffs ../flash_fs FLASH_IN_PROJECT)
