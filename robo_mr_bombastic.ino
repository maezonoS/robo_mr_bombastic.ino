#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PS4Controller.h>
#include <esp_arduino_version.h>

// ================ WIFI ================
const char* AP_SSID = "ROBO3";
const char* AP_PASS = "robo12345";

WebServer server(80);

// ================ PINOS L298N ================
#define MOTOR_L1 26
#define MOTOR_L2 25
#define MOTOR_D1 33
#define MOTOR_D2 32

// ESC da arma: mantido desativado
#define WEAPON_PIN 21

// Canais PWM
#define CH_L1 0
#define CH_L2 1
#define CH_D1 2
#define CH_D2 3

#define PWM_FREQ_MOTOR 1000
#define PWM_RES_MOTOR 8

// ================ SEGURANCA ================
const int PWM_MAX_INICIAL = 127;
const int ZONA_MORTA = 12;

const unsigned long TIMEOUT_COMANDO_MS = 700;
unsigned long ultimoComando = 0;

int velocidadeMaxima = PWM_MAX_INICIAL;
int velocidadeEsq = 0;
int velocidadeDir = 0;

bool controlePS4Ativo = false;
bool conexaoAnteriorPS4 = false;

// ================ PWM 2.x / 3.x ================

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

// ================ MOTORES ================

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

void aplicarMotores(int esquerda, int direita) {
  velocidadeEsq = constrain(
    esquerda, -velocidadeMaxima, velocidadeMaxima
  );

  velocidadeDir = constrain(
    direita, -velocidadeMaxima, velocidadeMaxima
  );

  setMotor(CH_L1, CH_L2, velocidadeEsq);
  setMotor(CH_D1, CH_D2, velocidadeDir);
}

void pararTracao() {
  aplicarMotores(0, 0);
}

// ================ CONTROLE PELO PS4 ================

int aplicarZonaMorta(int valor) {
  if (abs(valor) < ZONA_MORTA) {
    return 0;
  }

  return valor;
}

void controlarPS4() {
  if (!PS4.isConnected()) {
    controlePS4Ativo = false;
    return;
  }

  controlePS4Ativo = true;

  // Analógico esquerdo:
  // Y negativo = frente; Y positivo = ré.
  int aceleracao = -aplicarZonaMorta(PS4.LStickY());
  int direcao = aplicarZonaMorta(PS4.LStickX());

  // Botão X: parar enquanto estiver pressionado.
  if (PS4.Cross()) {
    pararTracao();
    ultimoComando = millis();
    return;
  }

  // Escala os eixos para o limite PWM.
  int frente = map(aceleracao, -127, 127,
                   -velocidadeMaxima, velocidadeMaxima);

  int curva = map(direcao, -127, 127,
                  -velocidadeMaxima, velocidadeMaxima);

  // Mistura aceleração e direção.
  // Ajustaremos os sentidos se necessário após o teste.
  int esquerda = frente + curva;
  int direita = frente - curva;

  // Normaliza para não ultrapassar o limite PWM.
  int maior = max(abs(esquerda), abs(direita));

  if (maior > velocidadeMaxima) {
    esquerda = (long)esquerda * velocidadeMaxima / maior;
    direita = (long)direita * velocidadeMaxima / maior;
  }

  aplicarMotores(esquerda, direita);
  ultimoComando = millis();
}

