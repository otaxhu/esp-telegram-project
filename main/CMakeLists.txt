idf_component_register(SRCS "src/main.c"
                            "src/wifi.c"
                       INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-parentheses)
