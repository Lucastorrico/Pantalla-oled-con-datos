#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// -------------------- Pines --------------------
#define RELE1 4
#define DHTPIN 18
#define DHTTYPE DHT22
#define OLED_RESET -1
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// -------------------- Temperaturas --------------------
const float TEMPERATURA_APAGADO_INICIAL = 37.7f;
const float TEMPERATURA_ENCENDIDO_INICIAL = 37.0f;
float temperaturaApagado = TEMPERATURA_APAGADO_INICIAL;
float temperaturaEncendido = TEMPERATURA_ENCENDIDO_INICIAL;

// -------------------- WiFi --------------------
const char *WIFI_SSID_INICIAL = "Lucaswifi";
const char *WIFI_CLAVE_INICIAL = "12345678";
const char *AP_SSID = "ESP32-Control-Lucas";
const char *AP_CLAVE = "12345678";

// -------------------- Objetos --------------------
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
Preferences preferencias;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// -------------------- Estado general --------------------
float temperatura = NAN;
bool lecturaValida = false;
bool oledDisponible = false;
bool releEncendido = false;
bool littleFSDisponible = false;
bool modoAP = false;
bool ntpIniciado = false;
bool horaConfirmada = false;
bool reinicioPendiente = false;
bool muestraGuardadaEnEstaSesion = false;

String wifiSSID;
String wifiClave;

unsigned long ultimaLectura = 0;
unsigned long ultimaMuestraGuardada = 0;
unsigned long ultimaRevisionWiFi = 0;
unsigned long momentoReinicio = 0;

const unsigned long INTERVALO_LECTURA = 2500;
uint32_t intervaloDentroSegundos = 300;  // 5 minutos
uint32_t intervaloFueraSegundos = 60;    // 1 minuto
uint32_t versionEventos = 0;

// -------------------- Historial --------------------
// Se reserva memoria para 6000 muestras. Desde la pagina se puede elegir
const size_t CAPACIDAD_MAX_MUESTRAS = 6000;
const size_t LIMITE_MUESTRAS_INICIAL = 4032;
size_t limiteMuestras = LIMITE_MUESTRAS_INICIAL;

struct Muestra {
  uint32_t epoch;
  uint32_t marcaMillis;
  int16_t temperaturaDecimas;
  uint8_t rele;
};

Muestra historial[CAPACIDAD_MAX_MUESTRAS];
size_t inicioHistorial = 0;
size_t cantidadHistorial = 0;

// -------------------- Eventos y alertas --------------------
const size_t MAX_EVENTOS = 100;

enum TipoEvento {
  EVENTO_RELE_ENCENDIDO = 1,
  EVENTO_RELE_APAGADO = 2,
  EVENTO_ALERTA_ALTA = 3,
  EVENTO_ALERTA_BAJA = 4
};

enum ZonaTemperatura {
  ZONA_DESCONOCIDA,
  ZONA_BAJA,
  ZONA_RANGO,
  ZONA_ALTA
};

struct Evento {
  uint32_t epoch;
  uint32_t marcaMillis;
  int16_t temperaturaDecimas;
  uint8_t tipo;
};

Evento eventos[MAX_EVENTOS];
size_t inicioEventos = 0;
size_t cantidadEventos = 0;
ZonaTemperatura zonaAnterior = ZONA_DESCONOCIDA;

const char *ARCHIVO_HISTORIAL = "/historial.csv";
const char *ARCHIVO_HISTORIAL_ANTERIOR = "/historial_ant.csv";
const char *ARCHIVO_EVENTOS = "/eventos.csv";
const char *ARCHIVO_EVENTOS_ANTERIOR = "/eventos_ant.csv";
const size_t MAX_ARCHIVO_HISTORIAL = 256UL * 1024UL;
const size_t MAX_ARCHIVO_EVENTOS = 64UL * 1024UL;

// -------------------- Pagina web --------------------
const char *ARCHIVO_PAGINA = "/index.html";

// -------------------- Utilidades de fecha y JSON --------------------
uint32_t obtenerEpoch() {
  time_t ahora = time(NULL);
  if (ahora < 1700000000) {
    return 0;
  }
  return (uint32_t)ahora;
}

