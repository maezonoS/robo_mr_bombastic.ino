#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PS4Controller.h>
#include <esp_arduino_version.h>

// ================ WIFI ================
// Cria uma rede Wi-Fi propria para controle do robo pelo celular.
const char* AP_SSID = "ROBO3";
const char* AP_PASS = "INSIRA_SUA_SENHA";

WebServer server(80);

// ================ MOTORES L298N ================
// Pinos utilizados para controlar os motores esquerdo e direito.
#define MOTOR_L1 26
#define MOTOR_L2 25
#define MOTOR_D1 33
#define MOTOR_D2 32

// Canais PWM dos motores (0 a 3)
// Canais PWM exclusivos para cada entrada dos motores.
#define CH_L1 0
#define CH_L2 1
#define CH_D1 2
#define CH_D2 3

#define PWM_FREQ_MOTOR 1000
#define PWM_RES_MOTOR 8

// ================ ESC BRUSHLESS (LEDC direto) ================
// O ESC utiliza um canal separado para evitar conflito com os motores.
#define ESC_PIN 21
#define CH_ESC 4        // Canal separado dos motores (usa outro timer)
#define ESC_FREQ 50     // 50 Hz = periodo de 20 ms
#define ESC_RES 16      // 16 bits de resolucao

const int ESC_MIN_US = 1000;
const int ESC_MAX_US = 2000;
// Altere para 2000 quando quiser liberar 100% da potencia da arma
const int ESC_TEST_MAX_US = 1200;

const unsigned long TIMEOUT_ESC_MS = 500;

int aceleradorESC = 0;
bool escHabilitado = false;
unsigned long ultimoComandoESC = 0;

// ================ SEGURANCA DA TRACAO ================
// Limites utilizados para evitar comandos muito baixos ou perda de controle.
const int PWM_MAX_INICIAL = 127;
const int PWM_MIN_SLIDER = 80;   // Antes era 40 (tensao muito baixa)
const int ZONA_MORTA = 12;

const unsigned long TIMEOUT_COMANDO_MS = 700;
unsigned long ultimoComando = 0;

int velocidadeMaxima = PWM_MAX_INICIAL;
int velocidadeEsq = 0;
int velocidadeDir = 0;

bool controlePS4Ativo = false;
bool conexaoAnteriorPS4 = false;

// ================ PWM ESP32 2.x / 3.x ================
// Configura os canais PWM e mantem compatibilidade
// com as versoes 2.x e 3.x do Arduino Core ESP32.

void configurarPWM(int canal, int pino, int freq, int resolucao) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(pino, freq, resolucao, canal);
#else
  ledcSetup(canal, freq, resolucao);
  ledcAttachPin(pino, canal);
#endif
}

void escreverPWM(int canal, int pino, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pino, duty);
#else
  ledcWrite(canal, duty);
#endif
}

// Converte microssegundos (1000 a 2000 us) em duty de 16 bits a 50 Hz
void escreverESC(int us) {
  us = constrain(us, ESC_MIN_US, ESC_MAX_US);
  uint32_t duty = (uint32_t)us * 65535UL / 20000UL;
  escreverPWM(CH_ESC, ESC_PIN, duty);
}

// ================ CONTROLE DA TRACAO ================}
// Controla individualmente um motor pela ponte H.
// O sinal da velocidade determina o sentido de rotacao.

void setMotor(int chA, int chB, int speed) {
  speed = constrain(speed, -velocidadeMaxima, velocidadeMaxima);

  int pinoA;
  int pinoB;

  if (chA == CH_L1) {
    pinoA = MOTOR_L1;
    pinoB = MOTOR_L2;
  } else {
    pinoA = MOTOR_D1;
    pinoB = MOTOR_D2;
  }

  if (speed > 0) {
    escreverPWM(chA, pinoA, speed);
    escreverPWM(chB, pinoB, 0);
  } else if (speed < 0) {
    escreverPWM(chA, pinoA, 0);
    escreverPWM(chB, pinoB, -speed);
  } else {
    escreverPWM(chA, pinoA, 0);
    escreverPWM(chB, pinoB, 0);
  }
}

