// Pruebas de src/beeper.h en el ordenador (no hace falta la placa).
//
//   g++ -std=c++17 -Wall -Wextra -Isrc tools/test_beeper.cpp -o /tmp/test_beeper && /tmp/test_beeper
#include <cstdio>
#include "beeper.h"

static int failures = 0;
#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) { printf("FALLA l.%d: %s\n", __LINE__, #cond); failures++; } \
  } while (0)

static const uint32_t MIN = 60UL * 1000UL;

// Simula `minutes` minutos con una lectura por minuto (como el firmware) y cuenta los
// pitidos nuevos. Cada minuto se evalúa el pitido a lo largo de 5 s en pasos de 100 ms.
static int simulate(beeper::Controller& c, uint32_t& t, int minutes, int value, int low, int high) {
  int beeps = 0;
  bool wasOn = false;
  for (int m = 0; m < minutes; m++) {
    c.onReading(t, value, low, high);
    for (int ms = 0; ms < 60000; ms += 100) {
      bool on = c.tick(t + ms);
      if (on && !wasOn) beeps++;
      wasOn = on;
    }
    t += MIN;
  }
  return beeps;
}

static void testRun(uint32_t start) {
  // --- dentro del rango: nunca suena ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 120, 100, 70, 180) == 0);
    CHECK(!c.inEpisode());
  }
  // --- justo en los límites cuenta como dentro del rango ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 10, 70, 70, 180) == 0);
    CHECK(simulate(c, t, 10, 180, 70, 180) == 0);
    CHECK(simulate(c, t, 10, 69, 70, 180) == 1);   // 1 por debajo: suena
  }
  // --- bajo: pitido inmediato de exactamente 5 s ---
  {
    beeper::Controller c; uint32_t t = start;
    c.onReading(t, 60, 70, 180);
    CHECK(c.tick(t));
    CHECK(c.tick(t + 4999));
    CHECK(!c.tick(t + 5000));
    CHECK(!c.tick(t + 20000));
  }
  // --- fuera de rango sostenido: 1 pitido al empezar y otro cada 30 min ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 29, 55, 70, 180) == 1);   // minutos 0-28: solo el inicial
    CHECK(simulate(c, t, 2, 55, 70, 180) == 1);    // minuto 30: segundo pitido
    CHECK(simulate(c, t, 28, 55, 70, 180) == 0);   // hasta el 59: silencio
    CHECK(simulate(c, t, 2, 55, 70, 180) == 1);    // minuto 60: tercero
  }
  // --- alto: igual que bajo ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 61, 250, 70, 180) == 3);  // minutos 0, 30 y 60
  }
  // --- vuelve al rango: nunca más suena, aunque pasen 30 min ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 5, 55, 70, 180) == 1);
    CHECK(simulate(c, t, 120, 110, 70, 180) == 0);
    CHECK(!c.inEpisode());
  }
  // --- vuelve al rango durante el pitido: se corta al instante ---
  {
    beeper::Controller c; uint32_t t = start;
    c.onReading(t, 55, 70, 180);
    CHECK(c.tick(t + 1000));
    c.onReading(t + 2000, 110, 70, 180);
    CHECK(!c.tick(t + 2000));
    CHECK(!c.tick(t + 2100));
  }
  // --- sale, vuelve, y sale otra vez: nuevo episodio con pitido inmediato ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 3, 55, 70, 180) == 1);
    CHECK(simulate(c, t, 3, 100, 70, 180) == 0);
    CHECK(simulate(c, t, 3, 55, 70, 180) == 1);
  }
  // --- bajo -> alto sin pasar por el rango: sigue siendo el mismo episodio ---
  {
    beeper::Controller c; uint32_t t = start;
    CHECK(simulate(c, t, 5, 55, 70, 180) == 1);
    CHECK(simulate(c, t, 5, 250, 70, 180) == 0);
  }
  // --- reset() corta el pitido y olvida el episodio ---
  {
    beeper::Controller c; uint32_t t = start;
    c.onReading(t, 55, 70, 180);
    c.reset();
    CHECK(!c.tick(t + 10));
    c.onReading(t + 20, 55, 70, 180);
    CHECK(c.tick(t + 30));
  }
  // --- llamar muy a menudo a onReading con el mismo valor no repite el pitido ---
  {
    beeper::Controller c; uint32_t t = start;
    int starts = 0; bool was = false;
    for (int i = 0; i < 20000; i++) {   // 20 000 x 100 ms = 33 min
      c.onReading(t + i * 100UL, 55, 70, 180);
      bool on = c.tick(t + i * 100UL);
      if (on && !was) starts++;
      was = on;
    }
    CHECK(starts == 2);                 // minuto 0 y minuto 30
  }
}

int main() {
  testRun(0);
  testRun(123456789UL);
  testRun(0xFFFFFFFFUL - 90UL * MIN);   // atraviesa el desbordamiento de millis()
  testRun(0xFFFFFFFFUL - 1000UL);       // desborda a los 1000 ms
  printf(failures ? "\n%d comprobaciones fallidas\n" : "Todas las pruebas correctas\n", failures);
  return failures ? 1 : 0;
}