String obtenerFechaHora() {
  struct tm informacion;
  if (!getLocalTime(&informacion, 10)) {
    return "Sin sincronizar";
  }

  char texto[24];
  strftime(texto, sizeof(texto), "%d/%m/%Y %H:%M:%S", &informacion);
  return String(texto);
}

String escaparJSON(const String &texto) {
  String resultado;
  resultado.reserve(texto.length() + 8);
  for (size_t i = 0; i < texto.length(); i++) {
    char c = texto.charAt(i);
    if (c == '\\' || c == '"') {
      resultado += '\\';
      resultado += c;
    } else if (c == '\n') {
      resultado += "\\n";
    } else if (c != '\r') {
      resultado += c;
    }
  }
  return resultado;
}

const char *nombreEvento(uint8_t tipo) {
  switch (tipo) {
    case EVENTO_RELE_ENCENDIDO: return "Rele encendido";
    case EVENTO_RELE_APAGADO: return "Rele apagado";
    case EVENTO_ALERTA_ALTA: return "Alerta: temperatura alta";
    case EVENTO_ALERTA_BAJA: return "Alerta: temperatura baja";
    default: return "Evento";
  }
}

// -------------------- Memoria circular --------------------
void establecerLimiteMuestras(size_t nuevoLimite) {
  if (nuevoLimite != 4032 && nuevoLimite != 6000) return;
  if (cantidadHistorial > nuevoLimite) {
    size_t quitar = cantidadHistorial - nuevoLimite;
    inicioHistorial = (inicioHistorial + quitar) % CAPACIDAD_MAX_MUESTRAS;
    cantidadHistorial = nuevoLimite;
  }
  limiteMuestras = nuevoLimite;
}

void agregarMuestraRAM(uint32_t epoch, uint32_t marcaMillis,
                       int16_t temperaturaDecimas, bool estadoRele) {
  size_t posicion = (inicioHistorial + cantidadHistorial) % CAPACIDAD_MAX_MUESTRAS;
  if (cantidadHistorial < limiteMuestras) {
    cantidadHistorial++;
  } else {
    inicioHistorial = (inicioHistorial + 1) % CAPACIDAD_MAX_MUESTRAS;
  }
  historial[posicion].epoch = epoch;
  historial[posicion].marcaMillis = marcaMillis;
  historial[posicion].temperaturaDecimas = temperaturaDecimas;
  historial[posicion].rele = estadoRele ? 1 : 0;
}

void agregarEventoRAM(uint32_t epoch, uint32_t marcaMillis,
                      int16_t temperaturaDecimas, uint8_t tipo) {
  size_t posicion;
  if (cantidadEventos < MAX_EVENTOS) {
    posicion = (inicioEventos + cantidadEventos) % MAX_EVENTOS;
    cantidadEventos++;
  } else {
    posicion = inicioEventos;
    inicioEventos = (inicioEventos + 1) % MAX_EVENTOS;
  }
  eventos[posicion].epoch = epoch;
  eventos[posicion].marcaMillis = marcaMillis;
  eventos[posicion].temperaturaDecimas = temperaturaDecimas;
  eventos[posicion].tipo = tipo;
}

// -------------------- Archivos de registro --------------------
void rotarArchivoSiHaceFalta(const char *ruta, const char *rutaAnterior, size_t limite) {
  if (!littleFSDisponible || !LittleFS.exists(ruta)) return;
  File archivo = LittleFS.open(ruta, "r");
  if (!archivo) return;
  size_t tamano = archivo.size();
  archivo.close();
  if (tamano >= limite) {
    LittleFS.remove(rutaAnterior);
    LittleFS.rename(ruta, rutaAnterior);
  }
}

void guardarMuestraArchivo(const Muestra &muestra) {
  if (!littleFSDisponible) return;
  rotarArchivoSiHaceFalta(ARCHIVO_HISTORIAL, ARCHIVO_HISTORIAL_ANTERIOR,
                          MAX_ARCHIVO_HISTORIAL);
  bool archivoNuevo = !LittleFS.exists(ARCHIVO_HISTORIAL);
  File archivo = LittleFS.open(ARCHIVO_HISTORIAL, "a");
  if (!archivo) return;
  if (archivoNuevo) archivo.println("epoch,temperatura_decimas,rele");
  archivo.print(muestra.epoch);archivo.print(',');
  archivo.print(muestra.temperaturaDecimas);archivo.print(',');
  archivo.println(muestra.rele);archivo.close();
}

