/*
 * Lógica de la alarma sonora, sin nada de hardware: solo decide CUÁNDO suena.
 * Se prueba en el ordenador con tools/test_beeper.cpp.
 *
 * Reglas:
 *   - Dentro del rango (bajo <= valor <= alto): nunca suena.
 *   - Al salir del rango: suena BEEP_MS (5 s) enseguida.
 *   - Si sigue fuera, vuelve a sonar BEEP_MS cuando pasan REPEAT_MS (30 min) desde el
 *     último pitido, y así indefinidamente.
 *   - Si vuelve al rango, se corta el pitido (si estaba sonando) y se olvida todo:
 *     una nueva salida del rango vuelve a empezar con un pitido inmediato.
 *
 * Solo hay que llamar a onReading() con lecturas VIGENTES (no con datos obsoletos),
 * y a tick() con frecuencia para que el pitido termine a tiempo.
 * Todas las comparaciones de tiempo toleran el desbordamiento de millis() (~49 días).
 */
#pragma once

#include <stdint.h>

namespace beeper {

constexpr uint32_t BEEP_MS = 5000UL;                  // duración de cada pitido
constexpr uint32_t REPEAT_MS = 30UL * 60UL * 1000UL;  // pausa entre pitidos (30 min)

class Controller {
 public:
  // Olvida el episodio actual y corta cualquier pitido.
  void reset() {
    episode_ = false;
    beeping_ = false;
  }

  // Llamar con la lectura vigente. Se puede llamar a cada vuelta del bucle.
  void onReading(uint32_t now, int value, int low, int high) {
    const bool outOfRange = value < low || value > high;
    if (!outOfRange) {
      reset();
      return;
    }
    if (!episode_) {
      episode_ = true;
      startBeep(now);
    } else if (diff(now, nextAt_) >= 0) {
      startBeep(now);
    }
  }

  // Devuelve true mientras el altavoz debe estar sonando.
  bool tick(uint32_t now) {
    if (beeping_ && diff(now, beepEnd_) >= 0) beeping_ = false;
    return beeping_;
  }

  bool inEpisode() const { return episode_; }

 private:
  static int32_t diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

  void startBeep(uint32_t now) {
    beeping_ = true;
    beepEnd_ = now + BEEP_MS;
    nextAt_ = now + REPEAT_MS;
  }

  bool episode_ = false;
  bool beeping_ = false;
  uint32_t beepEnd_ = 0;
  uint32_t nextAt_ = 0;
};

}  // namespace beeper