// Fator de correcao se um motor for mais forte que o outro (0.8 a 1.0)
float ajusteMotorEsq = 1.0;
float ajusteMotorDir = 1.0; // Se a direita estiver mais rapida, use 0.90

// Aplica as velocidades dos dois motores e seus fatores de correcao.
void aplicarMotores(int esquerda, int direita) {
  velocidadeEsq = constrain(esquerda, -velocidadeMaxima, velocidadeMaxima);
  velocidadeDir = constrain(direita, -velocidadeMaxima, velocidadeMaxima);

  int valEsq = (int)(velocidadeEsq * ajusteMotorEsq);
  int valDir = (int)(velocidadeDir * ajusteMotorDir);

  // Ajuste os sinais (+/-) caso algum lado fique invertido
  setMotor(CH_L1, CH_L2, -valEsq);
  setMotor(CH_D1, CH_D2,  valDir);
}

void pararTracao() {
  aplicarMotores(0, 0);
}

// ================ CONTROLE DO BRUSHLESS ================

// Desabilita o Brushless e envia o pulso minimo para o ESC.
void pararBrushless() {
  aceleradorESC = 0;
  escHabilitado = false;
  escreverESC(ESC_MIN_US);
}
// Controla a potencia do Brushless dentro do limite definido para os testes.
void controlarBrushless(int acelerador) {
  acelerador = constrain(acelerador, 0, 100);

  if (!escHabilitado || acelerador == 0) {
    escreverESC(ESC_MIN_US);
    aceleradorESC = 0;
    return;
  }

  int pulso = map(acelerador, 0, 100, ESC_MIN_US, ESC_TEST_MAX_US);

  aceleradorESC = acelerador;
  escreverESC(pulso);
  ultimoComandoESC = millis();
}

// ================ CONTROLE PELO PS4 ================
// Ignora pequenas variacoes do analogico para evitar movimento involuntario.
int aplicarZonaMorta(int valor) {
  if (abs(valor) < ZONA_MORTA) {
    return 0;
  }
  return valor;
}
// Processa os comandos do PS4.
// Analogico esquerdo controla a tracao, X para e R2 aciona o Brushless.
void controlarPS4() {
  if (!PS4.isConnected()) {
    controlePS4Ativo = false;
    pararBrushless();
    return;
  }

  controlePS4Ativo = true;

  int aceleracao = -aplicarZonaMorta(PS4.LStickY());
  int direcao = aplicarZonaMorta(PS4.LStickX());

  if (PS4.Cross()) {
    pararTracao();
    ultimoComando = millis();
  } else {
    int frente = map(aceleracao, -127, 127, -velocidadeMaxima, velocidadeMaxima);
    int curva  = map(direcao,    -127, 127, -velocidadeMaxima, velocidadeMaxima);

    int esquerda = frente + curva;
    int direita  = frente - curva;

    int maior = max(abs(esquerda), abs(direita));

    if (maior > velocidadeMaxima) {
      esquerda = (long)esquerda * velocidadeMaxima / maior;
      direita  = (long)direita  * velocidadeMaxima / maior;
    }

    aplicarMotores(esquerda, direita);
    ultimoComando = millis();
  }

  // R2 aciona o brushless enquanto pressionado
  if (PS4.R2()) {
    escHabilitado = true;
    controlarBrushless(100);
  } else {
    pararBrushless();
  }
}

// ================ PAGINA WEB ================
// Interface armazenada no ESP32 para controle pelo celular.

const char pagina[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Controle do Robo</title>

<style>
body {
  font-family: Arial, sans-serif;
  background: #151922;
  color: white;
  text-align: center;
  margin: 0;
  padding: 18px;
}

button {
  font-size: 18px;
  font-weight: bold;
  padding: 18px 12px;
  margin: 6px;
  min-width: 100px;
  border: 0;
  border-radius: 12px;
  touch-action: none;
  user-select: none;
}

.move {
  background: #2878e5;
  color: white;
}

.stop {
  background: #dc2626;
  color: white;
  width: 90%;
}

.weapon {
  background: #b91c1c;
  color: white;
  width: 90%;
}

.panel {
  background: #252c39;
  border-radius: 16px;
  padding: 16px;
  margin: 18px auto;
  max-width: 420px;
}

input {
  width: 90%;
}

#status {
  font-size: 18px;
  font-weight: bold;
}
</style>
</head>

