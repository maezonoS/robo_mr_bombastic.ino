
  // 2. ESC NO CANAL 4 (50 Hz, timer separado)
  configurarPWM(CH_ESC, ESC_PIN, ESC_FREQ, ESC_RES);

  // 2 segundos em neutro para o SimonK armar
  for (int i = 0; i < 20; i++) {
    escreverESC(ESC_MIN_US);
    delay(100);
  }

  // 3. WI-FI E SERVIDOR
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);          // Desativa economia de energia (menos atraso)
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

/*
 * loop()
 * Roda continuamente:
 *   1. Atende as requisicoes da pagina web.
 *   2. Detecta quando o PS4 conecta ou desconecta e desliga a arma
 *      (e a tracao, na desconexao) por seguranca.
 *   3. Com PS4 conectado: controla pelo PS4.
 *      Sem PS4: aplica os timeouts de seguranca do Wi-Fi
 *        - 700 ms sem comando -> para a tracao
 *        - 500 ms sem comando da arma -> desliga o brushless
 */
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

  delay(2);  // Pequena pausa para nao sobrecarregar o processador
}
