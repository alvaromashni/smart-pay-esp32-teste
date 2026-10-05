// Teste de comunicacao ESP32 <-> api-teste (smart-pay)
// Placa: DOIT ESP32 DEVKIT V1 (ESP32-WROOM-32)
//
// Abra o Monitor Serial em 115200 baud e digite uma letra + Enter:
//   h  health (GET /health, sem token)
//   a  cobranca aprovada        d  cobranca recusada
//   e  erro 500 simulado        t  timeout (API demora 8s, placa desiste em 5s)
//   u  token errado (espera 401)
//   i  idempotencia (mesma chave 2x: espera 201 e depois 200 com o mesmo charge_id)
//   l  lista as ultimas cobrancas
//   s  estresse: 20 cobrancas seguidas, mostra taxa de sucesso e latencia
//   ?  ajuda
// O botao BOOT da placa dispara uma cobranca aprovada (simula "cliente apertou o botao").
// LED azul (GPIO 2): 1 piscada longa = aprovada | 3 rapidas = recusada | 6 rapidas = erro/falha

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, API_TOKEN (nao vai pro git)
#include "root_ca.h"   // raizes da Let's Encrypt

const char* API_BASE = "https://api-teste.redesmartshop.com";
const int LED_PIN = 2;
const int BOTAO_PIN = 0;
const uint32_t HTTP_TIMEOUT_MS = 5000;

WiFiClientSecure tls;

struct Resultado {
  int http;          // codigo HTTP, ou negativo em falha de conexao
  uint32_t ms;       // latencia total da requisicao
  String corpo;
};

// ---------------- LED ----------------
void piscar(int vezes, int ligadoMs, int desligadoMs) {
  for (int i = 0; i < vezes; i++) {
    digitalWrite(LED_PIN, HIGH); delay(ligadoMs);
    digitalWrite(LED_PIN, LOW);  delay(desligadoMs);
  }
}

