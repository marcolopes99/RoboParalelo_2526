#include <WiFi.h>
#include <ThingerESP32.h>
#include <WebServer.h>


// ===================== CONFIG Wi-Fi ===================== */
#define USERNAME "marcolopes1993"
#define DEVICE_ID "esp32_hmi"
#define DEVICE_CREDENTIAL "915531736"
#define SSID "Iphones"
#define SSID_PASSWORD "935774275"

ThingerESP32 thing(USERNAME, DEVICE_ID, DEVICE_CREDENTIAL);
WebServer server(80); 


bool servidorWebIniciado = false;

/* ===================== HARDWARE ES32A08 ===================== */
#define LED_DEBUG       15  

#define SR_OUT_DATA     13 
#define SR_OUT_CLOCK    27 
#define SR_OUT_LATCH    14 
#define SR_OUT_OE       4   

#define SR_IN_DATA      5 
#define SR_IN_CLOCK     17 
#define SR_IN_LOAD      16 

/* ===================== MAPA DE I/O ===================== */
#define IDX_IN1 0 // Led Verde
#define IDX_IN2 1 // Led Amarelo
#define IDX_IN3 2 // Emergencia
#define IDX_IN4 3 // Sensor Inicio
#define IDX_IN5 4 // Sensor Estampagem
#define IDX_IN6 5 // Sens. Visao (SENSOR FISICO)
#define IDX_IN7 6 // Sens. Fim (RESET LUZES / DESTRAVA)
#define IDX_IN8 7 // HABILITAÇÃO VISAO

#define IDX_OUT1 0 // Visao OK
#define IDX_OUT2 1 // Visao NOK
#define IDX_OUT3 2 // Rearme
#define IDX_OUT4 3 // Start
#define IDX_OUT5 4 // Stop
#define IDX_OUT6 5 
#define IDX_OUT7 6 
#define IDX_OUT8 7 

/* ===================== VARIÁVEIS ===================== */
byte estadoEntradas = 0; 
byte estadoRelays = 0;   

// Estados
bool maquinaAtiva = false;
bool alarmeAtivo = false; 

// --- CONTROLO DE LOOP (ANTI-REPETIÇÃO) ---
bool resultadoProcessado = false; 

// --- CONTADORES DE PEÇAS (REPOSTOS) ---
unsigned long contadorBoas = 0;
unsigned long contadorMas = 0;
unsigned long contadorTotal = 0;

// Tempos (Gerais de trabalho, sem ciclo individual)
unsigned long tempoInicioTotal = 0; 
unsigned long tempoTotalTrabalhoAcumulado = 0; 
unsigned long tempoInicioParagem = 0; 
unsigned long tempoTotalParadoAcumulado = 0; 

// Filtros de Ruído 
unsigned long ultimoTempoContagem = 0; 
const int TEMPO_DEBOUNCE = 250; 

// Memórias de Estado
int ultimoSensorVisao = LOW; 
int ultimoSensorFim = LOW;   

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
    delay(500); 
    escreverSaida(pino, LOW);
}

/* ===================== COMANDOS START/STOP ===================== */

void comandoStop() {
    if(maquinaAtiva) {
        maquinaAtiva = false;
        tempoTotalTrabalhoAcumulado += (millis() - tempoInicioTotal);
        tempoInicioParagem = millis();
    }
    darPulso(IDX_OUT5); 
    //thing.call_endpoint("alerta_stop");
}

void comandoStart() {
    if(!maquinaAtiva) {
        if(tempoInicioParagem > 0) {
            tempoTotalParadoAcumulado += (millis() - tempoInicioParagem);
            tempoInicioParagem = 0;
        }
        maquinaAtiva = true; 
        tempoInicioTotal = millis(); 
    }
    darPulso(IDX_OUT4); 
    //thing.call_endpoint("alerta_inicio"); 
}

/* ===================== LÓGICA MÁQUINA ===================== */

void verificar_logica() {
    lerEntradas(); 

    int sensorVisao    = lerEntrada(IDX_IN6);
    int sensorHabilita = lerEntrada(IDX_IN8);
    int sensorFim      = lerEntrada(IDX_IN7);
    int emergencia     = lerEntrada(IDX_IN3); 

    // RESET LUZES E CICLO NA SAÍDA DA PEÇA (IN7)
    if (sensorFim != ultimoSensorFim) {

        escreverSaida(IDX_OUT1, false);
        escreverSaida(IDX_OUT2, false);

        resultadoProcessado = false;

        Serial.println("RESET CICLO POR IN7");
    }

    ultimoSensorFim = sensorFim;

    // EMERGÊNCIA
    bool novoEstadoAlarme = (emergencia == HIGH);
    if (novoEstadoAlarme && !alarmeAtivo) {
        thing.call_endpoint("alerta_emergencia"); 
        thing.call_endpoint("alerta_emergencia_email"); 
        if (maquinaAtiva) comandoStop(); 
    }
    alarmeAtivo = novoEstadoAlarme;

    // CONTAGEM POR SENSOR FÍSICO (IN6)
    if (sensorVisao == HIGH && ultimoSensorVisao == LOW) {
        if (millis() - ultimoTempoContagem > TEMPO_DEBOUNCE) {
            if (sensorHabilita == HIGH) {
                contadorBoas++;
                ultimoTempoContagem = millis(); 
            }
        }
    }
    ultimoSensorVisao = sensorVisao;
    
    contadorTotal = contadorBoas + contadorMas;
}

