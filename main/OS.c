#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include <sys/param.h>
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include <string.h>


#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "cJSON.h"
#include "esp_sleep.h"

#include <esp_http_server.h>
#include "esp_spiffs.h"

#include "mqtt_client.h"

//Access point Credentials
#define EXAMPLE_ESP_WIFI_SSID      "Alex's Hub" 
#define EXAMPLE_ESP_WIFI_PASS      "testtest"

#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       1
#define STA_SSID "test"
#define STA_PASS "test"

#define steps_per_rev 200

//Pin declarations
#define STEP_PIN 32
#define DIR_PIN 14
#define LIMIT1 26
#define LIMIT2 25
#define LEDR 13
#define LEDG 12
#define LEDB 27
#define SYNC 33
#define REED 4

#define ESP_INTR_FLAG_DEFAULT 0

#define EXAMPLE_ESP_MAXIMUM_RETRY  20

//Global variables
static RTC_DATA_ATTR char __SSID[32];
static RTC_DATA_ATTR char __PWD[64];
static RTC_DATA_ATTR char __SYSNAME[32];
static RTC_DATA_ATTR char __SYSPWD[64];

static int __STOP = 1;

static esp_mqtt_client_handle_t __client;
static httpd_handle_t* server_handle;

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;


static const char *TAG = "Hub";

static xQueueHandle gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void* arg)
{
    int msg_id;
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
            if (io_num == 26 && gpio_get_level(io_num) == 1) { //LIMIT1 - Lock
                //__STOP = 1;
                ESP_LOGI(TAG, "LIMIT1 HIT: STOP = %d", __STOP);
                if (gpio_get_level(REED) == 1) {
                    msg_id = esp_mqtt_client_publish(__client, "success", "Lock", 0, 1, 1);
                    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
                }
            } else if (io_num == 25 && gpio_get_level(io_num) == 1) { //LIMIT2 - Unlock
                //__STOP = 1;
                ESP_LOGI(TAG, "LIMIT2 HIT: STOP = %d", __STOP);
                if (gpio_get_level(REED) == 1) {
                    msg_id = esp_mqtt_client_publish(__client, "success", "Unlock", 0, 1, 1);
                    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
                }
            }
        }
    }
}

