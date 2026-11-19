#include "web_routes.h"

extern const char index_start[] asm("_binary_index_html_start"), index_end[] asm("_binary_index_html_end");
extern const char js_start[] asm("_binary_ui_js_gz_start"), js_end[] asm("_binary_ui_js_gz_end");
extern const char css_start[] asm("_binary_ui_css_gz_start"), css_end[] asm("_binary_ui_css_gz_end");
static esp_err_t page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(r, index_start, index_end - index_start);
}
static esp_err_t stylesheet(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/css");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    return httpd_resp_send(r, css_start, css_end - css_start);
}
static esp_err_t script(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/javascript");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    return httpd_resp_send(r, js_start, js_end - js_start);
}
esp_err_t fabric_http_web_start(httpd_handle_t server, fabric_manager_handle_t manager)
{
    const httpd_uri_t routes[] = {{.uri = "/", .method = HTTP_GET, .handler = page},
                                  {.uri = "/ui.css", .method = HTTP_GET, .handler = stylesheet},
                                  {.uri = "/ui.js", .method = HTTP_GET, .handler = script}};
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK)
            return err;
    }
    return ESP_OK;
}
