// librerias
#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

//declaraciones
#define RELE1 4
//#define RELE2 21 
//#define RELECOLO 19 
#define DHTPIN 18
#define DHTTYPE DHT22
#define Led1 2 
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// funciones y objetos
void sensordesconectado();
void sensorfuncionando();
DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
    Serial.begin(115200);
    pinMode(RELE1, OUTPUT);
    //pinMode(RELECOLO, OUTPUT);
    digitalWrite(RELE1, LOW);
    dht.begin();
    pinMode(Led1, OUTPUT);
    // Inicializar la pantalla OLED con la dirección I2C 0x3C
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("Fallo al asignar SSD1306"));
        for(;;);
    }
    display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

}

void loop() {
    
    float t = dht.readTemperature();
    //digitalWrite(RELECOLO,HIGH);
    if (isnan(t)) {
        Serial.println("Error al leer el DHT22");
        sensordesconectado();
    } else {
        Serial.print("Temperatura: ");
        Serial.print(t);
        Serial.println(" C");
        sensorfuncionando();
        display.clearDisplay();
        display.setTextSize(2);
        display.setCursor(0, 20);
        display.print(F("Temp: "));
        display.print(t);
        display.println(F(" C")); 
    }
    if (t < 37.7) {
            digitalWrite(RELE1, HIGH);
    } else {
            digitalWrite(RELE1, LOW);
    }
    display.display();
}

//funciones 

void sensordesconectado(){
    digitalWrite(Led1,HIGH);
    delay(500);
    digitalWrite(Led1,LOW);
    delay(500);

}
void sensorfuncionando()
{
    digitalWrite(Led1,HIGH);
    delay(5000);
    digitalWrite(Led1,LOW);
    delay(5000);
}
