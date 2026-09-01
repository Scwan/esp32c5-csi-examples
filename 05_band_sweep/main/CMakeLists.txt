idf_component_register(SRCS "sweep_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES csi_c5 esp_wifi nvs_flash esp_netif esp_event)
