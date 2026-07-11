zephyr_library()

zephyr_include_directories_ifdef(CONFIG_STATUS_LED include)

zephyr_library_sources_ifdef(CONFIG_STATUS_LED
  src/status_led.c
  src/status_led_priority.c
  src/status_led_pattern_engine.c
  src/status_led_patterns.c
)

zephyr_library_sources_ifdef(CONFIG_STATUS_LED_BACKEND_WS2812
  src/backends/status_led_hw_ws2812.c
)

zephyr_library_sources_ifdef(CONFIG_STATUS_LED_BACKEND_GPIO
  src/backends/status_led_hw_gpio.c
)

zephyr_library_sources_ifdef(CONFIG_STATUS_LED_BACKEND_PWM
  src/backends/status_led_hw_pwm.c
)

zephyr_library_sources_ifdef(CONFIG_STATUS_LED_BACKEND_MONO_GPIO
  src/backends/status_led_hw_mono_gpio.c
)

zephyr_library_sources_ifdef(CONFIG_STATUS_LED_BACKEND_MONO_PWM
  src/backends/status_led_hw_mono_pwm.c
)