// ================ PAGINA WEB ================

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
}
.move { background: #2878e5; color: white; }
.stop { background: #dc2626; color: white; width: 90%; }
.panel {
  background: #252c39;
  border-radius: 16px;
  padding: 16px;
  margin: 18px auto;
  max-width: 420px;
}
input { width: 90%; }
#status { font-size: 18px; font-weight: bold; }
</style>
</head>
<body>
<h1>Controle do Robo</h1>
<p>Controle pelo Wi-Fi ou pelo PS4</p>

<div class="panel">
  <p>Velocidade maxima</p>
  <h2><span id="v">127</span> / 255</h2>
  <input id="vel" type="range" min="40" max="127" value="127">
</div>

<div class="panel">
  <div>
    <button class="move" data-l="1" data-r="1">FRENTE</button>
  </div>
  <div>
    <button class="move" data-l="-1" data-r="1">ESQ.</button>
    <button class="stop" id="stop">PARAR</button>
    <button class="move" data-l="1" data-r="-1">DIR.</button>
  </div>
  <div>
    <button class="move" data-l="-1" data-r="-1">RE</button>
  </div>
  <p id="status">PARADO</p>
  <p>Com o PS4 conectado, ele tem prioridade sobre o celular.</p>
</div>

<script>
let left = 0, right = 0, timer = null;

async function enviar() {
  const v = document.getElementById('vel').value;
  document.getElementById('v').textContent = v;

  try {
    await fetch(`/cmd?l=${left*v}&r=${right*v}&v=${v}`,
                {cache: 'no-store'});
  } catch(e) {
    document.getElementById('status').textContent =
      'Falha de conexao';
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
  b.addEventListener('pointerup', parar);
  b.addEventListener('pointercancel', parar);
  b.addEventListener('pointerleave', parar);
});

document.getElementById('stop').addEventListener('click', parar);
document.getElementById('vel').addEventListener('input', enviar);
window.addEventListener('blur', parar);
document.addEventListener('visibilitychange', () => {
  if (document.hidden) parar();
});
</script>
</body>
</html>
)rawliteral";

// ================ ROTAS HTTP ================

void paginaInicial() {
  server.send(200, "text/html; charset=utf-8", pagina);
}

void receberComando() {
  if (!server.hasArg("l") || !server.hasArg("r")) {
    server.send(400, "text/plain", "Parametros ausentes");
    return;
  }

  // Evita comandos do celular enquanto o PS4 estiver conectado.
  if (PS4.isConnected()) {
    server.send(409, "text/plain", "Controle PS4 ativo");
    return;
  }

  int l = server.arg("l").toInt();
  int r = server.arg("r").toInt();

  if (server.hasArg("v")) {
    velocidadeMaxima = constrain(
      server.arg("v").toInt(), 40, PWM_MAX_INICIAL
    );
  }

  aplicarMotores(l, r);
  ultimoComando = millis();

  server.send(200, "text/plain", "OK");
}

// ================ SETUP ================

void setup() {
  Serial.begin(115200);

  configurarPWM(CH_L1, MOTOR_L1, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  configurarPWM(CH_L2, MOTOR_L2, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  configurarPWM(CH_D1, MOTOR_D1, PWM_FREQ_MOTOR, PWM_RES_MOTOR);
  configurarPWM(CH_D2, MOTOR_D2, PWM_FREQ_MOTOR, PWM_RES_MOTOR);

  // Arma desabilitada: nao enviar sinal de acionamento.
  pinMode(WEAPON_PIN, OUTPUT);
  digitalWrite(WEAPON_PIN, LOW);

  pararTracao();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  if (WiFi.softAP(AP_SSID, AP_PASS)) {
    Serial.println("Rede Wi-Fi iniciada");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Falha ao iniciar Wi-Fi");
  }

  server.on("/", HTTP_GET, paginaInicial);
  server.on("/cmd", HTTP_GET, receberComando);
  server.begin();

  // Inicia Bluetooth do controle PS4.
  // Se sua versao da biblioteca exigir um endereco MAC,
  // sera necessario configurar esse endereco aqui.
  PS4.begin();

  Serial.println("Aguardando controle PS4 por Bluetooth");
  Serial.println("Pressione PS + SHARE no controle para parear");

  ultimoComando = millis();
}

// ================ LOOP ================

void loop() {
  server.handleClient();
  bool conectadoAgora = PS4.isConnected();

if (conectadoAgora && !conexaoAnteriorPS4) {
  Serial.println("CONTROLE PS4 CONECTADO!");
}

if (!conectadoAgora && conexaoAnteriorPS4) {
  Serial.println("CONTROLE PS4 DESCONECTADO!");
}

conexaoAnteriorPS4 = conectadoAgora;

  if (PS4.isConnected()) {
    controlarPS4();
  } else {
    controlePS4Ativo = false;

    if (millis() - ultimoComando > TIMEOUT_COMANDO_MS) {
      pararTracao();
    }
  }

  delay(2);
}
