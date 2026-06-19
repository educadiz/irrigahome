// Responsabilidade: implementar a renderizacao de status no TFT ST7789.
// O que faz: inicializa o barramento SPI/display, mostra splash e desenha telemetria,
// estado da bomba/modo, conectividade MQTT e quantidade de agendamentos.
//
// CORRECAO DE FLICKER:
//  - Dirty flags por secao: cada bloco so e redesenhado quando seu dado muda
//  - Cache de strings renderizadas: evita clear+redraw quando valor nao mudou
//  - Header removido do loop de renderDashboard (era desenhado 2x por frame)
//  - Footer com cache de estado wifi/mqtt e timestamp (hh:mm)

#include "display_manager.h"
#include "config.h"
#include "sensor_manager.h"
#include "actuator_manager.h"
#include "mqtt_manager.h"
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>
#include <string.h>

extern SensorManager sensores;
extern ActuatorManager atuador;
extern MqttManager mqtt;

Adafruit_ST7789 display(DISPLAY_CS_PIN, DISPLAY_DC_PIN, DISPLAY_RST_PIN);

static unsigned long lastFrameTime   = 0;
static unsigned long lastBodyRefresh = 0;
static bool displayReady    = false;
static bool bodyInitialized = false;
static bool dashboardPainted = false;
static unsigned long splashStartedAt = 0;
static bool splashShown = false;
static char headerIpText[24] = "";

// ── Cabeçalho rotativo: "Irriga Home" → IP → MAC ────────────────────────────
// Cada slide fica visível por 3 s; há um breve apagão de 150 ms entre eles.
enum HeaderSlide : uint8_t { SLIDE_NAME = 0, SLIDE_IP = 1, SLIDE_MAC = 2, SLIDE_COUNT = 3 };
static HeaderSlide  headerSlide          = SLIDE_NAME;
static unsigned long headerSlideStart    = 0;    // millis() do início do slide atual
static bool          headerSlideVisible  = true; // false durante o apagão entre slides
static unsigned long headerBlankStart    = 0;    // millis() do início do apagão
static const unsigned long HEADER_SLIDE_MS  = 3000UL;
static const unsigned long HEADER_BLANK_MS  =  150UL;

static const unsigned long FRAME_INTERVAL_MS       = 200UL;
static const unsigned long BODY_REFRESH_INTERVAL_MS = 1000UL;
static const int HEADER_AREA_X                     = 8;
static const int HEADER_AREA_Y                     = 8;
static const int HEADER_AREA_W                     = SCREEN_WIDTH - 16;
static const int HEADER_AREA_H                     = 28;
static const int HEADER_INNER_X                    = HEADER_AREA_X + 1;
static const int HEADER_INNER_Y                    = HEADER_AREA_Y + 1;
static const int HEADER_INNER_W                    = SCREEN_WIDTH - 18;
static const int HEADER_INNER_H                    = HEADER_AREA_H - 2;
static const char* const HEADER_IP_PREFIX          = "IP: ";

// ── Cores ────────────────────────────────────────────────────────────────────
static const uint16_t COLOR_BG          = ST77XX_BLACK;
static const uint16_t COLOR_TEXT        = ST77XX_WHITE;
static const uint16_t COLOR_CARD        = 0x1082;
static const uint16_t COLOR_HEADER_DARK = 0x0223;
static const uint16_t COLOR_ALERT       = 0xF800;
static const uint16_t COLOR_PANEL_EDGE  = 0x52AA;//0x39e7
static const uint16_t COLOR_SOIL_BG     = 0x0F42;
static const uint16_t COLOR_SOIL_EDGE   = 0x2FE3;
static const uint16_t COLOR_OK          = 0x07E0;
static const uint16_t COLOR_DIM         = 0x7BEF;
static const uint16_t COLOR_ORANGE      = 0xFB80;
static const uint16_t COLOR_CYAN        = 0x07FF;

// ── Cache de dados lidos ─────────────────────────────────────────────────────
static SensorData cachedData      = {0, 0, 0, false};
static int        cachedDuracao   = 0;
static int        cachedAgendamentos = 0;

