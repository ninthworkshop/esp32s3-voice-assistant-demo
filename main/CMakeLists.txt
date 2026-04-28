idf_component_register(SRCS main.c
		       INCLUDE_DIRS "."
 		       PRIV_REQUIRES esp-sr driver mbedtls esp_http_client esp_wifi nvs_flash)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