void guardarEventoArchivo(const Evento &evento) {
  if (!littleFSDisponible) return;
  rotarArchivoSiHaceFalta(ARCHIVO_EVENTOS, ARCHIVO_EVENTOS_ANTERIOR,
                          MAX_ARCHIVO_EVENTOS);
  bool archivoNuevo = !LittleFS.exists(ARCHIVO_EVENTOS);
  File archivo = LittleFS.open(ARCHIVO_EVENTOS, "a");
  if (!archivo) return;
  if (archivoNuevo) archivo.println("epoch,temperatura_decimas,tipo");
  archivo.print(evento.epoch);archivo.print(',');
  archivo.print(evento.temperaturaDecimas);archivo.print(',');
  archivo.println(evento.tipo);archivo.close();
}

void cargarHistorialDesde(const char *ruta) {
  if (!littleFSDisponible || !LittleFS.exists(ruta)) return;
  File archivo = LittleFS.open(ruta, "r");
  if (!archivo) return;
  while (archivo.available()) {
    String linea = archivo.readStringUntil('\n');linea.trim();
    if (linea.length() == 0 || linea.startsWith("epoch")) continue;
    unsigned long epoch;int temperaturaDecimas;int estadoRele;
    if (sscanf(linea.c_str(), "%lu,%d,%d", &epoch, &temperaturaDecimas, &estadoRele) == 3 &&
        epoch >= 1700000000UL) {
      agregarMuestraRAM((uint32_t)epoch, 0, (int16_t)temperaturaDecimas, estadoRele != 0);
    }
  }
  archivo.close();
}

void cargarEventosDesde(const char *ruta) {
  if (!littleFSDisponible || !LittleFS.exists(ruta)) return;
  File archivo = LittleFS.open(ruta, "r");
  if (!archivo) return;
  while (archivo.available()) {
    String linea = archivo.readStringUntil('\n');linea.trim();
    if (linea.length() == 0 || linea.startsWith("epoch")) continue;
    unsigned long epoch;int temperaturaDecimas;int tipo;
    if (sscanf(linea.c_str(), "%lu,%d,%d", &epoch, &temperaturaDecimas, &tipo) == 3 &&
        epoch >= 1700000000UL) {
      agregarEventoRAM((uint32_t)epoch, 0, (int16_t)temperaturaDecimas, (uint8_t)tipo);
    }
  }
  archivo.close();
}

void cargarRegistros() {
  cargarHistorialDesde(ARCHIVO_HISTORIAL_ANTERIOR);
  cargarHistorialDesde(ARCHIVO_HISTORIAL);
  cargarEventosDesde(ARCHIVO_EVENTOS_ANTERIOR);
  cargarEventosDesde(ARCHIVO_EVENTOS);
}

void registrarEvento(uint8_t tipo, float valorTemperatura) {
  Evento evento;
  evento.epoch = obtenerEpoch();
  evento.marcaMillis = millis();
  evento.temperaturaDecimas = (int16_t)roundf(valorTemperatura * 10.0f);
  evento.tipo = tipo;
  agregarEventoRAM(evento.epoch, evento.marcaMillis, evento.temperaturaDecimas, evento.tipo);
  if (evento.epoch != 0) guardarEventoArchivo(evento);
  versionEventos++;
  Serial.print("EVENTO: ");Serial.print(nombreEvento(tipo));Serial.print(" | ");
  Serial.print(valorTemperatura, 1);Serial.println(" C");
}