// ── Cache de strings já renderizadas (dirty-flag por valor) ─────────────────
static char prevTempText[16]     = "";
static char prevHumText[16]      = "";
static char prevSoilText[16]     = "";
static bool prevNivelAgua        = false;
static bool prevPumpOn           = false;
static bool prevModoAuto         = false;
static char prevDuracao[16]      = "";
static int  prevAgendamentos     = -1;
static bool prevMqttConn         = false;
static bool prevWifiConn         = false;
static char prevTimeText[8]       = "";
static char prevDateText[20]    = ""; // "Ter 05/05/26" + margem

// ── Helpers ──────────────────────────────────────────────────────────────────
static void initializeSpiBus() {
    SPI.begin(DISPLAY_SCL_PIN, -1, DISPLAY_SDA_PIN, DISPLAY_CS_PIN);
}

static bool initializeDisplay() {
    initializeSpiBus();
    display.init(SCREEN_WIDTH, SCREEN_HEIGHT);
    display.setRotation(0);
    display.fillScreen(COLOR_BG);
    display.setTextWrap(false);
    displayReady = true;
    return true;
}

static void drawCenteredText(int centerX, int y, uint8_t size, const char* text, uint16_t color) {
    display.setTextSize(size);
    display.setTextColor(color);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(centerX - (int)w / 2, y);
    display.print(text);
}

static void drawCardValueCentered(int x, int y, int w, int h, const char* value, uint16_t color, uint8_t size) {
    display.setTextSize(size);
    display.setTextColor(color);
    int16_t x1, y1;
    uint16_t textW, textH;
    display.getTextBounds(value, 0, 0, &x1, &y1, &textW, &textH);
    display.setCursor(x + (w - (int)textW) / 2, y + (h - (int)textH) / 2);
    display.print(value);
}

static void drawPanel(int x, int y, int w, int h, uint16_t fillColor, uint16_t edgeColor) {
    display.fillRoundRect(x, y, w, h, 10, fillColor);
    display.drawRoundRect(x, y, w, h, 10, edgeColor);
}

static void drawStatusDot(int x, int y, uint16_t color) {
    display.fillCircle(x, y, 3, color);
    display.drawCircle(x, y, 3, COLOR_TEXT);
}

static void drawLeafIcon(int x, int y, uint16_t color) {
    display.fillTriangle(x + 4, y + 1, x + 12, y + 3, x + 5, y + 12, color);
    display.fillTriangle(x + 8, y + 2, x + 16, y + 6, x + 9, y + 14, color);
    display.drawLine(x + 4, y + 12, x + 14, y + 2, COLOR_TEXT);
}

static void drawThermometerIcon(int x, int y, uint16_t color) {
    display.drawRoundRect(x + 4, y + 1, 3, 12, 1, color);
    display.fillCircle(x + 5, y + 14, 3, color);
    display.fillRect(x + 4, y + 4, 1, 8, color);
}

static void drawDropletIcon(int x, int y, uint16_t color) {
    display.fillTriangle(x + 6, y, x + 2, y + 8, x + 10, y + 8, color);
    display.fillCircle(x + 6, y + 7, 4, color);
}

static void drawTankIcon(int x, int y, uint16_t color) {
    display.drawRoundRect(x + 1, y + 1, 14, 16, 2, color);
    display.fillRect(x + 4, y + 5, 8, 7, color);
    display.drawFastHLine(x + 4, y + 13, 8, color);
}

static void drawPumpIcon(int x, int y, uint16_t color) {
    display.drawRoundRect(x + 1, y + 3, 14, 12, 2, color);
    display.fillRect(x + 4, y + 5, 8, 5, color);
    display.drawCircle(x + 8, y + 8, 3, color);
}

static void clearValueArea(int x, int y, int w, int h, uint16_t fillColor) {
    display.fillRect(x, y, w, h, fillColor);
}

static void drawHeaderBackground() {
    drawPanel(HEADER_AREA_X, HEADER_AREA_Y, HEADER_AREA_W, HEADER_AREA_H, COLOR_HEADER_DARK, COLOR_PANEL_EDGE);
}

// Apaga somente a área interna do cabeçalho (preserva a borda do painel).
static void clearHeaderInner() {
    display.fillRect(HEADER_INNER_X, HEADER_INNER_Y,
                     HEADER_INNER_W, HEADER_INNER_H,
                     COLOR_HEADER_DARK);
}

