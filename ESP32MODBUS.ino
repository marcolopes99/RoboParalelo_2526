#include <WiFi.h>
#include <ThingerESP32.h>
#include <ModbusIP_ESP8266.h>

// ===================== CONFIGURAÇÕES =====================
#define USERNAME "marcolopes1993"
#define DEVICE_ID "esp32_hmi"
#define DEVICE_CREDENTIAL "915531736"
#define SSID "Iphones"
#define SSID_PASSWORD "935774275"

ThingerESP32 thing(USERNAME, DEVICE_ID, DEVICE_CREDENTIAL);
ModbusIP mb; 

// --- ENDEREÇOS MODBUS ---
const int ENDERECO_ESTADO = 100; 
const int COIL_OK         = 0;   
const int COIL_NOK        = 1;   

/* ===================== HARDWARE ===================== */
#define LED_DEBUG       15  

#define SR_OUT_DATA     13 
#define SR_OUT_CLOCK    27 
#define SR_OUT_LATCH    14 
#define SR_OUT_OE       4   

#define SR_IN_DATA      5 
#define SR_IN_CLOCK     17 
#define SR_IN_LOAD      16 

/* ===================== MAPA I/O ===================== */
#define IDX_IN1 0 // Vazio
#define IDX_IN2 1 // Vazio
#define IDX_IN3 2 // Emergencia
#define IDX_IN4 3 // Vazio
#define IDX_IN5 4 // Vazio
#define IDX_IN6 5 // Vazio
#define IDX_IN7 6 // >>> STOP VISÃO <<<
#define IDX_IN8 7 // >>> START VISÃO <<<

#define IDX_OUT1 0 // OK (Verde)
#define IDX_OUT2 1 // NOK (Vermelho)
#define IDX_OUT3 2 // Start Maquina (Manual)
#define IDX_OUT4 3 // Stop Maquina (Manual)
#define IDX_OUT5 4 // Rearme
#define IDX_OUT6 5 // Vazio
#define IDX_OUT7 6 // Vazio
#define IDX_OUT8 7 // Vazio

/* ===================== VARIÁVEIS ===================== */
byte estadoEntradas = 0; 
byte estadoRelays = 0;   

bool cicloAtivo = false; // Controla apenas a Visão
bool alarmeAtivo = false;

// Memórias de botão
int ultimoSensorVisao = LOW; 
int ultimoSensorFim = LOW;   

unsigned long ultimoEnvioEmergencia = 0;
const unsigned long INTERVALO_EMERGENCIA = 60000; 
unsigned long ultimoBlinkLED = 0;
bool estadoLed = false;


/* ===================== FUNÇÕES HARDWARE ===================== */

void atualizarHardware() {
    digitalWrite(SR_OUT_LATCH, LOW);
    shiftOut(SR_OUT_DATA, SR_OUT_CLOCK, MSBFIRST, estadoRelays);
    shiftOut(SR_OUT_DATA, SR_OUT_CLOCK, MSBFIRST, 0x00); 
    shiftOut(SR_OUT_DATA, SR_OUT_CLOCK, MSBFIRST, 0xFF); 
    digitalWrite(SR_OUT_LATCH, HIGH);
}

void lerEntradas() {
    digitalWrite(SR_IN_LOAD, LOW);
    delayMicroseconds(5);
    digitalWrite(SR_IN_LOAD, HIGH);
    estadoEntradas = shiftIn(SR_IN_DATA, SR_IN_CLOCK, MSBFIRST);
}

bool lerEntrada(int idEntrada) { 
    return !((estadoEntradas >> idEntrada) & 1); 
}

void escreverSaida(int idSaida, bool ligado) {
    if (ligado) estadoRelays |= (1 << idSaida);
    else estadoRelays &= ~(1 << idSaida);
    atualizarHardware(); 
}

void darPulso(int pino) {
    escreverSaida(pino, HIGH);
    unsigned long t = millis();
    while(millis() - t < 500) { thing.handle(); mb.task(); }
    escreverSaida(pino, LOW);
}

/* ===================== LÓGICA ===================== */