void registrarMuestra(float valorTemperatura) {
  Muestra muestra;
  muestra.epoch = obtenerEpoch();
  muestra.marcaMillis = millis();
  muestra.temperaturaDecimas = (int16_t)roundf(valorTemperatura * 10.0f);
  muestra.rele = releEncendido ? 1 : 0;
  agregarMuestraRAM(muestra.epoch, muestra.marcaMillis,
                    muestra.temperaturaDecimas, muestra.rele != 0);
  if (muestra.epoch != 0) guardarMuestraArchivo(muestra);
  versionEventos++;
  ultimaMuestraGuardada = millis();
  muestraGuardadaEnEstaSesion = true;
}

void registrarMuestraSiCorresponde(bool forzar) {
  bool fueraDelRango = temperatura <= temperaturaEncendido ||
                       temperatura >= temperaturaApagado;
  uint32_t intervalo = fueraDelRango ? intervaloFueraSegundos : intervaloDentroSegundos;
  if (forzar || !muestraGuardadaEnEstaSesion ||
      millis() - ultimaMuestraGuardada >= intervalo * 1000UL) {
    registrarMuestra(temperatura);
  }
}

void completarHoraPendiente() {
  uint32_t ahoraEpoch = obtenerEpoch();
  if (ahoraEpoch == 0) return;
  uint32_t ahoraMillis = millis();
  size_t muestrasCorregidas = 0;
  size_t eventosCorregidos = 0;

  for (size_t i = 0; i < cantidadHistorial; i++) {
    size_t posicion = (inicioHistorial + i) % CAPACIDAD_MAX_MUESTRAS;
    Muestra &muestra = historial[posicion];
    if (muestra.epoch == 0 && muestra.marcaMillis != 0) {
      uint32_t antiguedad = (ahoraMillis - muestra.marcaMillis) / 1000UL;
      muestra.epoch = ahoraEpoch > antiguedad ? ahoraEpoch - antiguedad : ahoraEpoch;
      guardarMuestraArchivo(muestra);muestrasCorregidas++;
    }
  }

  for (size_t i = 0; i < cantidadEventos; i++) {
    size_t posicion = (inicioEventos + i) % MAX_EVENTOS;
    Evento &evento = eventos[posicion];
    if (evento.epoch == 0 && evento.marcaMillis != 0) {
      uint32_t antiguedad = (ahoraMillis - evento.marcaMillis) / 1000UL;
      evento.epoch = ahoraEpoch > antiguedad ? ahoraEpoch - antiguedad : ahoraEpoch;
      guardarEventoArchivo(evento);eventosCorregidos++;
    }
  }

  if (eventosCorregidos > 0 || muestrasCorregidas > 0) versionEventos++;
  Serial.print("Fecha y hora disponibles. Muestras corregidas: ");
  Serial.print(muestrasCorregidas);Serial.print(" | Eventos corregidos: ");
  Serial.println(eventosCorregidos);
}

void revisarHora() {
  if (horaConfirmada || obtenerEpoch() == 0) return;
  horaConfirmada = true;
  completarHoraPendiente();
}

// -------------------- Pantalla OLED --------------------
void actualizarOLED() {
  if (!oledDisponible) return;
  display.clearDisplay();display.setTextColor(WHITE);display.setTextSize(1);
  if (lecturaValida) {
    display.setCursor(0, 0);display.print(temperatura, 1);display.println(" C");
    display.setCursor(0, 50);display.print("Rele: ");
    display.println(releEncendido ? "ENCENDIDO" : "APAGADO");
  } else {
    display.setCursor(0, 25);display.println("Error leyendo DHT22");
  }
  display.display();
}

// -------------------- Control del rele y alertas --------------------
bool controlarRele(float valorTemperatura) {
  bool nuevoEstado = releEncendido;
  if (valorTemperatura >= temperaturaApagado) nuevoEstado = false;
  else if (valorTemperatura <= temperaturaEncendido) nuevoEstado = true;
  if (nuevoEstado == releEncendido) return false;
  releEncendido = nuevoEstado;
  digitalWrite(RELE1, releEncendido ? HIGH : LOW);
  registrarEvento(releEncendido ? EVENTO_RELE_ENCENDIDO : EVENTO_RELE_APAGADO,
                  valorTemperatura);
  return true;
}