// Desenha `text` centralizado (H+V) dentro do cabeçalho.
// Usa a fonte bitmap nativa do Adafruit GFX:
//   size 2 → glifos 12×16px — ocupa ~16px de altura, cabe nos 26px internos com ~5px de margem
//   size 1 → glifos 6×8px  — usado para strings longas (IP, MAC) com espaçamento entre chars
static void drawCarouselText(const char* text, uint8_t textSize) {
    clearHeaderInner();

    display.setFont(nullptr);          // garante fonte bitmap nativa
    display.setTextSize(textSize);
    display.setTextColor(COLOR_TEXT);

    int16_t x1, y1;
    uint16_t textW, textH;
    display.getTextBounds(text, 0, 0, &x1, &y1, &textW, &textH);

    int drawX = HEADER_INNER_X + ((HEADER_INNER_W - (int)textW) / 2) - x1;
    int drawY = HEADER_INNER_Y + ((HEADER_INNER_H - (int)textH) / 2) - y1;

    display.setCursor(drawX, drawY);
    display.print(text);
}

// Monta o texto do slide atual e chama drawCarouselText().
// Chamada APENAS quando o slide precisa ser (re)desenhado.
static void paintHeaderSlide() {
    char text[32];
    uint8_t sz;

    switch (headerSlide) {
        case SLIDE_NAME:
            // "Irriga Home" — 11 chars × 12px = 132px, cabe nos ~222px de largura interna
            strncpy(text, "Irriga Home", sizeof(text) - 1);
            sz = 2;
            break;

        case SLIDE_IP: {
            // Ex.: "192.168.0.10" — até 15 chars × 12px = 180px, usa size 2 sem problema
            if (WiFi.status() == WL_CONNECTED) {
                IPAddress ip = WiFi.localIP();
                snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            } else {
                strncpy(text, "Sem WiFi", sizeof(text) - 1);
            }
            sz = 2;
            break;
        }

        case SLIDE_MAC: {
            // "aa:bb:cc:dd:ee:ff" — 17 chars × 12px (size 2) = 204px < 222px interno: cabe.
            // Mesmo tamanho visual dos outros slides para consistência.
            String mac = WiFi.macAddress();
            mac.toLowerCase();
            strncpy(text, mac.c_str(), sizeof(text) - 1);
            sz = 2;
            break;
        }

        default:
            strncpy(text, "Irriga Home", sizeof(text) - 1);
            sz = 2;
            break;
    }
    text[sizeof(text) - 1] = '\0';

    drawCarouselText(text, sz);
}

// Máquina de estados do carrossel: avança slide, gerencia apagão e redesenha.
// Deve ser chamada a cada frame pelo renderDashboard().
static void updateHeader() {
    unsigned long now = millis();

    if (!headerSlideVisible) {
        // Aguardando fim do apagão
        if ((now - headerBlankStart) >= HEADER_BLANK_MS) {
            // Avança para o próximo slide
            headerSlide      = (HeaderSlide)((headerSlide + 1) % SLIDE_COUNT);
            headerSlideStart = now;
            headerSlideVisible = true;
            paintHeaderSlide();
        }
        return;
    }

    // Slide visível: verifica se o tempo expirou
    if ((now - headerSlideStart) >= HEADER_SLIDE_MS) {
        // Inicia apagão
        clearHeaderInner();
        headerSlideVisible = false;
        headerBlankStart   = now;
    }
}

static void formatDateTime(char* dateText, size_t dateSize, char* timeText, size_t timeSize) {
    snprintf(dateText, dateSize, "--- --/--/--");
    snprintf(timeText, timeSize, "--:--");

    time_t nowTs = time(nullptr);
    if (nowTs <= 0) return;

    struct tm localTm;
    if (localtime_r(&nowTs, &localTm) == nullptr) {
        return;
    }

    // tm_wday: 0 = domingo … 6 = sábado (POSIX / ESP32). Fora disso → UB ao indexar WD_PT.
    int wday = localTm.tm_wday;
    if (wday < 0 || wday > 6) {
        wday = 0;
    }

    static const char* const WD_PT[7] = {
        "Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab"
    };

    int yy = (localTm.tm_year + 1900) % 100;
    snprintf(dateText, dateSize, "%s %02d/%02d/%02d",
             WD_PT[wday],
             localTm.tm_mday,
             localTm.tm_mon + 1,
             yy);

    snprintf(timeText, timeSize, "%02d:%02d", localTm.tm_hour, localTm.tm_min);
}

