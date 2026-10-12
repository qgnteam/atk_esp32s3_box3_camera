idf_component_register(
    SRCS "test.c" "application.c" "boards/atk_box3/board.c"
         "camera/camera_capture.c" "display/lcd_display.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_lcd esp_timer esp_psram
                  esp_video)