// ---------------- Wi-Fi e relogio ----------------
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Conectando ao Wi-Fi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t inicio = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - inicio > 40000) {
      Serial.println("\n[FALHA] Wi-Fi nao conectou em 40s. Confira SSID/senha e se a rede e 2.4 GHz.");
      return;
    }
    delay(500); Serial.print(".");
  }
  Serial.printf("\n[OK] Wi-Fi conectado. IP %s, sinal %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

// O TLS precisa da hora certa para validar a data do certificado.
void sincronizarRelogio() {
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");
  Serial.print("Sincronizando relogio (NTP)");
  time_t agora = time(nullptr);
  uint32_t inicio = millis();
  while (agora < 1700000000 && millis() - inicio < 15000) {
    delay(300); Serial.print("."); agora = time(nullptr);
  }
  struct tm t; localtime_r(&agora, &t);
  Serial.printf("\n[%s] Hora: %02d/%02d/%04d %02d:%02d:%02d\n",
                agora >= 1700000000 ? "OK" : "FALHA",
                t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
}

// ---------------- HTTP ----------------
Resultado requisitar(const char* metodo, const String& caminho, const String& corpo,
                     const char* token, const String& chaveIdem) {
  Resultado r{0, 0, ""};
  conectarWiFi();
  if (WiFi.status() != WL_CONNECTED) { r.http = -1000; return r; }

  HTTPClient http;
  String url = String(API_BASE) + caminho;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(tls, url)) { r.http = -1001; return r; }

  if (token) http.addHeader("Authorization", String("Bearer ") + token);
  if (corpo.length()) http.addHeader("Content-Type", "application/json");
  if (chaveIdem.length()) http.addHeader("Idempotency-Key", chaveIdem);

  Serial.printf("-> %s %s %s\n", metodo, caminho.c_str(), corpo.c_str());
  uint32_t t0 = millis();
  r.http = http.sendRequest(metodo, corpo);
  if (r.http > 0) r.corpo = http.getString();
  r.ms = millis() - t0;
  http.end();

  if (r.http > 0) {
    Serial.printf("<- HTTP %d em %lu ms\n   %s\n", r.http, (unsigned long)r.ms, r.corpo.c_str());
  } else {
    Serial.printf("<- FALHA de conexao (%d: %s) apos %lu ms\n",
                  r.http, HTTPClient::errorToString(r.http).c_str(), (unsigned long)r.ms);
  }
  return r;
}

String novaChave() {
  return "esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX) + "-" + String(millis());
}

// Cria uma cobranca e devolve o status lido do JSON ("approved", "declined", ...) ou "" em falha.
String cobrar(const char* simulate, int delayMs, const char* token, const String& chave,
              Resultado* saida, String* chargeId) {
  String caminho = String("/v1/charges?simulate=") + simulate;
  if (delayMs > 0) caminho += "&delay_ms=" + String(delayMs);
  String corpo = "{\"product_id\":\"23\",\"value\":650}";

  Resultado r = requisitar("POST", caminho, corpo, token, chave);
  if (saida) *saida = r;
  if (r.http <= 0) return "";

  JsonDocument json;
  if (deserializeJson(json, r.corpo)) { Serial.println("   [!] resposta nao e JSON valido"); return ""; }
  if (chargeId) *chargeId = json["charge_id"] | "";
  if (r.http == 200 || r.http == 201) return json["status"] | "";
  Serial.printf("   erro da API: %s - %s\n", (const char*)(json["error"] | "?"), (const char*)(json["message"] | "?"));
  return "";
}

void sinalizar(const String& status) {
  if (status == "approved") { Serial.println("   => APROVADA"); piscar(1, 1200, 200); }
  else if (status == "declined") { Serial.println("   => RECUSADA"); piscar(3, 150, 150); }
  else { Serial.println("   => SEM COBRANCA (erro/falha)"); piscar(6, 80, 80); }
}

void verificar(bool ok, const char* oQue) {
  Serial.printf("   [%s] %s\n\n", ok ? "PASSOU" : "FALHOU", oQue);
}

// ---------------- Casos de teste ----------------
void testeHealth() {
  Resultado r = requisitar("GET", "/health", "", nullptr, "");
  verificar(r.http == 200 && r.corpo.indexOf("ok") >= 0, "API no ar e TLS validado");
}

void testeSimulado(const char* sim, int esperado, const char* statusEsperado) {
  Resultado r;
  String st = cobrar(sim, 0, API_TOKEN, novaChave(), &r, nullptr);
  sinalizar(st);
  verificar(r.http == esperado && st == statusEsperado, "codigo HTTP e status conforme o contrato");
}

void testeTimeout() {
  Resultado r;
  cobrar("approved", 8000, API_TOKEN, novaChave(), &r, nullptr);
  sinalizar("");
  // Tempo total = conexao/TLS (~1,5s) + ate 5s esperando a resposta.
  // Passa se a placa desistiu por timeout antes dos 8s que a API levaria.
  verificar(r.http == HTTPC_ERROR_READ_TIMEOUT && r.ms < 8000,
            "placa desistiu por timeout antes da API responder, sem travar");
}

void testeTokenErrado() {
  Resultado r;
  cobrar("approved", 0, "token-errado", novaChave(), &r, nullptr);
  verificar(r.http == 401, "API recusou token invalido com 401");
}

void testeIdempotencia() {
  String chave = novaChave();
  Resultado r1, r2; String id1, id2;
  cobrar("approved", 0, API_TOKEN, chave, &r1, &id1);
  cobrar("approved", 0, API_TOKEN, chave, &r2, &id2);
  verificar(r1.http == 201 && r2.http == 200 && id1.length() && id1 == id2,
            "reenvio com a mesma chave nao duplicou a cobranca");
}

void testeLista() {
  Resultado r = requisitar("GET", "/v1/charges?page=1&page_size=5", "", API_TOKEN, "");
  verificar(r.http == 200, "listagem autenticada");
}

void testeEstresse() {
  const int N = 20;
  int ok = 0; uint32_t soma = 0, minMs = UINT32_MAX, maxMs = 0;
  for (int i = 0; i < N; i++) {
    Resultado r;
    String st = cobrar("approved", 0, API_TOKEN, novaChave(), &r, nullptr);
    if (st == "approved") {
      ok++; soma += r.ms;
      if (r.ms < minMs) minMs = r.ms;
      if (r.ms > maxMs) maxMs = r.ms;
    }
  }
  Serial.printf("\n=== Estresse: %d/%d aprovadas | latencia media %lu ms (min %lu, max %lu) | sinal %d dBm | heap livre %u bytes\n",
                ok, N, ok ? (unsigned long)(soma / ok) : 0UL,
                ok ? (unsigned long)minMs : 0UL, (unsigned long)maxMs, WiFi.RSSI(), ESP.getFreeHeap());
  verificar(ok == N, "todas as cobrancas responderam");
}

void ajuda() {
  Serial.println(F("\nComandos: h=health a=aprovada d=recusada e=erro500 t=timeout u=token errado"));
  Serial.println(F("          i=idempotencia l=listar s=estresse(20x) ?=ajuda | botao BOOT = aprovada\n"));
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOTAO_PIN, INPUT_PULLUP);
  Serial.println(F("\n=== smart-pay | teste ESP32 <-> api-teste ==="));

  tls.setCACert(ROOT_CA);
  // Sinal fraco pode demorar: insiste ate conectar antes de seguir.
  while (WiFi.status() != WL_CONNECTED) conectarWiFi();
  sincronizarRelogio();
  testeHealth();
  ajuda();
}

void loop() {
  if (digitalRead(BOTAO_PIN) == LOW) {
    delay(50);
    if (digitalRead(BOTAO_PIN) == LOW) {
      Serial.println("[BOTAO] cobranca aprovada");
      testeSimulado("approved", 201, "approved");
      while (digitalRead(BOTAO_PIN) == LOW) delay(10);
    }
  }

  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'h': testeHealth(); break;
      case 'a': testeSimulado("approved", 201, "approved"); break;
      case 'd': testeSimulado("declined", 201, "declined"); break;
      case 'e': { Resultado r; cobrar("error", 0, API_TOKEN, novaChave(), &r, nullptr); sinalizar("");
                  verificar(r.http == 500, "erro 500 tratado sem travar a placa"); } break;
      case 't': testeTimeout(); break;
      case 'u': testeTokenErrado(); break;
      case 'i': testeIdempotencia(); break;
      case 'l': testeLista(); break;
      case 's': testeEstresse(); break;
      case '?': ajuda(); break;
      default: break;  // ignora \r, \n e outras teclas
    }
  }
}
