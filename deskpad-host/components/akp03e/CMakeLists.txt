idf_component_register(
    SRCS "src/akp03e.c" "src/akp03e_usb.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES usb
    PRIV_REQUIRES esp_timer freertos
)