static void refreshBodyCache(unsigned long now) {
    cachedData        = sensores.read();
    cachedDuracao     = atuador.isLigado()
                            ? atuador.getRemainingSeconds(now)
                            : atuador.getDuracaoSegundos();
    cachedAgendamentos = atuador.getScheduleCount();
    lastBodyRefresh    = now;
    bodyInitialized    = true;
}

static void paintDashboardShell() {
    display.fillScreen(COLOR_BG);

    drawPanel(8,   40,  106, 52,  COLOR_CARD,      COLOR_PANEL_EDGE);
    drawPanel(126, 40,  106, 52,  COLOR_CARD,      COLOR_PANEL_EDGE);
    drawPanel(8,   96,  224, 60,  COLOR_CARD,      COLOR_PANEL_EDGE);
    drawPanel(8,   160, 224, 46,  COLOR_CARD,      COLOR_PANEL_EDGE);
    drawPanel(8,   210, 224, 54,  COLOR_CARD,      COLOR_PANEL_EDGE);

    display.setTextSize(1);
    display.setTextColor(COLOR_TEXT);
    display.setCursor(40,  48);  display.print("Temp Local");
    display.setCursor(155, 48);  display.print("Umid do Ar");
    display.setCursor(40,  104); display.print("Umid do Solo");
    display.setCursor(40,  168); display.print("Reservatorio de Agua");
    display.setCursor(40,  218); display.print("Bomba");

    drawHeaderBackground();

    // Inicia o carrossel no primeiro slide e pinta imediatamente
    headerSlide        = SLIDE_NAME;
    headerSlideStart   = millis();
    headerSlideVisible = true;
    paintHeaderSlide();

    // Invalida todo o cache de strings para forçar o primeiro draw completo
    prevTempText[0]  = '\0';
    prevHumText[0]   = '\0';
    prevSoilText[0]  = '\0';
    prevNivelAgua    = !cachedData.nivelAgua; // garante divergência
    prevPumpOn       = !atuador.isLigado();
    prevModoAuto     = !atuador.isModoAuto();
    prevDuracao[0]   = '\0';
    prevAgendamentos = -1;
    prevMqttConn     = !mqtt.isConnected();
    prevWifiConn     = !(WiFi.status() == WL_CONNECTED);
    prevTimeText[0]  = '\0';
    prevDateText[0]  = '\0';
}

// ── Cards dinâmicos (só redesenham quando o dado mudou) ─────────────────────

static void updateMetricCard(int x, int y, int w, int h,
                              const char* value, uint16_t accent,
                              uint16_t iconColor, bool useThermometer,
                              char* prevValue) {
    if (strcmp(prevValue, value) == 0) return; // sem mudança → sem redraw
    strncpy(prevValue, value, 15);
    prevValue[15] = '\0';

    // Limpa apenas a área do valor (abaixo do label fixo)
    clearValueArea(x + 2, y + 22, w - 4, h - 24, COLOR_CARD);

    if (useThermometer) drawThermometerIcon(x + 6, y + 8,  iconColor);
    else                drawDropletIcon(x + 6, y + 9, iconColor);

    drawCardValueCentered(x + 16, y + 22, w - 18, h - 24, value, accent, 2);
}

