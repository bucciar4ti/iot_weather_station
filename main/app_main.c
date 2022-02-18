#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include <dht.h>
//#include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "bmp280.h"

#define topics_pub "v1/devices/me/telemetry"
#define broker "mqtt://Dn27mxNlvr1KrT6PuSUJ:NULL@demo.thingsboard.io:1883"
//Dn27mxNlvr1KrT6PuSUJ
static const char *TAG = "MQTT_WeatherStation_Thingsboard";

TimerHandle_t hall_timer;
TimerHandle_t uv_timer;
QueueHandle_t wind_q;
QueueHandle_t windspeed_q;
QueueHandle_t bme280_t;
QueueHandle_t bme280_p;
QueueHandle_t bme280_h;
QueueHandle_t uv_q;
int counter = 0;
int counter1 = 0;
bool last_state = 0;
bool current_state = 0;
int raw_out;

#define ESP_CORE_0 0
#define ESP_CORE_1 1

#define SDA_GPIO 21
#define SCL_GPIO 22
static const dht_sensor_type_t sensor_type = DHT_TYPE_AM2301;
static const char *TAG1 = "wifi station";
/*WIFI configuration*/
#define ESP_WIFI_SSID      "YC Sopheak JR"
#define ESP_WIFI_PASS      "00000000"
#define ESP_MAXIMUM_RETRY  10
#define LED 2
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static bmp280_t dev;
esp_mqtt_client_config_t mqtt_cfg = {
			        .uri = broker,
			    };