bool actualizarAlertas(float valorTemperatura) {
  ZonaTemperatura nuevaZona;
  if (valorTemperatura >= temperaturaApagado) nuevaZona = ZONA_ALTA;
  else if (valorTemperatura <= temperaturaEncendido) nuevaZona = ZONA_BAJA;
  else nuevaZona = ZONA_RANGO;
  bool nuevaAlerta = false;
  if (nuevaZona == ZONA_ALTA && zonaAnterior != ZONA_ALTA) {
    registrarEvento(EVENTO_ALERTA_ALTA, valorTemperatura);nuevaAlerta = true;
  } else if (nuevaZona == ZONA_BAJA && zonaAnterior != ZONA_BAJA) {
    registrarEvento(EVENTO_ALERTA_BAJA, valorTemperatura);nuevaAlerta = true;
  }
  zonaAnterior = nuevaZona;
  return nuevaAlerta;
}

// -------------------- Sensor --------------------
void leerSensor() {
  float nuevaTemperatura = dht.readTemperature();
  if (isnan(nuevaTemperatura)) {
    lecturaValida = false;Serial.println("Error al leer el DHT22");actualizarOLED();return;
  }
  temperatura = nuevaTemperatura;lecturaValida = true;
  bool cambioRele = controlarRele(temperatura);
  bool nuevaAlerta = actualizarAlertas(temperatura);
  registrarMuestraSiCorresponde(cambioRele || nuevaAlerta);
  Serial.printf("Temperatura: %.1f C | Rele: %s\n", temperatura,
                releEncendido ? "ON" : "OFF");
  actualizarOLED();
}

// -------------------- WiFi y hora --------------------
void iniciarHoraInternet() {
  if (ntpIniciado) return;
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  ntpIniciado = true;Serial.println("Sincronizacion de fecha y hora iniciada");
}

void conectarWiFi() {
  iniciarHoraInternet();
  modoAP = false;WiFi.mode(WIFI_STA);WiFi.begin(wifiSSID.c_str(), wifiClave.c_str());
  Serial.print("Conectando a WiFi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000UL) {
    if (millis() - ultimaLectura >= INTERVALO_LECTURA) {
      ultimaLectura = millis();leerSensor();
    }
    delay(100);Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP del ESP32: ");Serial.println(WiFi.localIP());iniciarHoraInternet();return;
  }
  WiFi.disconnect();WiFi.mode(WIFI_AP_STA);WiFi.softAP(AP_SSID, AP_CLAVE);modoAP = true;
  Serial.println("No se pudo conectar a la red configurada");
  Serial.print("Red de configuracion: ");Serial.println(AP_SSID);
  Serial.print("IP de configuracion: ");Serial.println(WiFi.softAPIP());
}

void revisarWiFi() {
  if (millis() - ultimaRevisionWiFi < 30000UL) return;
  ultimaRevisionWiFi = millis();
  if (!modoAP && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado; intentando reconectar");WiFi.reconnect();
  } else if (WiFi.status() == WL_CONNECTED) iniciarHoraInternet();
}

// -------------------- Servidor web --------------------
// Entrega la pagina guardada en LittleFS (data/index.html).
void handleRoot() {
  if (littleFSDisponible && LittleFS.exists(ARCHIVO_PAGINA)) {
    File archivo = LittleFS.open(ARCHIVO_PAGINA, "r");
    if (archivo) {
      server.sendHeader("Cache-Control", "no-cache");
      server.streamFile(archivo, "text/html; charset=utf-8");
      archivo.close();
      return;
    }
  }
  server.send(500, "text/plain; charset=utf-8",
              "No se encontro /index.html en la memoria del ESP32.\n"
              "Subi la carpeta data con: Upload Filesystem Image (PlatformIO: uploadfs).");
}