static void updateSoilCard(int x, int y, int w, int h, char* prevSoil) {
    int soilValue = cachedData.umidadeSolo;
    if (soilValue < 0)   soilValue = 0;
    if (soilValue > 100) soilValue = 100;

    char soilText[16];
    snprintf(soilText, sizeof(soilText), "%d%%", soilValue);

    if (strcmp(prevSoil, soilText) == 0) return;
    strncpy(prevSoil, soilText, 15);
    prevSoil[15] = '\0';

    // Limpa apenas a área dinâmica: valor percentual + barra (label fixo vem do shell)
    clearValueArea(x + 115, y + 8, 100, 30, COLOR_CARD); // área do valor %
    drawLeafIcon(x + 10, y + 9, COLOR_OK);               // ícone (não é label)

    drawCardValueCentered(x + 120, y + 12, 90, 34, soilText, COLOR_TEXT, 2);

    // Barra de progresso
    const int barX = x + 10;
    const int barY = y + h - 12;
    const int barW = w - 20;
    const int barH = 6;

    // Limpa toda a área da barra antes de redesenhar
    clearValueArea(barX, barY - 9, barW, 9 + barH + 2, COLOR_CARD);

    display.drawRoundRect(barX, barY, barW, barH, 3, COLOR_DIM);
    int fillW = (barW - 2) * soilValue / 100;
    if (fillW < 0) fillW = 0;
    display.fillRoundRect(barX + 1, barY + 1, fillW, barH - 2, 2, COLOR_OK);

    display.setTextSize(1);
    display.setTextColor(COLOR_DIM);
    display.setCursor(barX,           barY - 9); display.print("0%");
    display.setCursor(barX + barW - 24, barY - 9); display.print("100%");
}

static void updateReservoirCard(int x, int y, int w, int h, bool* prevNivel) {
    if (*prevNivel == cachedData.nivelAgua) return;
    *prevNivel = cachedData.nivelAgua;

    clearValueArea(x + 2, y + 18, w - 4, h - 20, COLOR_CARD);
    drawTankIcon(x + 10, y + 10, cachedData.nivelAgua ? COLOR_OK : COLOR_ALERT);

    display.setTextColor(cachedData.nivelAgua ? COLOR_OK : COLOR_ALERT);
    display.setTextSize(2);
    display.setCursor(x + 30, y + 22);
    display.print(cachedData.nivelAgua ? " Cheio" : " Vazio");

    const int indicatorRadius = 7;
    const int indicatorX      = x + w - indicatorRadius - 15;
    const int indicatorY      = y + (h - indicatorRadius) / 2 + 5;

    if (cachedData.nivelAgua) {
        display.fillCircle(indicatorX, indicatorY, indicatorRadius, COLOR_OK);
        display.drawCircle(indicatorX, indicatorY, indicatorRadius, COLOR_TEXT);
    } else {
        display.fillCircle(indicatorX, indicatorY, indicatorRadius, COLOR_CARD); // apaga
        display.drawCircle(indicatorX, indicatorY, indicatorRadius, COLOR_ALERT);
    }
}

static void updatePumpCard(int x, int y, int w, int h,
                            bool* prevOn, bool* prevAuto,
                            char* prevDur, int* prevAgend) {
    const bool pumpOn    = atuador.isLigado();
    const bool modoAuto  = atuador.isModoAuto();
    char durationText[16];
    snprintf(durationText, sizeof(durationText), "%ds",
             cachedDuracao > 0 ? cachedDuracao : 0);

    bool changed = (*prevOn   != pumpOn)
                || (*prevAuto != modoAuto)
                || (strcmp(prevDur, durationText) != 0)
                || (*prevAgend != cachedAgendamentos);

    if (!changed) return;

    *prevOn   = pumpOn;
    *prevAuto = modoAuto;
    strncpy(prevDur, durationText, 15);
    prevDur[15]  = '\0';
    *prevAgend = cachedAgendamentos;

    uint16_t accent = pumpOn ? COLOR_OK : COLOR_ALERT;

    // Seção esquerda: limpa abaixo do label " Bomba" (pintado no shell em y+8)
    // para não apagar o label fixo, mas garantir que ON/OFF e ícone sejam limpos.
    clearValueArea(x + 2,    y + 16, 110, h - 18, COLOR_CARD);

    // Seção direita: limpa inteira (inclui y+10 onde "Modo: Automatico/Manual" fica)
    // Isso evita sobreposição quando "Automatico"(60px) vira "Manual"(36px).
    clearValueArea(x + 114,  y + 6,  w - 116, h - 8, COLOR_CARD);

    // Redesenha divisor vertical (apagado pelo clear da seção direita)
    display.drawFastVLine(x + 112, y + 10, h - 18, COLOR_DIM);

    // Ícone + estado da bomba (label " Bomba" preservado no shell)
    drawPumpIcon(x + 10, y + 9, accent);

    display.setTextSize(2);
    display.setTextColor(accent);
    display.setCursor(x + 30, y + 24);
    display.print(pumpOn ? " ON" : " OFF");

    // Seção direita — "Modo:" + valor na mesma linha, sem sobreposição
    const char* modeText = modoAuto ? "Automatico" : "Manual";
    char agendNum[4];
    snprintf(agendNum, sizeof(agendNum), "%d", cachedAgendamentos);

    display.setTextSize(1);
    display.setTextColor(COLOR_TEXT);
    display.setCursor(x + 122, y + 10); display.print("Modo: ");
    display.setTextColor(COLOR_OK);     display.print(modeText);

    display.setTextColor(COLOR_TEXT);
    display.setCursor(x + 122, y + 22); display.print("Tempo: ");
    display.setTextColor(COLOR_OK);      display.print(durationText);

    display.setTextColor(COLOR_TEXT);
    display.setCursor(x + 122, y + 34); display.print("Agendado: ");
    display.setTextColor(COLOR_OK);      display.print(agendNum);
}