/* ===================== AURORA VISION (HTTP) ===================== */
void handleAuroraVision() {

    if (!server.hasArg("result")) {
        server.send(400, "text/plain", "Missing result");
        return;
    }

    // Bloqueio por peça
    if (resultadoProcessado) {
        server.send(200, "text/plain", "Ignorado (resultado já definido)");
        return;
    }

    String resultado = server.arg("result");

    lerEntradas(); 
    bool sistemaHabilitado = (lerEntrada(IDX_IN8) == HIGH); 

    if (!sistemaHabilitado) {
        Serial.println("Aurora: Ignorado (IN8 OFF)");
        server.send(200, "text/plain", "Sistema desabilitado");
        return;
    }

    if (resultado == "OK") {
        contadorBoas++;
        contadorTotal = contadorBoas + contadorMas;

        escreverSaida(IDX_OUT1, true);   // LATCH OK
        escreverSaida(IDX_OUT2, false);  // Garante exclusão

        resultadoProcessado = true;

        Serial.println("Aurora: PECA OK");
    }
    else if (resultado == "NOK") {
        contadorMas++;
        contadorTotal = contadorBoas + contadorMas;

        escreverSaida(IDX_OUT2, true);   // LATCH NOK
        escreverSaida(IDX_OUT1, false);

        resultadoProcessado = true;

        Serial.println("Aurora: PECA NOK");
    }
    else {
        server.send(400, "text/plain", "Comando invalido");
        return;
    }

    server.send(200, "text/plain", "Resultado aceite");
}


/* ================================================================ */  

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

    // 2. Wi-Fi
    thing.add_wifi(SSID, SSID_PASSWORD);

    /* --- TELEMETRIA --- */
    thing["entradas"] >> [](pson &out){
        out["in1"] = lerEntrada(IDX_IN1); 
        out["in2"] = lerEntrada(IDX_IN2); 
        out["in3"] = lerEntrada(IDX_IN3); 
        out["in4"] = lerEntrada(IDX_IN4); 
        out["in5"] = lerEntrada(IDX_IN5); 
        out["in6"] = lerEntrada(IDX_IN6); 
        out["in7"] = lerEntrada(IDX_IN7); 
        out["in8"] = lerEntrada(IDX_IN8); 
        
        // Contadores REPOSTOS
        out["boas"] = contadorBoas; 
        out["mas"] = contadorMas; 
        out["total"] = contadorTotal;
        out["estado"] = maquinaAtiva ? 1 : 0;
        out["alarme"] = alarmeAtivo;

        // Tempos Gerais
        unsigned long trab = tempoTotalTrabalhoAcumulado;
        unsigned long parado = tempoTotalParadoAcumulado;

        if (maquinaAtiva) {
            trab += (millis() - tempoInicioTotal);
        } else {
            parado += (millis() - tempoInicioParagem);
        }

        long tSeg = (long)(trab / 1000);
        long pSeg = (long)(parado / 1000);

        out["tempo_ligado_min"] = tSeg / 60; out["tempo_ligado_s"] = tSeg % 60;
        out["tempo_parado_min"] = pSeg / 60; out["tempo_parado_s"] = pSeg % 60;

    };

    /* --- COMANDOS --- */
    thing["start"] << [](pson &in){ if(!in.is_empty()) comandoStart(); };
    thing["stop"]  << [](pson &in){ if(!in.is_empty()) comandoStop(); };
    thing["rearme"] << [](pson &in){ if(!in.is_empty()) darPulso(IDX_OUT3); };

    // Resets Contadores REPOSTOS
    thing["reset_boas"] << [](pson &in){ if(!in.is_empty()) contadorBoas = 0; };
    thing["reset_mas"]  << [](pson &in){ if(!in.is_empty()) contadorMas = 0; };
    thing["reset_all"]  << [](pson &in){ 
        if(!in.is_empty()) {
            contadorBoas=0; contadorMas=0; contadorTotal=0;
            tempoTotalTrabalhoAcumulado=0; tempoTotalParadoAcumulado=0;
            if (!maquinaAtiva) tempoInicioParagem = millis();
        }
    };
    thing["reset_tempos"] << [](pson &in){
        if(!in.is_empty()) {
            tempoTotalTrabalhoAcumulado = 0; tempoTotalParadoAcumulado = 0; 
            if (!maquinaAtiva) { tempoInicioParagem = millis(); } 
            else { tempoInicioTotal = millis(); }
        }
    };


    server.on("/vision", handleAuroraVision);


    tempoInicioParagem = millis(); 
}

/* ===================== LOOP ===================== */
void loop() {
    
    thing.handle();

    if (WiFi.status() == WL_CONNECTED) {
        if (!servidorWebIniciado) {
            server.begin();
            servidorWebIniciado = true;
            Serial.println("\nWI-FI LIGADO!");
            Serial.println(WiFi.localIP());
        }
        server.handleClient();
    } 
    else {
        servidorWebIniciado = false;
    }

    verificar_logica(); 

    if (millis() - ultimoBlinkLED > 1000) {
        ultimoBlinkLED = millis();
        estadoLed = !estadoLed;
        digitalWrite(LED_DEBUG, estadoLed ? HIGH : LOW);
    }
}