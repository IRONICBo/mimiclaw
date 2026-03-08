idf_component_register(
    SRCS
        "mimi.c"
        "bus/message_bus.c"
        "wifi/wifi_manager.c"
        "channels/telegram/telegram_bot.c"
        "channels/feishu/feishu_bot.c"
        "llm/llm_proxy.c"
        "agent/agent_loop.c"
        "agent/context_builder.c"
        "memory/memory_store.c"
        "memory/session_mgr.c"
        "gateway/ws_server.c"
        "micropython/mpy_runner.c"
        "micropython/mpy_stubs.c"
        "micropython/mpy_uart_stubs.c"
        "compat/esp_tls_stubs.c"
        "cli/serial_cli.c"
        "proxy/http_proxy.c"
        "cron/cron_service.c"
        "heartbeat/heartbeat.c"
        "tools/tool_registry.c"
        "tools/tool_cron.c"
        "tools/tool_web_search.c"
        "tools/tool_get_time.c"
        "tools/tool_files.c"
        "skills/skill_loader.c"
    INCLUDE_DIRS
        "."
        "micropython"
        "compat"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls
        esp_timer esp_websocket_client tcp_transport
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
target_include_directories(${COMPONENT_LIB} PRIVATE ${IDF_PATH}/components/mbedtls/esp_crt_bundle/include)
