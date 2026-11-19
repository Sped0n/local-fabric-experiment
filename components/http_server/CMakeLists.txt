idf_component_register(SRCS http_server.c access_token.c web_routes.c mcp/transport.c mcp/catalog.c mcp/subscriptions.c
    INCLUDE_DIRS include PRIV_INCLUDE_DIRS "." REQUIRES local_fabric
    PRIV_REQUIRES mbedtls nvs_flash esp_http_server esp_timer esp_hw_support espressif__cjson
    EMBED_FILES index.html ui.js.gz ui.css.gz EMBED_TXTFILES mcp/rule_definition.json)