void handleDatos() {
  String ip = modoAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String ssidActual = modoAP ? String(AP_SSID) : WiFi.SSID();
  String json;json.reserve(440);json = "{";
  json += "\"ok\":";json += lecturaValida ? "true" : "false";
  json += ",\"temperatura\":";json += lecturaValida ? String(temperatura, 1) : "null";
  json += ",\"rele\":";json += releEncendido ? "true" : "false";
  json += ",\"hora\":\"" + escaparJSON(obtenerFechaHora()) + "\"";
  json += ",\"horaValida\":";json += obtenerEpoch() != 0 ? "true" : "false";
  json += ",\"versionEventos\":" + String(versionEventos);
  json += ",\"modo\":\"" + String(modoAP ? "Punto de acceso" : "WiFi") + "\"";
  json += ",\"ssid\":\"" + escaparJSON(ssidActual) + "\"";
  json += ",\"ssidConfigurado\":\"" + escaparJSON(wifiSSID) + "\"";
  json += ",\"ip\":\"" + ip + "\"";
  json += ",\"intervaloDentro\":" + String(intervaloDentroSegundos);
  json += ",\"intervaloFuera\":" + String(intervaloFueraSegundos);
  json += ",\"limiteMuestras\":" + String(limiteMuestras);
  json += ",\"temperaturaEncendido\":" + String(temperaturaEncendido, 1);
  json += ",\"temperaturaApagado\":" + String(temperaturaApagado, 1);
  json += "}";
  server.sendHeader("Cache-Control", "no-store");server.send(200, "application/json", json);
}

void handleHistorial() {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200, "application/json", "");
  server.sendContent("[");
  String bloque;
  bloque.reserve(1024);
  size_t enviados = 0;
  for (size_t i = 0; i < cantidadHistorial; i++) {
    size_t posicion = (inicioHistorial + i) % CAPACIDAD_MAX_MUESTRAS;
    const Muestra &muestra = historial[posicion];
    if (muestra.epoch == 0) continue;
    String item;item.reserve(64);if (enviados > 0) item += ',';
    item += "{\"t\":" + String(muestra.epoch);
    item += ",\"v\":" + String(muestra.temperaturaDecimas / 10.0f, 1);
    item += ",\"r\":";item += muestra.rele ? "true" : "false";item += "}";
    if (bloque.length() + item.length() > 1000) {
      server.sendContent(bloque);
      bloque = "";
    }
    bloque += item;
    enviados++;
  }
  if (bloque.length() > 0) server.sendContent(bloque);
  server.sendContent("]");server.sendContent("");
}

void handleEventos() {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);server.send(200, "application/json", "");
  server.sendContent("[");
  String bloque;
  bloque.reserve(1024);
  size_t enviados = 0;
  for (size_t i = 0; i < cantidadEventos; i++) {
    size_t posicion = (inicioEventos + i) % MAX_EVENTOS;
    const Evento &evento = eventos[posicion];
    if (evento.epoch == 0) continue;
    String item;item.reserve(110);if (enviados > 0) item += ',';
    item += "{\"t\":" + String(evento.epoch);
    item += ",\"v\":" + String(evento.temperaturaDecimas / 10.0f, 1);
    item += ",\"tipo\":" + String(evento.tipo);
    item += ",\"evento\":\"" + escaparJSON(String(nombreEvento(evento.tipo))) + "\"}";
    if (bloque.length() + item.length() > 1000) {
      server.sendContent(bloque);
      bloque = "";
    }
    bloque += item;
    enviados++;
  }
  if (bloque.length() > 0) server.sendContent(bloque);
  server.sendContent("]");server.sendContent("");
}

void handleGuardarWiFi() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain; charset=utf-8", "Falta el nombre de la red WiFi.");return;
  }
  String nuevoSSID = server.arg("ssid");
  String nuevaClave = server.hasArg("clave") ? server.arg("clave") : "";nuevoSSID.trim();
  if (nuevaClave.length() == 0 && nuevoSSID == wifiSSID) nuevaClave = wifiClave;
  if (nuevoSSID.length() == 0 || nuevoSSID.length() > 32 || nuevaClave.length() > 63) {
    server.send(400, "text/plain; charset=utf-8", "Los datos WiFi no son validos.");return;
  }
  preferencias.putString("ssid", nuevoSSID);preferencias.putString("clave", nuevaClave);
  server.send(200, "text/plain; charset=utf-8",
              "WiFi guardado. El ESP32 se reiniciara y tratara de conectarse.");
  reinicioPendiente = true;momentoReinicio = millis() + 1500UL;
}