static esp_mqtt_client_handle_t client;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
char Datapub[128];
static const gpio_num_t dht_gpio = 33;
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG1, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG1,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG1, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
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
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG1, "wifi_init_sta finished.");

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
        ESP_LOGI(TAG1, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
        gpio_set_level(LED, 1);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG1, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
        gpio_set_level(LED, 0);
    } else {
        ESP_LOGE(TAG1, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void DHT22_Task(void *pvParameters)
{
    int16_t temperature = 0;
    int16_t humidity;
    while (1)
    {
        if (dht_read_data(sensor_type, dht_gpio, &humidity, &temperature) == ESP_OK){
            printf("Humidity: %d%% Temp: %dC\n", humidity / 10, temperature / 10);
            humidity = humidity / 10;
        xQueueSend(bme280_h, &humidity, 0);}
        else
            printf("Could not read data from sensor\n");


        vTaskDelay(pdMS_TO_TICKS(10000));
    }

}
void BME280_Task(void *pvParameters)
{
	float pressure, temperature, humidity;
		 while (1) {
			 bmp280_read_float(&dev, &temperature, &pressure, &humidity);
			 printf("%.2f Pa, %.2f C\n", pressure, temperature);
			 xQueueSend(bme280_t, &temperature, 0);
			 xQueueSend(bme280_p, &pressure, 0);
			// xQueueSend(bme280_h, &humidity, 0);
			 vTaskDelay(pdMS_TO_TICKS(10000));
		    }
}
void UV_Task(void *pvParameters)
{
	 while (1) {
		 int uvLevel = 0;
		for (int x = 0 ; x < 12 ; x++){
			raw_out = adc1_get_raw(ADC_CHANNEL_0);
		 	    uvLevel += raw_out;
		}
		 	 	 uvLevel /= 12;
		 	   float outputVoltage = uvLevel * 3.3/4095;
		 	   float uvIntensity = 8.29 * (0.99-outputVoltage);
		 	   printf("uvIntensity: %f mW/cm^2 \n", uvIntensity);
			xQueueSend(uv_q, &uvIntensity, 0);
			vTaskDelay(pdMS_TO_TICKS(10000));
		    }
}
void wind_speed(void *pvParameters)
{
	int round;
		 while (1) {
		      xQueueReceive(wind_q, &round, portMAX_DELAY);
		      float rps = (float)round / 10.00;
		      float winds = rps * 0.16 * 3.141592653589 ;
		      printf("wind speed %f m/s\n", winds);
		      xQueueSend(windspeed_q, &winds, 0);

		     vTaskDelay(10 / portTICK_PERIOD_MS);
		    }
}
void hall_timer_cb(TimerHandle_t s)
{
	if (counter1 < 200){
	if (gpio_get_level(4) == 1 && last_state == 0){
    	 counter++;
    	 last_state = 1;


     }
     else if (gpio_get_level(4) == 0 && last_state == 1){
    	 last_state = 0;

     }

	}
	else if (counter1 == 200){
		int round = counter;
		counter = 0;
		counter1 = 0;
		xQueueSend(wind_q, &round, 0);
		return;
	}

	counter1++;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
        switch (event->event_id) {
            case MQTT_EVENT_CONNECTED:
                ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

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

                break;
            case MQTT_EVENT_ERROR:
                ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
                break;
            default:
                ESP_LOGI(TAG, "Other event id:%d", event->event_id);
                break;
        }
}
void PublishData(void *pvParameter){
	float pressure, temperature;
	uint16_t humidity;
	float uvIntensity;
	float winds;
	while(1){
		xQueueReceive(windspeed_q, &winds, portMAX_DELAY);
		xQueueReceive(bme280_t, &temperature, portMAX_DELAY);
		xQueueReceive(bme280_p, &pressure, portMAX_DELAY);
		xQueueReceive(bme280_h, &humidity, portMAX_DELAY);
		xQueueReceive(uv_q, &uvIntensity, portMAX_DELAY);


		vTaskDelay(pdMS_TO_TICKS(1000));

		  			char bufT[10];
		  			char bufP[10];
		  			char bufH[10];
		  			char bufInt[10];
		  			char bufW[10];
		  			  sprintf(bufT,"%.2f",temperature);
		  			  sprintf(bufP,"%.2f",pressure);
		  			  sprintf(bufH,"%d",humidity);
		  			  sprintf(bufInt,"%f",uvIntensity);
		  		    	sprintf(bufW,"%f",winds);
		  			    strcpy(Datapub,"{\"Temperature\":");
		  			    strcat(Datapub,bufT);
		  			    strcat(Datapub,",\"Pressure\":");
		  			    strcat(Datapub,bufP);
		  			    strcat(Datapub,",\"Humidity\":");
		  			    strcat(Datapub,bufH);
		  			    strcat(Datapub,",\"uvIntensity\":");
		  			    strcat(Datapub,bufInt);
		  			    strcat(Datapub,",\"Windspeed\":");
		  			  	strcat(Datapub,bufW);
		  			    strcat(Datapub,"}");
		  			      printf("Data: %s\n", Datapub);
  		if (MQTT_EVENT_CONNECTED){
  		esp_mqtt_client_publish(client,topics_pub,Datapub, 0, 1, 0);
  	    ESP_LOGI(TAG, "sent publish successful");
  		vTaskDelay(pdMS_TO_TICKS(5000));
  		memset(Datapub, 0, sizeof Datapub);
  		}else{
  			esp_mqtt_client_reconnect(client);
  			esp_mqtt_client_start(client);

  		}

  }
}
void mqtt_app_start(void)
{
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
    }

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    //Change it the pin that has a led
	gpio_pad_select_gpio(LED);
	gpio_set_direction(LED, GPIO_MODE_OUTPUT);

	  ESP_LOGI(TAG1, "ESP_WIFI_MODE_STA");
	    wifi_init_sta();
    /*initial i2c and bme280*/
    	 i2cdev_init();
    	  memset(&dev, 0, sizeof(bmp280_t));
    	  bmp280_params_t params;
    	  bmp280_init_default_params(&params);
    	  bmp280_init_desc(&dev, BMP280_I2C_ADDRESS_0, 0 ,SDA_GPIO, SCL_GPIO);
    	  bmp280_init(&dev, &params);
    	     /*configure gpio as input*/
    			gpio_config_t io_conf;
    			io_conf.intr_type = GPIO_INTR_DISABLE;
    	    	io_conf.mode = GPIO_MODE_INPUT;
    	    	io_conf.pin_bit_mask = 1ULL << 4;
    	    	io_conf.pull_up_en = 1;
    	    	io_conf.pull_down_en = 0;
    	    	gpio_config(&io_conf);
    	    	/*adc2 configure*/

    	    	adc1_config_width(ADC_WIDTH_BIT_12);
    	    	adc1_config_channel_atten(ADC_CHANNEL_0, ADC_ATTEN_DB_11);
    	    	/*queue create*/
    	    	wind_q = xQueueCreate(1, sizeof(int));
    	    	windspeed_q = xQueueCreate(1, sizeof(float));
    	    	bme280_t = xQueueCreate(1, sizeof(float));
    	    	bme280_p = xQueueCreate(1, sizeof(float));
    	    	bme280_h = xQueueCreate(1, sizeof(float));
    	    	uv_q = xQueueCreate(1, sizeof(float));
    	    	mqtt_app_start();
    	    	/*task create*/
    	    	xTaskCreatePinnedToCore(&wind_speed,
    	    		    				"wind Task",
    	    		    				2048,
    	    		    				NULL,
    	    		    				3,
    	    		    				NULL,
    	    		    				1);
xTaskCreatePinnedToCore(&PublishData,"Publish Data to Thingsboard", 2048, NULL, 4,NULL, 1);
xTaskCreatePinnedToCore(&UV_Task,"Publish Data to Thingsboard", 2048, NULL, 2,NULL, 1);
xTaskCreatePinnedToCore(&BME280_Task,"Publish Data to Thingsboard", 2048, NULL, 2,NULL, 1);
xTaskCreatePinnedToCore(&DHT22_Task,"Publish Data to Thingsboard", 2048, NULL, 2,NULL, 1);
    	    	/*timer create*/
    	    	hall_timer = xTimerCreate("Hall Sensor",
    	    	            pdMS_TO_TICKS(50),
    	    	            pdTRUE,
    	    	            NULL,
    						hall_timer_cb);

    	    	xTimerStart(hall_timer, 0);


}