void verificar_logica() {
    lerEntradas(); 

    int sensorHabilita = lerEntrada(IDX_IN8); // IN8 (LIGA VISÃO)
    int sensorFim      = lerEntrada(IDX_IN7); // IN7 (DESLIGA VISÃO)
    int emergencia     = lerEntrada(IDX_IN3); 


    // --- 1. IN8: ATIVA VISÃO E LIMPA LUZES ---
    if (sensorHabilita == HIGH && !cicloAtivo) {
        cicloAtivo = true;
        
        // Apenas apaga as luzes de resultado
        escreverSaida(IDX_OUT1, false);
        escreverSaida(IDX_OUT2, false);
    }

    // --- 2. IN7: DESATIVA VISÃO E APAGA LUZES ---
    if (sensorFim == HIGH && ultimoSensorFim == LOW) {
        cicloAtivo = false;
        
        // Apaga luzes
        escreverSaida(IDX_OUT1, false);
        escreverSaida(IDX_OUT2, false);
    }
    ultimoSensorFim = sensorFim;


    // --- 3. INFORMAR MODBUS ---
    mb.Ists(ENDERECO_ESTADO, cicloAtivo);


    // --- 4. RECEBER MODBUS (SÓ SE VISÃO ATIVA) ---
    if (cicloAtivo) {
        
        // SE OK: Liga 1, Desliga 2
        if (mb.Coil(COIL_OK)) {
            escreverSaida(IDX_OUT1, true); 
            escreverSaida(IDX_OUT2, false);
            mb.Coil(COIL_OK, false); 
        }

        // SE NOK: Liga 2, Desliga 1
        if (mb.Coil(COIL_NOK)) {
            escreverSaida(IDX_OUT2, true); 
            escreverSaida(IDX_OUT1, false);
            mb.Coil(COIL_NOK, false); 
        }
    } 
    else {
        // Limpa buffer se inativo
        if (mb.Coil(COIL_OK)) mb.Coil(COIL_OK, false);
        if (mb.Coil(COIL_NOK)) mb.Coil(COIL_NOK, false);
    }


    // --- 5. EMERGÊNCIA ---
    bool falhaHardware = (emergencia == HIGH) && (lerEntrada(IDX_IN4) == HIGH) && (lerEntrada(IDX_IN5) == HIGH);

    if (emergencia == HIGH && !falhaHardware) {
        unsigned long tempoAtual = millis();
        if (tempoAtual - ultimoEnvioEmergencia > INTERVALO_EMERGENCIA) {
            thing.handle(); 
            thing.call_endpoint("alerta_emergencia"); 
            thing.call_endpoint("alerta_emergencia_email"); 
            ultimoEnvioEmergencia = tempoAtual;
            
            // Desliga visão se carregarem na emergência
            if (cicloAtivo) {
                cicloAtivo = false;
                escreverSaida(IDX_OUT1, false);
                escreverSaida(IDX_OUT2, false);
            }
        } 
    }
}


/* ===================== SETUP ===================== */
void setup() {
    Serial.begin(115200);

    pinMode(SR_OUT_DATA, OUTPUT); pinMode(SR_OUT_CLOCK, OUTPUT);
    pinMode(SR_OUT_LATCH, OUTPUT); pinMode(SR_OUT_OE, OUTPUT);
    pinMode(SR_IN_DATA, INPUT); pinMode(SR_IN_CLOCK, OUTPUT);
    pinMode(SR_IN_LOAD, OUTPUT);
    pinMode(LED_DEBUG, OUTPUT);

    digitalWrite(SR_OUT_OE, LOW); 
    atualizarHardware(); 

    // WI-FI (Conexão Segura)
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, SSID_PASSWORD);
    
    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
        delay(500);
        tentativas++;
    }
    
    // PROTOCOLOS
    thing.add_wifi(SSID, SSID_PASSWORD);

    mb.server();
    mb.addIsts(ENDERECO_ESTADO, false);
    mb.addCoil(COIL_OK, false);
    mb.addCoil(COIL_NOK, false);

    // TELEMETRIA
    thing["entradas"] >> [](pson &out){
        out["visao_ativa"] = cicloAtivo;
    };

    // COMANDOS MANUAIS (Só App)
    thing["start"] << [](pson &in){ if(!in.is_empty()) darPulso(IDX_OUT3); };
    thing["stop"]  << [](pson &in){ if(!in.is_empty()) darPulso(IDX_OUT4); };
    thing["rearme"] << [](pson &in){ if(!in.is_empty()) darPulso(IDX_OUT5); };
}

/* ===================== LOOP ===================== */
void loop() {
    thing.handle();
    mb.task();
    verificar_logica(); 

    if (millis() - ultimoBlinkLED > 1000) {
        ultimoBlinkLED = millis();
        estadoLed = !estadoLed;
        digitalWrite(LED_DEBUG, estadoLed ? HIGH : LOW);
    }
}
