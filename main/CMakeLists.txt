idf_component_register(
    SRCS "test.c" "application.c" "boards/atk_box3/board.c"
         "camera/camera_capture.c" "display/lcd_display.c"
         "storage/camera_storage.c" "storage/media_format.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_lcd esp_timer esp_psram
                  esp_video esp_driver_gpio esp_driver_sdspi
                  esp_driver_usb_serial_jtag fatfs sdmmc)