void handleAjustarHora() {
  if (obtenerEpoch() != 0) {
    revisarHora();
    server.send(200, "text/plain; charset=utf-8", "La hora ya esta sincronizada.");return;
  }
  if (!server.hasArg("epoch")) {
    server.send(400, "text/plain; charset=utf-8", "Falta la fecha y hora.");return;
  }
  String textoEpoch = server.arg("epoch");
  char *fin;
  unsigned long nuevaHora = strtoul(textoEpoch.c_str(), &fin, 10);
  if (*fin != '\0' || nuevaHora < 1700000000UL || nuevaHora > 4102444800UL) {
    server.send(400, "text/plain; charset=utf-8", "Fecha y hora no validas.");return;
  }
  struct timeval tiempo;
  tiempo.tv_sec = (time_t)nuevaHora;tiempo.tv_usec = 0;
  settimeofday(&tiempo, NULL);
  revisarHora();
  server.send(200, "text/plain; charset=utf-8", "Fecha y hora recibidas del navegador.");
}

void handleConfigRegistro() {
  if (!server.hasArg("dentro") || !server.hasArg("fuera") || !server.hasArg("limite")) {
    server.send(400, "text/plain; charset=utf-8", "Falta la configuracion del historial.");return;
  }
  long minutosDentro = server.arg("dentro").toInt();
  long segundosFuera = server.arg("fuera").toInt();
  long nuevoLimite = server.arg("limite").toInt();
  if (minutosDentro < 1 || minutosDentro > 60 || segundosFuera < 10 ||
      segundosFuera > 600 || (nuevoLimite != 4032 && nuevoLimite != 6000)) {
    server.send(400, "text/plain; charset=utf-8", "Intervalos fuera de los limites permitidos.");return;
  }
  intervaloDentroSegundos = (uint32_t)minutosDentro * 60UL;
  intervaloFueraSegundos = (uint32_t)segundosFuera;
  establecerLimiteMuestras((size_t)nuevoLimite);
  preferencias.putUInt("int_rango", intervaloDentroSegundos);
  preferencias.putUInt("int_fuera", intervaloFueraSegundos);
  preferencias.putUInt("max_muestras", (uint32_t)limiteMuestras);
  versionEventos++;
  server.send(200, "text/plain; charset=utf-8", "Configuracion del historial guardada.");
}

bool convertirTemperatura(const String &texto, float &valor) {
  const char *inicio = texto.c_str();
  char *fin;
  float numero = strtof(inicio, &fin);
  while (*fin == ' ' || *fin == '\t') fin++;
  if (fin == inicio || *fin != '\0' || isnan(numero) || isinf(numero)) return false;
  valor = roundf(numero * 10.0f) / 10.0f;
  return true;
}

void handleConfigTemperatura() {
  if (!server.hasArg("encendido") || !server.hasArg("apagado")) {
    server.send(400, "text/plain; charset=utf-8", "Faltan las temperaturas.");return;
  }
  float nuevoEncendido;
  float nuevoApagado;
  if (!convertirTemperatura(server.arg("encendido"), nuevoEncendido) ||
      !convertirTemperatura(server.arg("apagado"), nuevoApagado)) {
    server.send(400, "text/plain; charset=utf-8", "Las temperaturas no son validas.");return;
  }
  if (nuevoEncendido < -40.0f || nuevoApagado > 80.0f ||
      nuevoApagado - nuevoEncendido < 0.1f) {
    server.send(400, "text/plain; charset=utf-8",
                "El encendido debe ser menor que el apagado. Rango permitido: -40 a 80 C.");return;
  }
  temperaturaEncendido = nuevoEncendido;
  temperaturaApagado = nuevoApagado;
  preferencias.putFloat("temp_on", temperaturaEncendido);
  preferencias.putFloat("temp_off", temperaturaApagado);

  if (lecturaValida) {
    zonaAnterior = ZONA_DESCONOCIDA;
    bool cambioRele = controlarRele(temperatura);
    bool nuevaAlerta = actualizarAlertas(temperatura);
    registrarMuestraSiCorresponde(cambioRele || nuevaAlerta);
    actualizarOLED();
  }

  String respuesta = "Temperaturas guardadas. Enciende a ";
  respuesta += String(temperaturaEncendido, 1);
  respuesta += " C y apaga a ";
  respuesta += String(temperaturaApagado, 1);
  respuesta += " C.";
  server.send(200, "text/plain; charset=utf-8", respuesta);
}