<body>
<h1>Controle do Robo</h1>
<p>Controle pelo Wi-Fi ou pelo PS4</p>

<div class="panel">
  <p>Velocidade maxima da tracao</p>
  <h2><span id="v">127</span> / 255</h2>
  <input id="vel" type="range" min="80" max="127" value="127">
</div>

<div class="panel">
  <div>
    <button class="move" data-l="1" data-r="1">FRENTE</button>
  </div>

  <div>
    <button class="move" data-l="-1" data-r="1">ESQUERDA</button>
    <button class="move" data-l="1" data-r="-1">DIREITA</button>
  </div>

  <div>
    <button class="move" data-l="-1" data-r="-1">RE</button>
  </div>

  <button class="stop" id="stop">PARAR</button>

  <p id="status">PARADO</p>
  <p>Com o PS4 conectado, ele tem prioridade.</p>
</div>

<div class="panel">
  <h3>Motor Brushless</h3>
  <p>Acionamento momentaneo</p>

  <button class="weapon" id="brushless">
    SEGURE PARA ACIONAR
  </button>

  <p>Solte para enviar o comando minimo.</p>
  <p>Limite inicial: 1200 us</p>
</div>

<script>
let left = 0;
let right = 0;
let timer = null;
let escTimer = null;
let escPressionado = false;

// Envia os comandos de movimento e velocidade para o ESP32.
async function enviar() {

  const v = document.getElementById('vel').value;
  document.getElementById('v').textContent = v;

  try {
    const resposta = await fetch(
      `/cmd?l=${left * v}&r=${right * v}&v=${v}`,
      { cache: 'no-store' }
    );

    if (resposta.status === 409) {
      document.getElementById('status').textContent = 'PS4 conectado';
    } else if (!resposta.ok) {
      document.getElementById('status').textContent = 'Erro no comando';
    }
  } catch (e) {
    document.getElementById('status').textContent = 'Falha de conexao';
  }
}

function iniciar(l, r) {
  left = l;
  right = r;

  document.getElementById('status').textContent = 'MOVENDO';

  if (timer) clearInterval(timer);

  enviar();
  timer = setInterval(enviar, 200);
}

function parar() {
  left = 0;
  right = 0;

  if (timer) clearInterval(timer);
  timer = null;

  enviar();
  document.getElementById('status').textContent = 'PARADO';
}

document.querySelectorAll('[data-l]').forEach(b => {
  b.addEventListener('pointerdown', e => {
    e.preventDefault();
    iniciar(Number(b.dataset.l), Number(b.dataset.r));
  });

  b.addEventListener('pointerup', e => {
    e.preventDefault();
    parar();
  });

  b.addEventListener('pointercancel', parar);
  b.addEventListener('pointerleave', parar);
});

document.getElementById('stop').addEventListener('click', parar);
document.getElementById('vel').addEventListener('input', enviar);

// ================ BRUSHLESS PELO CELULAR ================

async function enviarESC(on) {
  try {
    await fetch('/esc?on=' + (on ? '1' : '0'), { cache: 'no-store' });
  } catch (e) {
    // O timeout do ESP32 interrompe o comando
  }
}

function iniciarESC(e) {
  e.preventDefault();

  if (escPressionado) return;

  escPressionado = true;
  enviarESC(true);

  if (escTimer) clearInterval(escTimer);
  escTimer = setInterval(() => enviarESC(true), 150);
}

function pararESC() {
  if (!escPressionado) return;

  escPressionado = false;

  if (escTimer) clearInterval(escTimer);
  escTimer = null;

  enviarESC(false);
}


