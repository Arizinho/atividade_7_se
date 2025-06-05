/*
Projeto de um sistema de irrigação inteligente utilizando o protocolo de comunicação MQTT
*/

#include "pico/stdlib.h"            // Biblioteca da Raspberry Pi Pico para funções padrão (GPIO, temporização, etc.)
#include "pico/cyw43_arch.h"        // Biblioteca para arquitetura Wi-Fi da Pico com CYW43
#include "pico/unique_id.h"         // Biblioteca com recursos para trabalhar com os pinos GPIO do Raspberry Pi Pico

//Bibliotecas Gerais Raspberry Pi Pico
#include <stdio.h>
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/adc.h"

//Bibliotecas Display Oled
#include "lib/ssd1306.h"
#include "lib/font.h"

#include "lwip/apps/mqtt.h"         // Biblioteca LWIP MQTT -  fornece funções e recursos para conexão MQTT
#include "lwip/apps/mqtt_priv.h"    // Biblioteca que fornece funções e recursos para Geração de Conexões
#include "lwip/dns.h"               // Biblioteca que fornece funções e recursos suporte DNS:
#include "lwip/altcp_tls.h"         // Biblioteca que fornece funções e recursos para conexões seguras usando TLS:

//Bibliotecas FreeRTOS
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

//arquivo .pio
#include "led_matrix.pio.h"

//definições de portas e pinos
#define I2C_PORT i2c1
#define I2C_SDA 14
#define I2C_SCL 15
#define endereco 0x3C


#define WIFI_SSID "SEU_SSID"                  // Substitua pelo nome da sua rede Wi-Fi
#define WIFI_PASSWORD "SEU_PASSORD_WIFI"      // Substitua pela senha da sua rede Wi-Fi
#define MQTT_SERVER "SEU_HOST"                // Substitua pelo endereço do host - broket MQTT: Ex: 192.168.1.107
#define MQTT_USERNAME "SEU_USERNAME_MQTT"     // Substitua pelo nome da host MQTT - Username
#define MQTT_PASSWORD "SEU_PASSWORD_MQTT"     // Substitua pelo Password da host MQTT - credencial de acesso - caso exista

#ifndef MQTT_SERVER
#error Need to define MQTT_SERVER
#endif

// This file includes your client certificate for client server authentication
#ifdef MQTT_CERT_INC
#include MQTT_CERT_INC
#endif

#ifndef MQTT_TOPIC_LEN
#define MQTT_TOPIC_LEN 100
#endif

//Dados do cliente MQTT
typedef struct {
    mqtt_client_t* mqtt_client_inst;
    struct mqtt_connect_client_info_t mqtt_client_info;
    char data[MQTT_OUTPUT_RINGBUF_SIZE];
    char topic[MQTT_TOPIC_LEN];
    uint32_t len;
    ip_addr_t mqtt_server_address;
    bool connect_done;
    int subscribe_count;
    bool stop_client;
} MQTT_CLIENT_DATA_T;

#ifndef DEBUG_printf
#ifndef NDEBUG
#define DEBUG_printf printf
#else
#define DEBUG_printf(...)
#endif
#endif

#ifndef INFO_printf
#define INFO_printf printf
#endif

#ifndef ERROR_printf
#define ERROR_printf printf
#endif

// Temporização da coleta de temperatura - how often to measure our temperature
#define TEMP_WORKER_TIME_S 1

// Manter o programa ativo - keep alive in seconds
#define MQTT_KEEP_ALIVE_S 60

// QoS - mqtt_subscribe
// At most once (QoS 0)
// At least once (QoS 1)
// Exactly once (QoS 2)
#define MQTT_SUBSCRIBE_QOS 1
#define MQTT_PUBLISH_QOS 1
#define MQTT_PUBLISH_RETAIN 0

// Tópico usado para: last will and testament
#define MQTT_WILL_TOPIC "/online"
#define MQTT_WILL_MSG "0"
#define MQTT_WILL_QOS 1

