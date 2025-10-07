/*
  ESP32 Dynamic WiFi Config (Captive Portal + Web Interface)
  - Inicia en STA si hay credenciales guardadas y la conexión es exitosa.
  - Si no hay credenciales o la conexión falla, inicia un AP y un portal cautivo
    para que el usuario ingrese SSID y contraseña.
  - Credenciales guardadas en Preferences (NVS).
  - Endpoints:
     GET  /             -> Formulario HTML para configurar WiFi (captura desde navegador)
     POST /wifi         -> Guarda credenciales (form-urlencoded)
     POST /api/wifi     -> Guarda credenciales (JSON o form-urlencoded)
     GET  /api/status   -> Estado JSON (connected, ssid, ip)
     POST /api/reset    -> Borra configuración (resetea)
     GET  /reset        -> Borra configuración y redirige/confirmación
  - Funciones: reconexión automática, restablecer credenciales, captive portal via DNSServer.
  - Librerías usadas: WiFi.h, WebServer.h, DNSServer.h, Preferences.h
  - Autor: (tu nombre)
  - Fecha: (colocar fecha)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h> // Necesita instalar ArduinoJson (opcional, si no la tienes el endpoint JSON fallará)

// ====== Configuración ======
const char* AP_SSID = "ESP32_ConfigAP";
const char* AP_PASS = "configesp32"; // opcional, puede dejarse vacío
const byte DNS_PORT = 53;
const IPAddress AP_IP(192,168,4,1);
const IPAddress AP_NETMASK(255,255,255,0);

const char* PREF_NAMESPACE = "wifi_cfg";
const char* PREF_KEY_SSID = "ssid";
const char* PREF_KEY_PASS = "pass";

const unsigned long CONNECT_TIMEOUT_MS = 15000; // tiempo a intentar conectar al arranque (ms)
const unsigned long RECONNECT_INTERVAL_MS = 15000; // intervalo entre intentos de reconexión si se cae

// ====== Objetos globales ======
WebServer server(80);
DNSServer dnsServer;
Preferences prefs;

String savedSSID = "";
String savedPASS = "";
bool haveSavedCredentials = false;

unsigned long lastReconnectAttempt = 0;
unsigned long lastConnectTryTime = 0;

// ====== HTML del portal (simple, inline) ======
const char* CONFIG_PAGE = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Configurar WiFi - ESP32</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{font-family:Arial,Helvetica,sans-serif;margin:20px;background:#f4f4f4}
    .card{background:#fff;padding:20px;border-radius:8px;max-width:520px;margin:auto;box-shadow:0 2px 6px rgba(0,0,0,0.15)}
    h2{margin-top:0}
    input[type=text], input[type=password]{width:100%;padding:10px;margin:8px 0;border:1px solid #ccc;border-radius:4px}
    button{padding:10px 16px;border:none;background:#007bff;color:#fff;border-radius:4px;cursor:pointer}
    .info{font-size:0.9em;color:#444}
    .small{font-size:0.8em;color:#666}
    .danger{color:#b00}
  </style>
</head>
<body>
  <div class="card">
    <h2>Configurar WiFi</h2>
    <p class="info">Ingrese el SSID y la contraseña de la red WiFi a la que debe conectarse el dispositivo.</p>
    <form action="/wifi" method="POST">
      <label>SSID</label><br>
      <input type="text" name="ssid" required maxlength="64"><br>
      <label>Contraseña</label><br>
      <input type="password" name="pass" maxlength="64"><br>
      <button type="submit">Guardar y conectar</button>
    </form>
    <hr>
    <p class="small">También puedes usar el endpoint <code>/api/wifi</code> (POST, JSON) para configurar programáticamente.</p>
    <p class="small danger">IMPORTANTE: La primera conexión puede tardar unos segundos. Si falla, vuelve a la página y verifica SSID/contraseña.</p>
    <hr>
    <form action="/reset" method="GET">
      <button type="submit">Restablecer configuración (borrar credenciales)</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

// ====== Prototipos ======
void startAPPortal();
void stopAPPortal();
void handleRoot();
void handleWifiSubmit();
void handleApiWifi();
void handleStatus();
void handleReset();
void handleApiReset();
void loadCredentials();
void saveCredentials(const char* ssid, const char* pass);
void clearCredentials();
void tryConnectSaved();
void captiveRedirect();
String jsEscape(const String &s);

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("== ESP32 Dynamic WiFi Config ==");
  prefs.begin(PREF_NAMESPACE, false);
  loadCredentials();

  // Intentar conectar con credenciales guardadas
  if (haveSavedCredentials) {
    Serial.printf("Intentando conectar a SSID guardado: %s\n", savedSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    lastConnectTryTime = millis();

    unsigned long startAttempt = millis();
    while ((millis() - startAttempt) < CONNECT_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(200);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Conectado en modo STA.");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      // Montaremos servidor mínimo de estado y endpoints mientras esté en STA
      server.on("/api/status", HTTP_GET, handleStatus);
      server.on("/api/wifi", HTTP_POST, handleApiWifi); // permitir reconfiguración desde la red local
      server.on("/api/reset", HTTP_POST, handleApiReset);
      server.begin();
      return;
    } else {
      Serial.println("No se conectó con las credenciales guardadas. Iniciando AP portal...");
      // limpiar posibles intentos fallidos
      WiFi.disconnect(true);
      delay(500);
      startAPPortal();
    }
  } else {
    Serial.println("No hay credenciales guardadas. Iniciando AP portal...");
    startAPPortal();
  }
}

// ====== Loop ======
void loop() {
  // Si el servidor está corriendo (ya sea en AP o STA) procesar peticiones
  server.handleClient();

  // Si tenemos AP con DNS activo, atender las peticiones DNS
  dnsServer.processNextRequest();

  // Si estamos en STA pero desconectados, intentar reconectar periódicamente
  if (WiFi.getMode() == WIFI_MODE_APSTA || WiFi.getMode() == WIFI_MODE_STA) {
    if (WiFi.status() != WL_CONNECTED && haveSavedCredentials) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
        Serial.println("Intento de reconexión...");
        lastReconnectAttempt = now;
        WiFi.disconnect();
        delay(200);
        WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
      }
    }
  }
}

// ====== Funciones ======

void startAPPortal() {
  // Configurar modo AP
  WiFi.mode(WIFI_MODE_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  Serial.print("AP iniciado. SSID: ");
  Serial.println(AP_SSID);
  Serial.print("IP AP: ");
  Serial.println(WiFi.softAPIP());

  // Iniciar DNS server para redirigir todo al portal (captura de DNS)
  dnsServer.start(DNS_PORT, "*", AP_IP);

  // Rutas del servidor web (portal cautivo)
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_POST, handleWifiSubmit); // formulario web
  // API endpoints
  server.on("/api/wifi", HTTP_POST, handleApiWifi);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/reset", HTTP_GET, handleReset);
  server.on("/api/reset", HTTP_POST, handleApiReset);

  // Handler catch-all para redirigir peticiones HTTP al portal (simple captive behavior)
  server.onNotFound([]() {
    // si es una petición de navegador, responder con la página de configuración
    // para simular portal cautivo, devolvemos el formulario.
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "-1");
    server.send(200, "text/html", CONFIG_PAGE);
  });

  server.begin();
}

void stopAPPortal() {
  // cerrar servidor y DNS
  server.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html", CONFIG_PAGE);
}

void handleWifiSubmit() {
  // Form-urlencoded: ssid, pass
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "Falta ssid");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  ssid.trim();
  pass.trim();

  saveCredentials(ssid.c_str(), pass.c_str());

  // Intentar conectar
  stopAPPortal();
  Serial.printf("Guardadas credenciales. Intentando conectar a '%s'\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long startAttempt = millis();
  bool connected = false;
  while ((millis() - startAttempt) < CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (connected) {
    String body = "<html><body><h2>Conectado correctamente</h2><p>IP: " + WiFi.localIP().toString() + "</p></body></html>";
    server.send(200, "text/html", body);
    // Iniciar server con endpoints en modo STA (si no está ya)
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/wifi", HTTP_POST, handleApiWifi);
    server.on("/api/reset", HTTP_POST, handleApiReset);
  } else {
    // Si no se conectó, volver a iniciar AP portal y mostrar fallo
    Serial.println("Fallo al conectar con las credenciales proporcionadas. Reiniciando portal AP.");
    // Re-iniciar portal AP
    startAPPortal();
    String body = "<html><body><h2>Error al conectar</h2><p>Las credenciales no funcionaron. Vuelve a intentarlo.</p><a href='/'>Volver</a></body></html>";
    server.send(200, "text/html", body);
  }
}

void handleApiWifi() {
  // Acepta JSON {"ssid":"mired","pass":"mipass"} o form-urlencoded
  bool gotSSID = false;
  String ssid, pass;

  // Primero intentar JSON
  if (server.hasHeader("Content-Type")) {
    String ct = server.header("Content-Type");
    if (ct.indexOf("application/json") >= 0) {
      String payload = server.arg("plain");
      if (payload.length() > 0) {
        // Parse JSON con ArduinoJson
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
          if (doc.containsKey("ssid")) {
            ssid = String((const char*)doc["ssid"]);
            gotSSID = true;
          }
          if (doc.containsKey("pass")) {
            pass = String((const char*)doc["pass"]);
          }
        } else {
          server.send(400, "application/json", "{\"error\":\"JSON inválido\"}");
          return;
        }
      }
    }
  }

  // Si no vino JSON, revisar form-urlencoded
  if (!gotSSID) {
    if (server.hasArg("ssid")) {
      ssid = server.arg("ssid");
      pass = server.arg("pass");
      gotSSID = true;
    } else {
      // También permitir query params ?ssid=...&pass=...
      if (server.hasArg("plain") && server.arg("plain").length()>0) {
        // fallback: maybe body is form without header? parse naive
        String p = server.arg("plain");
        int i = p.indexOf("ssid=");
        if (i>=0) {
          int j = p.indexOf('&', i);
          if (j<0) j = p.length();
          ssid = p.substring(i+5, j);
          ssid.replace("+"," ");
          gotSSID = true;
        }
        i = p.indexOf("pass=");
        if (i>=0) {
          int j = p.indexOf('&', i);
          if (j<0) j = p.length();
          pass = p.substring(i+5, j);
          pass.replace("+"," ");
        }
      }
    }
  }

  if (!gotSSID) {
    server.send(400, "application/json", "{\"error\":\"ssid requerido\"}");
    return;
  }

  ssid.trim();
  pass.trim();
  saveCredentials(ssid.c_str(), pass.c_str());

  // Intentar conectar
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long startAttempt = millis();
  bool connected = false;
  while ((millis() - startAttempt) < CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    delay(200);
  }

  if (connected) {
    StaticJsonDocument<256> resp;
    resp["status"] = "connected";
    resp["ssid"] = ssid;
    resp["ip"] = WiFi.localIP().toString();
    String out;
    serializeJson(resp, out);
    server.send(200, "application/json", out);
  } else {
    // Si no conectó, seguir en AP (si está AP) o informar del fallo
    StaticJsonDocument<256> resp;
    resp["status"] = "failed";
    resp["reason"] = "No se pudo conectar con las credenciales";
    String out;
    serializeJson(resp, out);
    server.send(200, "application/json", out);
  }
}

void handleStatus() {
  StaticJsonDocument<256> resp;
  if (WiFi.status() == WL_CONNECTED) {
    resp["connected"] = true;
    resp["ssid"] = WiFi.SSID();
    resp["ip"] = WiFi.localIP().toString();
  } else {
    resp["connected"] = false;
    resp["mode"] = (WiFi.getMode() == WIFI_MODE_AP) ? "AP" : "STA";
    resp["saved_ssid"] = savedSSID;
  }
  String out;
  serializeJson(resp, out);
  server.send(200, "application/json", out);
}

void handleReset() {
  // Borrar credenciales y mostrar confirmación
  clearCredentials();
  String body = "<html><body><h2>Configuración borrada</h2><p>Las credenciales fueron eliminadas. El dispositivo reiniciará en modo AP para permitir nueva configuración.</p></body></html>";
  server.send(200, "text/html", body);
  delay(500);
  ESP.restart();
}

void handleApiReset() {
  clearCredentials();
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Credenciales borradas. Reiniciando.\"}");
  delay(300);
  ESP.restart();
}

void loadCredentials() {
  savedSSID = prefs.getString(PREF_KEY_SSID, "");
  savedPASS = prefs.getString(PREF_KEY_PASS, "");
  if (savedSSID.length() > 0) {
    haveSavedCredentials = true;
    Serial.printf("Credenciales cargadas: SSID='%s'\n", savedSSID.c_str());
  } else {
    haveSavedCredentials = false;
    Serial.println("No hay credenciales guardadas en NVS.");
  }
}

void saveCredentials(const char* ssid, const char* pass) {
  prefs.putString(PREF_KEY_SSID, String(ssid));
  prefs.putString(PREF_KEY_PASS, String(pass));
  prefs.end(); // asegurar flush
  prefs.begin(PREF_NAMESPACE, false);
  savedSSID = String(ssid);
  savedPASS = String(pass);
  haveSavedCredentials = true;
  Serial.printf("Credenciales guardadas en NVS: %s / %s\n", ssid, (pass && strlen(pass)>0) ? "********" : "(vacía)");
}

void clearCredentials() {
  prefs.clear();
  prefs.end();
  prefs.begin(PREF_NAMESPACE, false);
  savedSSID = "";
  savedPASS = "";
  haveSavedCredentials = false;
  Serial.println("Credenciales borradas de NVS.");
}

String jsEscape(const String &s) {
  String r = s;
  r.replace("\\", "\\\\");
  r.replace("\"", "\\\"");
  r.replace("\n", "\\n");
  r.replace("\r", "\\r");
  return r;
}