void handleBorrarRegistros() {
  if (littleFSDisponible) {
    LittleFS.remove(ARCHIVO_HISTORIAL);LittleFS.remove(ARCHIVO_HISTORIAL_ANTERIOR);
    LittleFS.remove(ARCHIVO_EVENTOS);LittleFS.remove(ARCHIVO_EVENTOS_ANTERIOR);
  }
  inicioHistorial = 0;cantidadHistorial = 0;inicioEventos = 0;cantidadEventos = 0;
  muestraGuardadaEnEstaSesion = false;
  server.send(200, "text/plain; charset=utf-8", "Registros borrados.");
}

void configurarServidor() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/datos", HTTP_GET, handleDatos);
  server.on("/historial", HTTP_GET, handleHistorial);
  server.on("/eventos", HTTP_GET, handleEventos);
  server.on("/ajustar-hora", HTTP_POST, handleAjustarHora);
  server.on("/wifi", HTTP_POST, handleGuardarWiFi);
  server.on("/config-registro", HTTP_POST, handleConfigRegistro);
  server.on("/config-temperatura", HTTP_POST, handleConfigTemperatura);
  server.on("/borrar-registros", HTTP_POST, handleBorrarRegistros);
  server.begin();Serial.println("Servidor web iniciado");
}

void setup() {
  Serial.begin(115200);delay(500);
  pinMode(RELE1, OUTPUT);digitalWrite(RELE1, LOW);
  dht.begin();delay(2000);
  Wire.begin(21, 22);display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  oledDisponible = true;actualizarOLED();Serial.println("OLED iniciada");

  preferencias.begin("lucas-control", false);
  wifiSSID = preferencias.getString("ssid", WIFI_SSID_INICIAL);
  wifiClave = preferencias.getString("clave", WIFI_CLAVE_INICIAL);
  intervaloDentroSegundos = preferencias.getUInt("int_rango", 300);
  intervaloFueraSegundos = preferencias.getUInt("int_fuera", 60);
  uint32_t limiteGuardado = preferencias.getUInt("max_muestras", LIMITE_MUESTRAS_INICIAL);
  establecerLimiteMuestras((size_t)limiteGuardado);
  temperaturaEncendido = preferencias.getFloat("temp_on", TEMPERATURA_ENCENDIDO_INICIAL);
  temperaturaApagado = preferencias.getFloat("temp_off", TEMPERATURA_APAGADO_INICIAL);
  if (intervaloDentroSegundos < 60 || intervaloDentroSegundos > 3600) intervaloDentroSegundos = 300;
  if (intervaloFueraSegundos < 10 || intervaloFueraSegundos > 600) intervaloFueraSegundos = 60;
  if (temperaturaEncendido < -40.0f || temperaturaApagado > 80.0f ||
      temperaturaApagado - temperaturaEncendido < 0.1f) {
    temperaturaEncendido = TEMPERATURA_ENCENDIDO_INICIAL;
    temperaturaApagado = TEMPERATURA_APAGADO_INICIAL;
  }

  littleFSDisponible = LittleFS.begin(true);
  if (littleFSDisponible) {
    cargarRegistros();Serial.println("Memoria de registros iniciada");
    if (!LittleFS.exists(ARCHIVO_PAGINA)) {
      Serial.println("AVISO: falta /index.html. Subi la carpeta data (uploadfs).");
    }
  } else Serial.println("No se pudo iniciar LittleFS; los registros funcionaran solo en RAM");

  iniciarHoraInternet();
  leerSensor();ultimaLectura = millis();
  conectarWiFi();configurarServidor();
}

void loop() {
  server.handleClient();
  revisarHora();
  if (millis() - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = millis();leerSensor();
  }
  revisarWiFi();
  if (reinicioPendiente && (long)(millis() - momentoReinicio) >= 0) ESP.restart();
  delay(2);
}