#ifndef MQTT_DEVICE_NAME
#define MQTT_DEVICE_NAME "pico"
#endif

// Definir como 1 para adicionar o nome do cliente aos tópicos, para suportar vários dispositivos que utilizam o mesmo servidor
#ifndef MQTT_UNIQUE_TOPIC
#define MQTT_UNIQUE_TOPIC 0
#endif

//Pinos dos periféricos utilizados
#define LED_BLUE 12
#define BUZZER 10
#define OUT_PIN 7 //GPIO matriz de leds
#define ADC_JOYSTICK_X 26
#define ADC_JOYSTICK_Y 27

//estrutura para armazenar dados dos sensores
typedef struct
{
    uint8_t nivel_reservatorio;   //nível da água (%)
    uint8_t umidade;              //umidade (%)
} sensor_data_t;

//Protótipos 
static void pub_request_cb(__unused void *arg, err_t err);
static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name);
static void sub_request_cb(void *arg, err_t err);
static void unsub_request_cb(void *arg, err_t err);
static void sub_unsub_topics(MQTT_CLIENT_DATA_T* state, bool sub);
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len);
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
static void start_client(MQTT_CLIENT_DATA_T *state);
static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg);
uint32_t matrix_rgb(uint8_t b, uint8_t r, uint8_t g);
void desenho_pio(bool estado, PIO pio, uint sm);
void buzzer_setup();

//Variáveis Globais
volatile uint8_t umidade_minima = 20;       //Umidade mínima para ligar bomba (%)
volatile uint8_t tempo_irrigacao = 5;       //Tempo de duração da irrigação   (s)
volatile bool auto_on = 0;                  //Flag para indicar se modo automático está on (1 caso esteja)
volatile bool parada = 0;                   //Flag para interrupção de bomba (1 para indicar parada)

//xQueueSensorData: fila de dados lidos dos sensores pelo ADC (sensordata)
QueueHandle_t xQueueSensorData;

//Semáforos para controle de fluxo de tarefas
SemaphoreHandle_t xAcionaBombaSem;
SemaphoreHandle_t xDisplayMutex;

//Cliente global para publicação de dados no broker pela tarefa vSensorTask 
mqtt_client_t *mqtt_client_global = NULL;

