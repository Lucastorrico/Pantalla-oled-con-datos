#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

int numeroActual = 0;

void setup() {
  Serial.begin(9600);
  
  // Inicializar pantalla
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Error al inicializar SSD1306"));
    for(;;); // Detener
  }
  mostrarNumero();
}

void mostrarNumero() {
  display.clearDisplay();
  display.setTextSize(3);             // Tamaño del texto
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(30, 20);          // Posición del texto
  display.print(numeroActual);        // Muestra el número
  display.display();
}

void loop() {
  if (Serial.available()) {
    char comando = Serial.read();

    if (comando == ' ') {           // subir
      numeroActual++;
      mostrarNumero();
    } else if (comando == 'x' || comando == 'X') {  // bajar
      numeroActual--;
      mostrarNumero();
    }
  }
}