static void updateFooterBar(bool* prevMqtt, bool* prevWifi,
                             char* prevTime, char* prevDate) {
    char dateText[20];
    char timeText[8];
    formatDateTime(dateText, sizeof(dateText), timeText, sizeof(timeText));

    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    const bool mqttConnected = mqtt.isConnected();

    bool connectivityChanged = (*prevMqtt != mqttConnected)
                             || (*prevWifi != wifiConnected);
    bool timeChanged = (strcmp(prevTime, timeText) != 0);
    bool dateChanged = (strcmp(prevDate, dateText) != 0);

    if (!connectivityChanged && !timeChanged && !dateChanged) return;

    const int footerY = 274;
    const int footerH = SCREEN_HEIGHT - footerY - 6;
    const int lineY   = footerY + 4;

    // Redesenha o painel de fundo apenas se conectividade mudou
    if (connectivityChanged) {
        *prevMqtt = mqttConnected;
        *prevWifi = wifiConnected;

        drawPanel(8, footerY, SCREEN_WIDTH - 16, footerH, COLOR_HEADER_DARK, COLOR_PANEL_EDGE);

        // Monta string completa só para medir largura total e calcular startX
        // Estrutura: "MQTT: " [dot] "OK/--" "  WiFi: " [dot] "OK/--"
        // Cada char size=1 tem 6px de largura, dot = círculo r=3 (largura efetiva ~8px)
        // Segmentos: "MQTT: "(6ch=36) dot(8) status(2ch=12) "  WiFi: "(8ch=48) dot(8) status(2ch=12) = 124px total
        const int totalW  = 124;
        const int startX  = (SCREEN_WIDTH - totalW) / 2;

        display.setTextSize(1);

        int curX = startX;
        display.setTextColor(COLOR_TEXT);
        display.setCursor(curX, lineY);
        display.print("MQTT: ");
        curX += 36;

        drawStatusDot(curX + 3, lineY + 3, mqttConnected ? COLOR_OK : COLOR_ALERT);
        curX += 8;

        display.setCursor(curX, lineY);
        display.setTextColor(COLOR_TEXT);
        display.print(mqttConnected ? "OK" : "--");
        curX += 14; // 2 chars + pequena folga

        display.setTextColor(COLOR_TEXT);
        display.setCursor(curX, lineY);
        display.print("  WiFi: ");
        curX += 48;

        drawStatusDot(curX + 3, lineY + 3, wifiConnected ? COLOR_OK : COLOR_ALERT);
        curX += 8;

        display.setCursor(curX, lineY);
        display.setTextColor(COLOR_TEXT);
        display.print(wifiConnected ? "OK" : "--");
    }

    // Data (dia da semana abreviado + dd/mm/aa): redesenha se mudou ou se o painel do footer foi refeito
    if (dateChanged || connectivityChanged) {
        strncpy(prevDate, dateText, sizeof(prevDateText) - 1);
        prevDate[sizeof(prevDateText) - 1] = '\0';

        int16_t x1, y1;
        uint16_t dw, dh;
        display.setTextSize(1);
        display.getTextBounds(dateText, 0, 0, &x1, &y1, &dw, &dh);
        const int footerInnerW = SCREEN_WIDTH - 16;
        int dateX = 8 + (footerInnerW - (int)dw) / 2;
        clearValueArea(dateX, lineY + 14, dw, dh + 1, COLOR_HEADER_DARK);
        display.setTextColor(COLOR_OK);
        display.setCursor(dateX, lineY + 14);
        display.print(dateText);
    }

    // Hora: idem (painel MQTT/WiFi apaga a área inteira)
    if (timeChanged || connectivityChanged) {
        strncpy(prevTime, timeText, 7);
        prevTime[7] = '\0';

        int16_t x1, y1;
        uint16_t tw, th;
        display.setTextSize(1);
        display.getTextBounds(timeText, 0, 0, &x1, &y1, &tw, &th);
        const int footerInnerW = SCREEN_WIDTH - 16;
        int timeX = 8 + (footerInnerW - (int)tw) / 2;
        clearValueArea(timeX, lineY + 24, tw, th + 1, COLOR_HEADER_DARK);
        display.setTextColor(COLOR_OK);
        display.setCursor(timeX, lineY + 24);
        display.print(timeText);
    }
}