//Tarefa para realizar leitura dos sensores (joystick) de umidade ("sensordata.umidade") e nível de reservatório ("sensordata.nivel_reservatorio")
void vSensorTask(void *params)
{
    //inicializa ADC
    adc_gpio_init(ADC_JOYSTICK_Y);
    adc_gpio_init(ADC_JOYSTICK_X);
    adc_init();

    sensor_data_t sensordata;
    uint32_t last_time = 0;
    while (true)
    {
        adc_select_input(0); // GPIO 26 = ADC0
        sensordata.umidade = adc_read()*(100.2f/4096.2f); //converte valor lido para faixa de 0 a 100

        adc_select_input(1); // GPIO 27 = ADC1
        sensordata.nivel_reservatorio = adc_read()*(100.2f/4096.2f); //converte valor lido para faixa de 0 a 100

        //Envia o valor dos sensores para a fila "xQueueSensorData"
        xQueueSend(xQueueSensorData, &sensordata, 0);  
        
        //Publica umidade e nivel do reservatório no broker
        if (mqtt_client_global && mqtt_client_is_connected(mqtt_client_global) && (to_ms_since_boot(get_absolute_time())-last_time) > 2000) {
            last_time = to_ms_since_boot(get_absolute_time());
            char buffer[8];

            //Publica umidade
            snprintf(buffer, sizeof(buffer), "%u", sensordata.umidade);
            mqtt_publish(mqtt_client_global, "/dados/umidade", buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, NULL);

            //Publica nível do reservatório
            snprintf(buffer, sizeof(buffer), "%u", sensordata.nivel_reservatorio);
            mqtt_publish(mqtt_client_global, "/dados/nivel", buffer, strlen(buffer), MQTT_PUBLISH_QOS, MQTT_PUBLISH_RETAIN, pub_request_cb, NULL);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

//Tarefa com função de configurar e manter conexão via protocolo MQTT
void vMQTTConexaoTask(void *params){
    INFO_printf("mqtt client starting\n");

    // Cria registro com os dados do cliente
    static MQTT_CLIENT_DATA_T state;

    // Inicializa a arquitetura do cyw43
    if (cyw43_arch_init()) {
        panic("Failed to inizialize CYW43");
    }

    // Usa identificador único da placa
    char unique_id_buf[5];
    pico_get_unique_board_id_string(unique_id_buf, sizeof(unique_id_buf));
    for(int i=0; i < sizeof(unique_id_buf) - 1; i++) {
        unique_id_buf[i] = tolower(unique_id_buf[i]);
    }

    // Gera nome único, Ex: pico1234
    char client_id_buf[sizeof(MQTT_DEVICE_NAME) + sizeof(unique_id_buf) - 1];
    memcpy(&client_id_buf[0], MQTT_DEVICE_NAME, sizeof(MQTT_DEVICE_NAME) - 1);
    memcpy(&client_id_buf[sizeof(MQTT_DEVICE_NAME) - 1], unique_id_buf, sizeof(unique_id_buf) - 1);
    client_id_buf[sizeof(client_id_buf) - 1] = 0;
    INFO_printf("Device name %s\n", client_id_buf);

    state.mqtt_client_info.client_id = client_id_buf;
    state.mqtt_client_info.keep_alive = MQTT_KEEP_ALIVE_S; // Keep alive in sec
#if defined(MQTT_USERNAME) && defined(MQTT_PASSWORD)
    state.mqtt_client_info.client_user = MQTT_USERNAME;
    state.mqtt_client_info.client_pass = MQTT_PASSWORD;
#else
    state.mqtt_client_info.client_user = NULL;
    state.mqtt_client_info.client_pass = NULL;
#endif
    static char will_topic[MQTT_TOPIC_LEN];
    strncpy(will_topic, full_topic(&state, MQTT_WILL_TOPIC), sizeof(will_topic));
    state.mqtt_client_info.will_topic = will_topic;
    state.mqtt_client_info.will_msg = MQTT_WILL_MSG;
    state.mqtt_client_info.will_qos = MQTT_WILL_QOS;
    state.mqtt_client_info.will_retain = true;
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    // TLS enabled
#ifdef MQTT_CERT_INC
    static const uint8_t ca_cert[] = TLS_ROOT_CERT;
    static const uint8_t client_key[] = TLS_CLIENT_KEY;
    static const uint8_t client_cert[] = TLS_CLIENT_CERT;
    // This confirms the indentity of the server and the client
    state.mqtt_client_info.tls_config = altcp_tls_create_config_client_2wayauth(ca_cert, sizeof(ca_cert),
            client_key, sizeof(client_key), NULL, 0, client_cert, sizeof(client_cert));
#if ALTCP_MBEDTLS_AUTHMODE != MBEDTLS_SSL_VERIFY_REQUIRED
    WARN_printf("Warning: tls without verification is insecure\n");
#endif
#else
    state->client_info.tls_config = altcp_tls_create_config_client(NULL, 0);
    WARN_printf("Warning: tls without a certificate is insecure\n");
#endif
#endif

    // Conectar à rede WiFI - fazer um loop até que esteja conectado
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        panic("Failed to connect");
    }
    INFO_printf("\nConnected to Wifi\n");

    //Faz um pedido de DNS para o endereço IP do servidor MQTT
    cyw43_arch_lwip_begin();
    int err = dns_gethostbyname(MQTT_SERVER, &state.mqtt_server_address, dns_found, &state);
    cyw43_arch_lwip_end();

    // Se tiver o endereço, inicia o cliente
    if (err == ERR_OK) {
        start_client(&state);
    } else if (err != ERR_INPROGRESS) { // ERR_INPROGRESS means expect a callback
        panic("dns request failed");
    }

    mqtt_client_global = state.mqtt_client_inst;

    // Loop condicionado a conexão mqtt
     while (!state.connect_done || mqtt_client_is_connected(state.mqtt_client_inst)) {
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(10000));
    }

    INFO_printf("mqtt client exiting\n");
    vTaskDelete(NULL);
}

/*
Tarefa para realizar acionamento de bomba - representada pelo LED azul
Aguarda semáforo binário ("xAcionaBombaSem") para realizar ativação da bomba

*/
void vAcionaBombaTask(void *params){
    //Inicializa led azul
    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);
    gpio_put(LED_BLUE, 0);

    uint32_t last_time = 0;
    uint8_t tempo_irr = 1;
    sensor_data_t sensordata;

    while(true){
        //Guarda leitura dos sensores da fila para desligamento de bomba em caso de nível < 10%
        xQueueReceive(xQueueSensorData, &sensordata, portMAX_DELAY);

        //aguarda chamada de semáforo para ativar bomba
        if (xSemaphoreTake(xAcionaBombaSem, 0) == pdTRUE) {
            if(sensordata.nivel_reservatorio >= 10){
                last_time = to_ms_since_boot(get_absolute_time());
                tempo_irr = tempo_irrigacao;
                gpio_put(LED_BLUE, 1);
            }  
        }

        //Desligamento de bomba: (1) Tempo de irrigação atingido ou (2) Parada solicitada ou (3) Nível de reservatório < 10%
        if ((to_ms_since_boot(get_absolute_time()) - last_time > tempo_irr*1000) || parada == 1 || sensordata.nivel_reservatorio < 10){
            gpio_put(LED_BLUE, 0);
            parada = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
Função para controle de bomba em modo automático ON
Liga bomba em caso de Umidade Lida (sensordata.umidade) < Umidade Mínima Configurada (umidade_minima)
*/
void vControlePorUmidadeTask(void *params){
    sensor_data_t sensordata;
    uint32_t last_time = 0;
    while(true){
        //Espera modo auto on ativo e leitura da fila para ativar bomba
        if(auto_on && xQueueReceive(xQueueSensorData, &sensordata, portMAX_DELAY) == pdTRUE){
            if(sensordata.umidade < umidade_minima && (to_ms_since_boot(get_absolute_time()) - last_time) > tempo_irrigacao*1000){
                last_time = to_ms_since_boot(get_absolute_time());

                //Ativa semáfora binário para ligação da bomba
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;  //nenhum contexto de tarefa foi despertado
                xSemaphoreGiveFromISR(xAcionaBombaSem, &xHigherPriorityTaskWoken);    //entrada de usuário
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken); //troca o contexto da tarefa
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
Tarefa para acionamento de alertas em caso de nível de reservatório < 10%
Mostra alerta na matriz de leds e aviso auditivo
*/
void vAlertaNivelTask(void *params){
    sensor_data_t sensordata;
    bool estado = 0;

    //inicializa buzzer
    buzzer_setup();

    //inicialização matriz de leds
    PIO pio = pio0; //seleciona a pio0
    uint offset = pio_add_program(pio, &pio_matrix_program);
    uint sm = pio_claim_unused_sm(pio, true);
    pio_matrix_program_init(pio, sm, offset, OUT_PIN);

    while(1){
        //Aguarda fila de dados dos sensores
        if(xQueueReceive(xQueueSensorData, &sensordata, portMAX_DELAY)==pdTRUE){
            //Ativa alertas: nivel_reservatorio < 10 %
            if (sensordata.nivel_reservatorio < 10){
                estado = 1;
                pwm_set_gpio_level(BUZZER, 1000);
                vTaskDelay(pdMS_TO_TICKS(100));
                pwm_set_gpio_level(BUZZER, 0);
                vTaskDelay(pdMS_TO_TICKS(100));   
            }
            //Desativa alertas 
            else{
                estado = 0;
                pwm_set_gpio_level(BUZZER, 0);
            }
                desenho_pio(estado, pio, sm); 
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/*
Tarefa para mostrar informações dos sensores e alertas no display OLED
*/
void vTaskDisplay(void *params) {
    bool estado;
    sensor_data_t sensordata;

    char buffer1[4], buffer2[4];

    // I2C Initialisation. Using it at 400Khz.
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);                    // Set the GPIO pin function to I2C
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);                    // Set the GPIO pin function to I2C
    gpio_pull_up(I2C_SDA);                                        // Pull up the data line
    gpio_pull_up(I2C_SCL);                                        // Pull up the clock line
    ssd1306_t ssd;                                                // Inicializa a estrutura do display
    ssd1306_init(&ssd, WIDTH, HEIGHT, false, endereco, I2C_PORT); // Inicializa o display
    ssd1306_config(&ssd);                                         // Configura o display
    ssd1306_send_data(&ssd);                                      // Envia os dados para o display
    // Limpa o display. O display inicia com todos os pixels apagados.
    ssd1306_fill(&ssd, false);
    ssd1306_send_data(&ssd);
    
    while (true) {
        if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY)==pdTRUE) {
            xQueueReceive(xQueueSensorData, &sensordata, portMAX_DELAY);
            if(sensordata.nivel_reservatorio < 10){
                ssd1306_fill(&ssd, false);                          // Limpa o display
                ssd1306_line(&ssd, 0, 22, 123, 22, 1);
                ssd1306_draw_string(&ssd, "ALERTA!!!", 30, 2); // Desenha uma string
                ssd1306_draw_string(&ssd, "Falta de Agua", 2, 13);          
            }
            else{
                ssd1306_fill(&ssd, false);
                ssd1306_draw_string(&ssd, "Leitura Sensors", 2, 2); // Desenha uma string
            }
                ssd1306_rect(&ssd, 0, 0, 124, 64, 1, 0);
                ssd1306_line(&ssd, 0, 11, 123, 11, 1);

                ssd1306_draw_string(&ssd, "Nvl", 2, 30);
                ssd1306_draw_string(&ssd, "de", 30, 30);
                ssd1306_draw_string(&ssd, "agua", 50, 30);
                ssd1306_draw_string(&ssd, ":", 80, 30);

                ssd1306_draw_string(&ssd, "Umidade:", 2, 45);

                sprintf(buffer1, "%u", sensordata.nivel_reservatorio);
                sprintf(buffer2, "%u", sensordata.umidade);
                ssd1306_draw_string(&ssd, buffer1, 92, 30); // Desenha uma string
                ssd1306_draw_string(&ssd, buffer2, 92, 45); // Desenha uma string
                ssd1306_send_data(&ssd);

                xSemaphoreGive(xDisplayMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

//rotina para inicializar buzzer com PWM
void buzzer_setup(){
    gpio_set_function(BUZZER, GPIO_FUNC_PWM);
    pwm_set_wrap(pwm_gpio_to_slice_num(BUZZER), 1999);
    pwm_set_clkdiv(pwm_gpio_to_slice_num(BUZZER), 125);
    pwm_set_enabled(pwm_gpio_to_slice_num(BUZZER), true);
    pwm_set_gpio_level(BUZZER, 0);
}

//rotina para definição da intensidade de cores do led na matriz 5x5
uint32_t matrix_rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (g << 24) | (r << 16) | (b << 8);
}

//rotina para acionar a matrix de leds - ws2812b
void desenho_pio(bool estado, PIO pio, uint sm){
    for (int16_t i = 0; i < 25; i++) {
        if (estado){
            (i%5 == 2 && i != 7) ? pio_sm_put_blocking(pio, sm, matrix_rgb(5, 5, 5)) : pio_sm_put_blocking(pio, sm, matrix_rgb(15, 0, 0));
        }
        else {
            pio_sm_put_blocking(pio, sm, matrix_rgb(0, 0, 0));
        }
    }
}

// Requisição para publicar
static void pub_request_cb(__unused void *arg, err_t err) {
    if (err != 0) {
        ERROR_printf("pub_request_cb failed %d", err);
    }
}

//Topico MQTT
static const char *full_topic(MQTT_CLIENT_DATA_T *state, const char *name) {
#if MQTT_UNIQUE_TOPIC
    static char full_topic[MQTT_TOPIC_LEN];
    snprintf(full_topic, sizeof(full_topic), "/%s%s", state->mqtt_client_info.client_id, name);
    return full_topic;
#else
    return name;
#endif
}

// Requisição de Assinatura - subscribe
static void sub_request_cb(void *arg, err_t err) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (err != 0) {
        panic("subscribe request failed %d", err);
    }
    state->subscribe_count++;
}

// Requisição para encerrar a assinatura
static void unsub_request_cb(void *arg, err_t err) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (err != 0) {
        panic("unsubscribe request failed %d", err);
    }
    state->subscribe_count--;
    assert(state->subscribe_count >= 0);

    // Stop if requested
    if (state->subscribe_count <= 0 && state->stop_client) {
        mqtt_disconnect(state->mqtt_client_inst);
    }
}

// Tópicos de assinatura
static void sub_unsub_topics(MQTT_CLIENT_DATA_T* state, bool sub) {
    mqtt_request_cb_t cb = sub ? sub_request_cb : unsub_request_cb;
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/irrigacao/umidade_min"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/irrigacao/auto_on"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/irrigacao/tempo"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/irrigacao/turn_on"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/irrigacao/parada"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
    mqtt_sub_unsub(state->mqtt_client_inst, full_topic(state, "/exit"), MQTT_SUBSCRIBE_QOS, cb, state, sub);
}

// Dados de entrada publicados
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    strncpy(state->topic, topic, sizeof(state->topic));
}

// Conexão MQTT
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        state->connect_done = true;
        sub_unsub_topics(state, true); // subscribe;

        // indicate online
        if (state->mqtt_client_info.will_topic) {
            mqtt_publish(state->mqtt_client_inst, state->mqtt_client_info.will_topic, "1", 1, MQTT_WILL_QOS, true, pub_request_cb, state);
        }

    } else if (status == MQTT_CONNECT_DISCONNECTED) {
        if (!state->connect_done) {
            panic("Failed to connect to mqtt server");
        }
    }
    else {
        panic("Unexpected status");
    }
}

// Inicializar o cliente MQTT
static void start_client(MQTT_CLIENT_DATA_T *state) {
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    const int port = MQTT_TLS_PORT;
    INFO_printf("Using TLS\n");
#else
    const int port = MQTT_PORT;
    INFO_printf("Warning: Not using TLS\n");
#endif

    state->mqtt_client_inst = mqtt_client_new();
    if (!state->mqtt_client_inst) {
        panic("MQTT client instance creation error");
    }
    INFO_printf("IP address of this device %s\n", ipaddr_ntoa(&(netif_list->ip_addr)));
    INFO_printf("Connecting to mqtt server at %s\n", ipaddr_ntoa(&state->mqtt_server_address));

    cyw43_arch_lwip_begin();
    if (mqtt_client_connect(state->mqtt_client_inst, &state->mqtt_server_address, port, mqtt_connection_cb, state, &state->mqtt_client_info) != ERR_OK) {
        panic("MQTT broker connection error");
    }
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    // This is important for MBEDTLS_SSL_SERVER_NAME_INDICATION
    mbedtls_ssl_set_hostname(altcp_tls_context(state->mqtt_client_inst->conn), MQTT_SERVER);
#endif
    mqtt_set_inpub_callback(state->mqtt_client_inst, mqtt_incoming_publish_cb, mqtt_incoming_data_cb, state);
    cyw43_arch_lwip_end();
}

// Call back com o resultado do DNS
static void dns_found(const char *hostname, const ip_addr_t *ipaddr, void *arg) {
    MQTT_CLIENT_DATA_T *state = (MQTT_CLIENT_DATA_T*)arg;
    if (ipaddr) {
        state->mqtt_server_address = *ipaddr;
        start_client(state);
    } else {
        panic("dns request failed");
    }
}

// Dados de entrada MQTT
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    MQTT_CLIENT_DATA_T* state = (MQTT_CLIENT_DATA_T*)arg;
#if MQTT_UNIQUE_TOPIC
    const char *basic_topic = state->topic + strlen(state->mqtt_client_info.client_id) + 1;
#else
    const char *basic_topic = state->topic;
#endif
    strncpy(state->data, (const char *)data, len);
    state->len = len;
    state->data[len] = '\0';

    DEBUG_printf("Topic: %s, Message: %s\n", state->topic, state->data);

    //Altera nível mínimo de umidade para irrigação
    if (strcmp(basic_topic, "/irrigacao/umidade_min") == 0) {
        uint8_t valor = atoi(state->data);
        if (valor >= 0 && valor <= 100) {
            umidade_minima = valor;
        }
    }
    
    //Ativa/destiva modo automático
    else if (strcmp(basic_topic, "/irrigacao/auto_on") == 0) {
        if (lwip_stricmp((const char *)state->data, "On") == 0 || strcmp((const char *)state->data, "1") == 0){
            auto_on = 1;
        }
        else if (lwip_stricmp((const char *)state->data, "Off") == 0 || strcmp((const char *)state->data, "0") == 0){
            auto_on = 0;
        }
    } 
    
    //Altera tempo de duração da irrigação
    else if (strcmp(basic_topic, "/irrigacao/tempo") == 0) {
        uint8_t tempo = atoi(state->data);
        if (tempo > 1 && tempo < 60) {
            tempo_irrigacao = tempo;
        }
    }

    //Ativa bomba manualmente
    else if (strcmp(basic_topic, "/irrigacao/turn_on") == 0) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;  //nenhum contexto de tarefa foi despertado
        xSemaphoreGiveFromISR(xAcionaBombaSem, &xHigherPriorityTaskWoken);    //entrada de usuário
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken); //troca o contexto da tarefa
    }

    //Parada forçada da bomba
    else if (strcmp(basic_topic, "/irrigacao/parada") == 0) {
        parada = 1;
    }
            
    else if (strcmp(basic_topic, "/exit") == 0) {
        state->stop_client = true; // stop the client when ALL subscriptions are stopped
        sub_unsub_topics(state, false); // unsubscribe
    }
}

int main(void) {

    // Inicializa todos os tipos de bibliotecas stdio padrão presentes que estão ligados ao binário.
    stdio_init_all();

    //cria a fila para compartilhamento de valor dos "sensores"
    xQueueSensorData = xQueueCreate(5, sizeof(sensor_data_t));

    //cria semáforos
    xDisplayMutex = xSemaphoreCreateMutex();
    xAcionaBombaSem = xSemaphoreCreateBinary();

    //cria tarefas
    xTaskCreate(vSensorTask, "Leitura de Sensores Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
    xTaskCreate(vMQTTConexaoTask, "Conexão MQTT Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);  
    xTaskCreate(vAcionaBombaTask, "Acionamento Bomba Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
    xTaskCreate(vControlePorUmidadeTask, "Controle por Umidade Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL); 
    xTaskCreate(vAlertaNivelTask, "Alerta de Nivel Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);
    xTaskCreate(vTaskDisplay, "Display de Dados Task", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY, NULL);

    vTaskStartScheduler();
    panic_unsupported();
}