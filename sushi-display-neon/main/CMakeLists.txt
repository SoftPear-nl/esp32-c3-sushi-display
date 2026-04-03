idf_component_register(SRCS "main.c" "neon_letter_service.c" "display_sequence.c"
                      INCLUDE_DIRS "."
                      REQUIRES spi_flash spiffs esp_lcd esp_timer driver)
#spiffs_create_partition_image(spiffs ../flash_fs FLASH_IN_PROJECT)
spiffs_create_partition_image(spiffs ../flash_fs)