//MQTT handler
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    __client = client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            gpio_set_level(LEDR, 0);
            gpio_set_level(LEDG, 1);
            gpio_set_level(LEDB, 0);
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

            msg_id = esp_mqtt_client_subscribe(client, "apartment/deadbolt-1", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "username", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "success", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_publish(client, "success", "Unlock", 0, 1, 1);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_publish(client, "login_user_topic", __SYSNAME, 0, 1, 1);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_publish(client, "login_password_topic", __SYSPWD, 0, 1, 1);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);



            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            if ((strcmp(event->topic, "apartment/deadbolt-1Lock") == 0)) {// && (strcmp(event->data, "Lock") == 0)) {
                ESP_LOGI(TAG, "LOCKING");
                __STOP = 0;
                gpio_set_level(DIR_PIN, 1);
                while(gpio_get_level(LIMIT1) == 1 && gpio_get_level(LIMIT2) == 1) {
                    ESP_LOGI(TAG, "PULSE");
                    gpio_set_level(STEP_PIN,1);
                    vTaskDelay(10/ portTICK_PERIOD_MS); 
                    gpio_set_level(STEP_PIN,0);
                    vTaskDelay(10/ portTICK_PERIOD_MS);
                }
                gpio_set_level(DIR_PIN, 0);
                for(int i = 0; i < 20; i++) {
                    ESP_LOGI(TAG, "PULSE");
                    gpio_set_level(STEP_PIN,1);
                    vTaskDelay(10/ portTICK_PERIOD_MS); 
                    gpio_set_level(STEP_PIN,0);
                    vTaskDelay(10/ portTICK_PERIOD_MS);
                }
            } else if ((strcmp(event->topic, "apartment/deadbolt-1Unlock") == 0)) {//} && (strcmp(event->data, "Lock") == 0)) {
                ESP_LOGI(TAG, "UNLOCK");
                __STOP = 0;
                gpio_set_level(DIR_PIN, 0);
                while(gpio_get_level(LIMIT1) == 1 && gpio_get_level(LIMIT2) == 1) {
                    ESP_LOGI(TAG, "PULSE");
                    gpio_set_level(STEP_PIN,1);
                    vTaskDelay(10/ portTICK_PERIOD_MS); 
                    gpio_set_level(STEP_PIN,0);
                    vTaskDelay(10/ portTICK_PERIOD_MS);
                }
                gpio_set_level(DIR_PIN, 1);
                for(int i = 0; i < 20; i++) {
                    ESP_LOGI(TAG, "PULSE");
                    gpio_set_level(STEP_PIN,1);
                    vTaskDelay(10/ portTICK_PERIOD_MS); 
                    gpio_set_level(STEP_PIN,0);
                    vTaskDelay(10/ portTICK_PERIOD_MS);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

//Define MQTT handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

//Start MQTT client
static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://ixnuhznr:2w64vuqqaboL@driver.cloudmqtt.com:18804",
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
    ESP_LOGI(TAG, "MQTT Client started");
}

//Update globals from SPIFFS
static void readAuth() {
    FILE* f;
    ESP_LOGI(TAG, "Reading file");
    f = fopen("/spiffs/auth.txt", "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "Failed to open file for reading");
        //Create File if missing
        ESP_LOGI(TAG, "Opening file");
        f = fopen("/spiffs/auth.txt", "w");
        fprintf(f, "Hello World!\n");
        fclose(f);
        ESP_LOGI(TAG, "File written");
        return;
    }
    char line[64];
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char* pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Extracting: %s", line);
    strcpy(__SSID, strtok(line, ","));
    strcpy(__PWD, strtok(NULL, ","));
    strcpy(__SYSNAME, strtok(NULL, ","));
    strcpy(__SYSPWD, strtok(NULL, ","));
    ESP_LOGI(TAG, "SSID: %s, PASS: %s, SYSNAME: %s, SYSPWD: %s", __SSID, __PWD, __SYSNAME, __SYSPWD);
    return;
}

//AP handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {           //Client connects
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) { //Client disconnects
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

//STA handler
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        gpio_set_level(LEDR, 1);
        gpio_set_level(LEDG, 1);
        gpio_set_level(LEDB, 0);
    }
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            //.ssid = "voxcaster13",
            //.password = "overunder",
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    for (int i = 0; i < 32; i++) {
         wifi_config.sta.ssid[i] = __SSID[i];
        }
    for (int i = 0; i < 64; i++) {
        wifi_config.sta.password[i] =  __PWD[i];
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 __SSID, __PWD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 __SSID, __PWD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

/*  WiFi connection over AP by Moritz Boesenberg
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
   Accessed at https://github.com/lemmi25/ESP32WiFiAP/blob/master/main/main.c on 3/09/2021
*/

static esp_err_t servePage_get_handler(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html>");

    httpd_resp_sendstr_chunk(req, "<head>");
    httpd_resp_sendstr_chunk(req, "<title>Hub Configuration Portal</title>");
    httpd_resp_sendstr_chunk(req, "</head>");

    httpd_resp_sendstr_chunk(req, "<body>");

    //Header
    httpd_resp_sendstr_chunk(req, "<h1>Hub Configuration Portal</h1>");

    //Declare Form
    httpd_resp_sendstr_chunk(req, "<form class=\"form1\" id=\"loginForm\" action=\"\">");

    //SSID Field
    httpd_resp_sendstr_chunk(req, "<br/><label for=\"SSID\">Wifi SSID: </label>");
    httpd_resp_sendstr_chunk(req, "<input id=\"ssid\" type=\"text\" name=\"ssid\" maxlength=\"64\" minlength=\"4\">");      

    //Password Field, password type adds obscuration
    httpd_resp_sendstr_chunk(req, "<br/><label for=\"Password\">Password: </label>");
    httpd_resp_sendstr_chunk(req, "<input id=\"pwd\" type=\"password\" name=\"pwd\" maxlength=\"64\" minlength=\"4\">");    

    //SSID Field
    httpd_resp_sendstr_chunk(req, "<br/><label for=\"System Login\">System Login: </label>");
    httpd_resp_sendstr_chunk(req, "<input id=\"sysname\" type=\"text\" name=\"sysname\" maxlength=\"64\" minlength=\"4\">");      

    //Password Field, password type adds obscuration
    httpd_resp_sendstr_chunk(req, "<br/><label for=\"System Password\">System Password: </label>");
    httpd_resp_sendstr_chunk(req, "<input id=\"syspwd\" type=\"password\" name=\"syspwd\" maxlength=\"64\" minlength=\"4\">");    

    //Submit Button
    httpd_resp_sendstr_chunk(req, "<br/><button>Submit</button>");
    httpd_resp_sendstr_chunk(req, "</form>");

    //Submit Script: Creates listener on submit button, packs up, and sends responses to 192.168.4.1/connection via the POST command
    httpd_resp_sendstr_chunk(req, "<script>");
    httpd_resp_sendstr_chunk(req, "document.getElementById(\"loginForm\").addEventListener(\"submit\", (e) => {e.preventDefault(); const formData = new FormData(e.target); const data = Array.from(formData.entries()).reduce((memo, pair) => ({...memo, [pair[0]]: pair[1],  }), {}); var xhr = new XMLHttpRequest(); xhr.open(\"POST\", \"http://192.168.4.1/connection\", true); xhr.setRequestHeader('Content-Type', 'application/json'); xhr.send(JSON.stringify(data)); document.getElementById(\"output\").innerHTML = JSON.stringify(data);});");
    httpd_resp_sendstr_chunk(req, "</script>");

    httpd_resp_sendstr_chunk(req, "</body></html>");

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

//URI: "/"
static const httpd_uri_t servePage = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = servePage_get_handler,
    .user_ctx = NULL};

static esp_err_t psw_ssid_get_handler(httpd_req_t *req)
{
    char buf[128];
    int ret, remaining = req->content_len;

    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                                  MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == 0)
            {
                ESP_LOGI(TAG, "No content received please try again ...");
            }
            else if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {

                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Log data received */
        /* ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "===================================="); */
        cJSON *root = cJSON_Parse(buf);

        sprintf(__SSID, "%s", cJSON_GetObjectItem(root, "ssid")->valuestring);
        sprintf(__PWD, "%s", cJSON_GetObjectItem(root, "pwd")->valuestring);
        sprintf(__SYSNAME, "%s", cJSON_GetObjectItem(root, "sysname")->valuestring);
        sprintf(__SYSPWD, "%s", cJSON_GetObjectItem(root, "syspwd")->valuestring);

        ESP_LOGI(TAG, "pwd: %s", __PWD);
        ESP_LOGI(TAG, "ssid: %s", __SSID);
        ESP_LOGI(TAG, "sysname: %s", __SYSNAME);
        ESP_LOGI(TAG, "syspwd: %s", __SYSPWD);
        
        remaining -= ret;
        
        //Update Auth
        ESP_LOGI(TAG, "Opening file");
        FILE* f = fopen("/spiffs/auth.txt", "w");
        if (f == NULL) {
            ESP_LOGE(TAG, "Failed to open file for writing");
        }
        fprintf(f, "%s,%s,%s,%s", __SSID, __PWD, __SYSNAME, __SYSPWD);
        fclose(f);
        ESP_LOGI(TAG, "WIFI written");
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    //esp_sleep_enable_timer_wakeup(100000);
    //esp_deep_sleep_start();

    
    ESP_ERROR_CHECK(httpd_stop(server_handle));
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    return ESP_OK;
}

//URI: "/connection"
static const httpd_uri_t psw_ssid = {
    .uri = "/connection",
    .method = HTTP_POST,
    .handler = psw_ssid_get_handler,
    .user_ctx = "TEST"};

//Starts HTTP server
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
       // httpd_register_uri_handler(server, &hello);
        httpd_register_uri_handler(server, &servePage);
        httpd_register_uri_handler(server, &psw_ssid);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //Initialize GPIO
    ESP_LOGI(TAG, "GPIO setup");
    //Outputs
    gpio_pad_select_gpio(STEP_PIN);
    gpio_pad_select_gpio(DIR_PIN);
    gpio_pad_select_gpio(LEDR);
    gpio_pad_select_gpio(LEDG);
    gpio_pad_select_gpio(LEDB);

    gpio_set_direction(STEP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(DIR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LEDR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LEDG, GPIO_MODE_OUTPUT);
    gpio_set_direction(LEDB, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "Outputs Configured");

    //Inputs
    gpio_pad_select_gpio(LIMIT1);
    gpio_pad_select_gpio(LIMIT2);
    gpio_pad_select_gpio(SYNC);

    gpio_set_direction(SYNC, GPIO_MODE_INPUT);
    gpio_set_direction(LIMIT1, GPIO_MODE_INPUT);
    gpio_set_direction(LIMIT2, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "Inputs Configured");
    gpio_set_intr_type(LIMIT1, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(LIMIT2, GPIO_INTR_POSEDGE);
    gpio_set_intr_type(SYNC, GPIO_INTR_ANYEDGE);
    
    //create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    ESP_LOGI(TAG, "Configure ISR");
    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pins
    gpio_isr_handler_add(LIMIT1, gpio_isr_handler, (void*) LIMIT1);
    gpio_isr_handler_add(LIMIT2, gpio_isr_handler, (void*) LIMIT2);
    gpio_isr_handler_add(SYNC, gpio_isr_handler, (void*) SYNC);

    ESP_LOGI(TAG, "GPIO Configured");
    

    //Initialize SPIFFS (from idf Examples)
    ESP_LOGI(TAG, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t SPIFFStotal = 0, SPIFFSused = 0;
    ret = esp_spiffs_info(conf.partition_label, &SPIFFStotal, &SPIFFSused);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", SPIFFStotal, SPIFFSused);
    }
    //Read stored credentials
    readAuth();

    //Determine Wifi Mode
    if (gpio_get_level(SYNC) == 1) {
        gpio_set_level(LEDR, 0);
        gpio_set_level(LEDG, 0);
        gpio_set_level(LEDB, 1);
        //Start access point and web server
        ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");
        wifi_init_softap();
        server_handle = start_webserver();
    } else {
        gpio_set_level(LEDR, 1);
        gpio_set_level(LEDG, 0);
        gpio_set_level(LEDB, 0);
        ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
        wifi_init_sta();

        ESP_LOGI(TAG, "init MQTT");
        //MQTT Verbosity
        esp_log_level_set("*", ESP_LOG_INFO);
        esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
        esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
        esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
        esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
        esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
        esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);
        vTaskDelay(1000/ portTICK_PERIOD_MS);
        //Start MQTT
        ESP_LOGI(TAG, "Start MQTT"); 
        mqtt_app_start();
    } 
}