// Mantem o movimento ativo enquanto o botao estiver pressionado.
function iniciar(l, r) {

const botaoESC = document.getElementById('brushless');

botaoESC.addEventListener('pointerdown', iniciarESC);
botaoESC.addEventListener('pointerup', pararESC);
botaoESC.addEventListener('pointercancel', pararESC);
botaoESC.addEventListener('pointerleave', pararESC);

window.addEventListener('blur', () => {
  parar();
  pararESC();
});

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    parar();
    pararESC();
  }
});
</script>
</body>
</html>
)rawliteral";

// ================ ROTAS HTTP ================
// Abre a pagina principal de controle.

void paginaInicial() {
  server.send(200, "text/html; charset=utf-8", pagina);
}

void receberComando() {
  if (!server.hasArg("l") || !server.hasArg("r")) {
    server.send(400, "text/plain", "Parametros ausentes");
    return;
  }

  // O PS4 tem prioridade sobre o celular
  if (PS4.isConnected()) {
    server.send(409, "text/plain", "Controle PS4 ativo");
    return;
  }

  int l = constrain(server.arg("l").toInt(), -255, 255);
  int r = constrain(server.arg("r").toInt(), -255, 255);

  if (server.hasArg("v")) {
    velocidadeMaxima = constrain(
      server.arg("v").toInt(), PWM_MIN_SLIDER, PWM_MAX_INICIAL
    );
  }

  aplicarMotores(l, r);
  ultimoComando = millis();

  server.send(200, "text/plain", "OK");
}

void receberESC() {
  if (PS4.isConnected()) {
    server.send(409, "text/plain", "Controle PS4 ativo");
    return;
  }

  if (!server.hasArg("on")) {
    pararBrushless();
    server.send(400, "text/plain", "Parametro ausente");
    return;
  }

  bool ligar = server.arg("on") == "1";

  if (ligar) {
    escHabilitado = true;
    controlarBrushless(100);
  } else {
    pararBrushless();
  }

  server.send(200, "text/plain", "OK");
}

// ================ SETUP ================

void setup() {
  Serial.begin(115200);

  // Deixa o pino do ESC em nivel baixo ate o PWM ser configurado
  pinMode(ESC_PIN, OUTPUT);
  digitalWrite(ESC_PIN, LOW);

  // 1. MOTORES NOS CANAIS 0 A 3 (1 kHz)
  configurarPWM(CH_L1, MOTOR_L1, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  configurarPWM(CH_L2, MOTOR_L2, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  configurarPWM(CH_D1, MOTOR_D1, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  configurarPWM(CH_D2, MOTOR_D2, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  pararTracao();

  // 2. ESC NO CANAL 4 (50 Hz, timer separado)
  configurarPWM(CH_ESC, ESC_PIN, ESC_FREQ, ESC_RES);

  // 2 segundos em neutro para o SimonK armar
  for (int i = 0; i < 20; i++) {
    escreverESC(ESC_MIN_US);
    delay(100);
  }

  // 3. WI-FI E SERVIDOR
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(AP_SSID, AP_PASS);

  server.on("/", HTTP_GET, paginaInicial);
  server.on("/cmd", HTTP_GET, receberComando);
  server.on("/esc", HTTP_GET, receberESC);
  server.begin();

  // 4. CONTROLE PS4
  PS4.begin();

  ultimoComando = millis();
  ultimoComandoESC = millis();
}

// ================ LOOP ================

void loop() {
  server.handleClient();

  bool conectadoAgora = PS4.isConnected();

  if (conectadoAgora && !conexaoAnteriorPS4) {
    Serial.println("CONTROLE PS4 CONECTADO!");
    pararBrushless();
  }

  if (!conectadoAgora && conexaoAnteriorPS4) {
    Serial.println("CONTROLE PS4 DESCONECTADO!");
    pararTracao();
    pararBrushless();
  }

  conexaoAnteriorPS4 = conectadoAgora;

  if (conectadoAgora) {
    controlarPS4();
  } else {
    controlePS4Ativo = false;

    if (millis() - ultimoComando > TIMEOUT_COMANDO_MS) {
      pararTracao();
    }

    if (escHabilitado && millis() - ultimoComandoESC > TIMEOUT_ESC_MS) {
      pararBrushless();
      Serial.println("Timeout do ESC: comando minimo enviado");
    }
  }

  delay(2);
}