// ── Dashboard principal ──────────────────────────────────────────────────────
static void renderDashboard() {
    if (!dashboardPainted) {
        paintDashboardShell();
        dashboardPainted = true;
    }

    // Header NÃO é redesenhado aqui — já está pintado no shell e não muda

    char tempText[16];
    char humidityText[16];
    snprintf(tempText,    sizeof(tempText),    "%.1fC",  cachedData.temperatura);
    snprintf(humidityText, sizeof(humidityText), "%.1f%%", cachedData.umidadeAr);

    updateMetricCard(8,   40, 106, 52, tempText,     COLOR_ORANGE, COLOR_ORANGE, true,  prevTempText);
    updateMetricCard(126, 40, 106, 52, humidityText, COLOR_CYAN,   COLOR_CYAN,  false, prevHumText);
    updateSoilCard(8,  96,  224, 60, prevSoilText);
    updateReservoirCard(8, 160, 224, 46, &prevNivelAgua);
    updatePumpCard(8, 210, 224, 54, &prevPumpOn, &prevModoAuto, prevDuracao, &prevAgendamentos);
    updateFooterBar(&prevMqttConn, &prevWifiConn, prevTimeText, prevDateText);
    updateHeader();
}

// ================== INIT ==================
void DisplayManager::begin() {
    initializeDisplay();
    splashStartedAt  = millis();
    splashShown      = false;
    dashboardPainted = false;
    headerIpText[0] = '\0';

    if (displayReady) showSplash();

    lastFrameTime   = 0;
    lastBodyRefresh = 0;
    bodyInitialized = false;
    // Carrossel é inicializado em paintDashboardShell() na primeira chamada a renderDashboard()
}

// ================== SPLASH ==================
void DisplayManager::showSplash() {
    if (!displayReady) return;

    splashShown = true;
    display.fillScreen(COLOR_BG);

    display.setTextSize(4);
    display.setTextColor(COLOR_OK);
    display.setCursor(115, 50);
    display.print("*");

    drawCenteredText(SCREEN_WIDTH / 2, 105, 3, "Irriga", COLOR_TEXT);
    drawCenteredText(SCREEN_WIDTH / 2, 135, 3, "Home",   COLOR_TEXT);

    display.setTextSize(1);
    display.setTextColor(COLOR_DIM);
    int16_t x1, y1;
    uint16_t w, h;
    const char* footerText = "Codewave | 2026";
    display.getTextBounds(footerText, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 295);
    display.print(footerText);
}

// ================== LOOP ==================
void DisplayManager::update() {
    unsigned long now = millis();

    if (!displayReady) {
        initializeDisplay();
        bodyInitialized = false;
        lastFrameTime   = 0;
        lastBodyRefresh = 0;
        showSplash();
        return;
    }

    if ((now - splashStartedAt) < 3000UL) {
        if (!splashShown) showSplash();
        return;
    }

    if ((now - lastFrameTime) < FRAME_INTERVAL_MS) return;
    lastFrameTime = now;

    if (!bodyInitialized || (now - lastBodyRefresh) >= BODY_REFRESH_INTERVAL_MS) {
        refreshBodyCache(now);
    }

    renderDashboard();
}
