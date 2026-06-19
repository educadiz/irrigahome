// Responsabilidade: declarar a interface de apresentacao no display TFT.
// O que faz: expor metodos para iniciar o display, mostrar splash e atualizar tela.

#pragma once

class DisplayManager {
public:
    void begin();
    void showSplash();
    void update();
};