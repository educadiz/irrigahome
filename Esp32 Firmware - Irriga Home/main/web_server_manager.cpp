// Responsabilidade: servir pagina de manutencao via HTTP local e aplicar
// offsets de calibracao de forma thread-safe.
// O que faz: WebServer na porta 80, task FreeRTOS no Core 0, flag de config
// pendente consumida pelo Core 1 em applyPendingConfig().

#include "web_server_manager.h"
#include "config.h"
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

// ── Servidor e instancia global da task ──────────────────────────────────────
static WebServer          server(80);
static WebServerManager*  _instance = nullptr;
// Simple authentication state (in-memory). Allows up to 3 attempts.
static bool               _authValid = false;
static int                _authAttemptsLeft = 3;

static void resetAuthState() {
  _authValid = false;
  _authAttemptsLeft = 3;
}

static String formatMacAddress() {
  String macAddress = WiFi.macAddress();
  macAddress.toLowerCase();
  return macAddress;
}

// ── HTML da pagina de manutencao (PROGMEM — nao consome RAM heap) ────────────
// Pagina unica, CSS e JS inline, sem dependencias externas.
// JS busca /api/data a cada 3 s e envia POST /api/config ao salvar.
static const char MANUTENCAO_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Irriga Home — Manutencao</title>
<style>
  :root {
    --primary: #2B6CFF;
    --card: #F2F4F7;
    --border: #D7DDE6;
    --text: #122033;
    --muted: #3E516C;
    --shadow: 0 12px 30px rgba(18, 32, 51, .08);
  }

  * {
    box-sizing: border-box;
    margin: 0;
    padding: 0;
  }

  body {
    min-height: 100vh;
    padding: 24px 16px 18px;
    display: flex;
    justify-content: center;
    background: linear-gradient(180deg, #b2e2ba 0%, #ebf5ef 100%);
    color: var(--text);
    font: 600 16px/1.35 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  }

  .page {
    width: 100%;
    max-width: 980px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }

  .topbar {
    display: block;
    width: 100%;
  }

  .title-wrap {
    width: 100%;
  }

  .header-row {
    width: 100%;
  }

  .header-action {
    width: auto;
    min-width: 72px;
    height: 44px;
    padding: 0 14px;
    margin-top: 0;
    border-radius: 999px;
    background: var(--primary);
    color: #fff;
    border: 0;
    box-shadow: none;
    font: 700 .94rem/1 system-ui, sans-serif;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    text-align: center;
    white-space: nowrap;
  }

  .sair-btn {
    width: auto;
    min-width: 72px;
    height: 44px;
    padding: 0 14px;
    margin-top: 0;
    border: 0;
    border-radius: 999px;
    background: var(--primary);
    color: #fff;
    font: 700 .94rem/1 system-ui, sans-serif;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    text-align: center;
    white-space: nowrap;
    box-shadow: none;
  }

  .sair-btn:active {
    transform: translateY(1px);
  }

  header {
    width: 100%;
  }

  h1 {
    text-align: center;
    font-size: 1.38rem;
    font-weight: 700;
    color: #1B5E20;
    margin-bottom: 4px;
  }

  .sub {
    text-align: center;
    font-size: .8rem;
    color: var(--muted);
    margin-bottom: 12px;
  }

  .cards {
    width: 100%;
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 12px;
  }

  .card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 18px;
    padding: 14px 16px;
    box-shadow: var(--shadow);
  }

  .card h2 {
    text-align: center;
    margin-bottom: 10px;
    font-size: .86rem;
    letter-spacing: .09em;
    text-transform: uppercase;
    color: #1B2C41;
  }

  .row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    min-height: 30px;
    margin-bottom: 7px;
  }

  .label {
    flex: 1;
    color: #15263A;
    font-weight: 600;
    font-size: .87rem;
  }

  .emoji {
    width: 28px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    margin-right: 8px;
    font-size: 1.05rem;
  }

  .value {
    min-width: 92px;
    text-align: right;
    color: var(--primary);
    font-weight: 700;
    font-size: .88rem;
  }

  .value.water-ok {
    color: var(--primary);
  }

  .value.water-empty {
    color: #C63535;
  }

  #live .row {
    min-height: 26px;
    margin-bottom: 5px;
  }

  #live .row:last-child {
    margin-bottom: 0;
  }

  input[type=number] {
    width: 96px;
    padding: 6px 9px;
    border: 1px solid var(--border);
    border-radius: 10px;
    background: #fff;
    color: var(--text);
    text-align: center;
    font-size: .85rem;
    box-shadow: inset 0 1px 0 rgba(255, 255, 255, .8);
  }

  input[type=number]:focus {
    outline: none;
    border-color: var(--primary);
    box-shadow: 0 0 0 3px rgba(43, 108, 255, .12);
  }

  .unit {
    min-width: 28px;
    color: var(--muted);
    font-size: .78rem;
    font-weight: 600;
  }

  .btn {
    width: 100%;
    margin-top: 8px;
    min-height: 44px;
    padding: 0 16px;
    border: 0;
    border-radius: 12px;
    background: var(--primary);
    color: #fff;
    font: 700 .94rem/1 system-ui, sans-serif;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    text-align: center;
    white-space: nowrap;
  }

  .mini-btn {
    min-height: 44px;
    padding: 0 14px;
    border: 0;
    border-radius: 12px;
    background: #1f8f46;
    color: #fff;
    font: 700 .94rem/1 system-ui, sans-serif;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    text-align: center;
    white-space: nowrap;
  }

  .mini-btn:active {
    transform: translateY(1px);
  }

  .divider {
    border: 0;
    border-top: 1px solid var(--border);
    margin: 14px 0;
  }

  .pill {
    display: inline-block;
    padding: 3px 10px;
    border-radius: 999px;
    font-size: .76rem;
    font-weight: 700;
  }

  .on {
    background: #E8F7EC;
    color: #1F7A3A;
  }

  .off {
    background: transparent;
    color: #C63535;
    padding: 0;
    border-radius: 0;
  }

  footer {
    width: 100%;
    text-align: center;
    color: var(--muted);
    font-size: .78rem;
    padding: 4px 0 0;
  }

  .toast {
    position: fixed;
    left: 50%;
    bottom: 24px;
    transform: translateX(-50%);
    padding: 10px 22px;
    border-radius: 10px;
    background: #1f8f46;
    color: #fff;
    font: 700 .9rem/1 system-ui, sans-serif;
    opacity: 0;
    pointer-events: none;
    transition: opacity .3s;
  }

  .toast.err {
    background: #d92d20;
  }

  .toast.show {
    opacity: 1;
  }

  .modal-backdrop {
    position: fixed;
    inset: 0;
    background: rgba(12, 22, 34, .48);
    display: none;
    align-items: center;
    justify-content: center;
    padding: 18px;
    z-index: 50;
  }

  .modal-backdrop.show {
    display: flex;
  }

  .modal {
    width: min(100%, 360px);
    background: #fff;
    border-radius: 16px;
    padding: 18px 18px 16px;
    box-shadow: 0 18px 48px rgba(18, 32, 51, .22);
    border: 1px solid rgba(215, 221, 230, .9);
  }

  .modal h3 {
    text-align: center;
    margin-bottom: 8px;
    color: #1B2C41;
    font-size: 1rem;
  }

  .modal p {
    text-align: center;
    color: var(--muted);
    font-size: .9rem;
    margin-bottom: 16px;
  }

  .modal-actions {
    display: flex;
    gap: 10px;
  }

  .modal-actions button {
    flex: 1;
    width: auto;
    margin-top: 0;
    height: 44px;
  }

  .secondary-btn {
    background: #EEF2F7;
    color: #1B2C41;
  }

  @media (max-width: 820px) {
    .cards {
      grid-template-columns: 1fr;
    }

    .page {
      max-width: 760px;
    }
  }

  @media (max-width: 520px) {
    body {
      padding: 12px 10px 14px;
    }

    .page {
      gap: 10px;
    }

    .card {
      padding: 12px 12px 13px;
      border-radius: 14px;
    }

    .card h2 {
      margin-bottom: 8px;
    }

    .row {
      flex-wrap: wrap;
      align-items: flex-start;
      gap: 6px 8px;
      min-height: unset;
      margin-bottom: 10px;
    }

    .label {
      flex: 1 1 100%;
      min-width: 0;
      line-height: 1.25;
    }

    .value {
      flex: 1 1 auto;
      min-width: 0;
      text-align: left;
      word-break: break-word;
    }

    .unit {
      margin-left: auto;
    }

    .mini-btn {
      width: 100%;
      margin-left: 0;
    }

    input[type=number] {
      width: 100%;
      min-width: 0;
      flex: 1 1 100%;
      text-align: left;
    }

    .btn {
      padding: 13px;
    }

    .topbar {
      display: block;
    }

    .header-action {
      min-width: 72px;
      height: 44px;
      padding: 0 14px;
      font: 700 .94rem/1 system-ui, sans-serif;
    }

    .modal {
      padding: 16px 16px 14px;
    }

    .modal-actions {
      flex-direction: column;
    }

    .toast {
      bottom: 14px;
      width: calc(100% - 24px);
      text-align: center;
    }
  }
</style>
</head>
<body>
<div class="page">
  <header>
    <div class="topbar">
      <div class="header-row">
        <div class="title-wrap">
        <h1>Irriga 🌱 Home</h1>
        <p class="sub">Console de manutenção e ajustes</p>
        </div>
      </div>
    </div>
  </header>

  <div class="cards">
    <section class="card" id="live">
      <h2>Leituras</h2>
      <p class="sub">Leituras dos sensores em tempo real</p>
      <div class="row"><span class="emoji">🌱</span><span class="label">MAC Address do irrigador</span><span class="value" id="lv-irrigador">--</span></div>
      <div class="row"><span class="emoji">💧</span><span class="label">Umidade do solo</span><span class="value" id="lv-solo">--</span></div>
      <div class="row"><span class="emoji">🌡️</span><span class="label">Temperatura</span><span class="value" id="lv-temp">--</span></div>
      <div class="row"><span class="emoji">💨</span><span class="label">Umidade do ar</span><span class="value" id="lv-ar">--</span></div>
      <div class="row"><span class="emoji">🚰</span><span class="label">Nível do reservatório</span><span class="value" id="lv-agua">--</span></div>
      <div class="row"><span class="emoji">🔌</span><span class="label">Bomba de água</span><span class="value" id="lv-bomba">--</span></div>
      <div class="row"><span class="emoji">⚙️</span><span class="label">Modo de acionamento</span><span class="value" id="lv-modo">--</span></div>
      <div class="row"><span class="emoji">⏱️</span><span class="label">Tempo de rega programado</span><span class="value" id="lv-programmed">--</span></div>
      <div class="row">
        <span class="emoji">🗓️</span>
        <span class="label">Agendados na memória NVRAM</span>
        <span class="value" id="lv-sched-count">--</span>
        <button class="mini-btn" onclick="resetSchedules()">Reset</button>
      </div>
      <div class="logout-row" style="margin-top:8px;">
        <button class="sair-btn" id="logout-btn" type="button" onclick="abrirLogoutModal()" style="display:none;">Sair</button>
      </div>
    </section>

    <section class="card">
      <h2>Calibração</h2>
      <p class="sub">Offsets aplicados na leitura afetam exibição e lógica de controle automático</p>

      <div class="row">
        <span class="label">Offset Umidade Solo [-30.0, +30.0]</span>
        <input type="number" id="off-solo" min="-30.0" max="30.0" step="0.1" value="0.0">
        <span class="unit">%</span>
      </div>
      <div class="row">
        <span class="label">Offset Temperatura [-10.0, +10.0]</span>
        <input type="number" id="off-temp" min="-10.0" max="10.0" step="0.1" value="0.0">
        <span class="unit">°C</span>
      </div>
      <div class="row">
        <span class="label">Offset Umidade do Ar [-20.0, +20.0]</span>
        <input type="number" id="off-ar" min="-20.0" max="20.0" step="0.1" value="0.0">
        <span class="unit">%</span>
      </div>

      <hr class="divider">
      <h2 style="margin-bottom:12px">Ajuste de vazão da bomba</h2>
      <div class="row">
        <span class="label">Fator [0.1, 5]</span>
        <input type="number" id="flow-scale" min="0.1" max="5" step="0.0001" value="1.0">
        <span class="unit"></span>
      </div>
      <div class="row">
        <span class="label">Offset (mL) [-500, 500]</span>
        <input type="number" id="flow-offset" min="-500" max="500" step="0.1" value="0.0">
        <button class="mini-btn" onclick="medir()" id="measure-btn">Medir</button>
        <span class="unit">mL</span>
      </div>

      <button class="btn" onclick="salvar()">Salvar ajuste</button>
    </section>
  </div>

  <!-- Auth overlay: solicita senha antes de permitir ajustes -->
  <div id="auth-overlay" style="position:fixed;left:0;top:0;right:0;bottom:0;background:rgba(0,0,0,0.35);display:flex;align-items:center;justify-content:center;">
    <div style="width:360px;background:#fff;padding:18px;border-radius:12px;box-shadow:0 8px 30px rgba(0,0,0,0.2);">
      <h2 style="margin-bottom:8px;text-align:center">Acesso Restrito</h2>
      <p style="font-size:.9rem;color:#3E516C;margin-bottom:12px;text-align:center">Informe a senha para liberar as configurações (3 tentativas)</p>
      <div style="display:flex;gap:8px;margin-bottom:12px;align-items:center;">
        <input id="auth-pass" type="password" placeholder="Senha" style="flex:1;padding:8px;border:1px solid #D7DDE6;border-radius:8px;font-size:1rem">
        <button id="auth-btn" class="mini-btn">Entrar</button>
      </div>
      <div style="text-align:center;color:#C63535;font-weight:700" id="auth-msg"></div>
    </div>
  </div>

  <footer>Desenvolvido por CodeWave | 2026</footer>
</div>

<div class="modal-backdrop" id="logout-modal" aria-hidden="true">
  <div class="modal" role="dialog" aria-modal="true" aria-labelledby="logout-title">
    <h3 id="logout-title">Confirmar saída</h3>
    <p>Deseja encerrar a sessão de manutenção deste irrigador?</p>
    <div class="modal-actions">
      <button class="btn secondary-btn" type="button" onclick="fecharLogoutModal()">Cancelar</button>
      <button class="sair-btn" type="button" onclick="confirmarLogout()">Sair</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
const $ = id => document.getElementById(id);

const badge = on => on ? '<span class="pill on">ON</span>' : '<span class="off">OFF</span>';
const formatDuration = totalSeconds => {
  const seconds = Math.max(0, totalSeconds | 0), minutes = (seconds / 60) | 0, rest = seconds % 60;
  return minutes ? (minutes + ' min' + (rest ? ' ' + rest + ' s' : '')) : (seconds + ' s');
};

function toast(msg, err) {
  $('toast').textContent = msg;
  $('toast').className = 'toast show' + (err ? ' err' : '');
  setTimeout(() => { $('toast').className = 'toast'; }, 3000);
}

// ---- Autenticacao ----
let isAuthenticated = false;
function showAuthMessage(msg) {
  $('auth-msg').textContent = msg;
}

function setLogoutButtonVisible(visible) {
  const btn = $('logout-btn');
  if (btn) btn.style.display = visible ? 'inline-flex' : 'none';
}

function abrirLogoutModal() {
  const modal = $('logout-modal');
  if (modal) modal.classList.add('show');
}

function fecharLogoutModal() {
  const modal = $('logout-modal');
  if (modal) modal.classList.remove('show');
}

function confirmarLogout() {
  fetch('/api/logout', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      fecharLogoutModal();
      if (d.ok) {
        isAuthenticated = false;
        setLogoutButtonVisible(false);
        document.getElementById('auth-overlay').style.display = 'flex';
        $('auth-pass').value = '';
        $('auth-pass').disabled = false;
        $('auth-btn').disabled = false;
        showAuthMessage('Sessão encerrada');
        toast('Sessão encerrada', false);
      } else {
        toast('Não foi possível sair', true);
      }
    })
    .catch(() => {
      fecharLogoutModal();
      toast('Falha na comunicação com o irrigador', true);
    });
}

function tryAuth() {
  const pass = $('auth-pass').value || '';
  fetch('/api/auth', { method: 'POST', body: new URLSearchParams({ password: pass }) })
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        isAuthenticated = true;
        document.getElementById('auth-overlay').style.display = 'none';
        showAuthMessage('');
        setLogoutButtonVisible(true);
        toast('Acesso liberado', false);
      } else {
        if (d.locked) {
          showAuthMessage('Bloqueado: número máximo de tentativas excedido');
          $('auth-btn').disabled = true;
          $('auth-pass').disabled = true;
        } else {
          showAuthMessage('Senha inválida. Tentativas restantes: ' + (d.attemptsLeft ?? 0));
        }
      }
    })
    .catch(() => showAuthMessage('Falha na comunicação'));
}

document.addEventListener('DOMContentLoaded', () => {
  // hook login button
  const btn = $('auth-btn');
  if (btn) btn.addEventListener('click', tryAuth);
  const input = $('auth-pass');
  if (input) input.addEventListener('keyup', (e) => { if (e.key === 'Enter') tryAuth(); });
  setLogoutButtonVisible(false);

  const modal = $('logout-modal');
  if (modal) {
    modal.addEventListener('click', (event) => {
      if (event.target === modal) {
        fecharLogoutModal();
      }
    });
  }
});


function atualizar() {
  fetch('/api/data')
    .then(r => r.json())
    .then(d => {
      $('lv-irrigador').textContent = d.macAddress || d.deviceId || '--';
      $('lv-solo').textContent = d.soil + '%';
      $('lv-temp').textContent = d.temp + '\u00b0C';
      $('lv-ar').textContent = d.humidity + '%';
      $('lv-agua').textContent = d.water ? 'Cheio' : 'Vazio';
      $('lv-agua').className = 'value ' + (d.water ? 'water-ok' : 'water-empty');
      $('lv-bomba').innerHTML = badge(d.pump);
      $('lv-modo').textContent = d.mode === 'auto' ? 'Automático' : 'Manual';
      $('lv-programmed').textContent = formatDuration(d.programmedDurationSec);
      $('lv-sched-count').textContent = String(d.scheduleCount ?? 0);

      if (!window._loaded) {
        window._loaded = true;
        $('off-solo').value = d.offSolo;
        $('off-temp').value = d.offTemp;
        $('off-ar').value = d.offAr;
        $('flow-scale').value = d.flowScale;
        $('flow-offset').value = d.flowOffsetMl;
      }
    })
    .catch(() => {});
}

function resetSchedules() {
  if (!confirm('Confirma zerar todos os agendamentos salvos no ESP32?')) {
    return;
  }

  fetch('/api/schedules/reset', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        toast('Agendamentos apagados com sucesso!');
        atualizar();
      } else {
        toast('Falha ao apagar agendamentos', true);
      }
    })
    .catch(() => toast('Falha na comunicação com o irrigador', true));
}

function salvar() {
  const body = new URLSearchParams({
    offSolo: $('off-solo').value,
    offTemp: $('off-temp').value,
    offAr: $('off-ar').value,
    flowScale: $('flow-scale').value,
    flowOffset: $('flow-offset').value
  });

  fetch('/api/config', { method: 'POST', body })
    .then(r => r.json())
    .then(d => toast(d.ok ? 'Calibração salva com sucesso!' : 'Erro ao salvar: ' + d.error, !d.ok))
    .catch(() => toast('Falha na comunicação com o irrigador', true));
}

function logout() {
  abrirLogoutModal();
}

function medir() {
  if (!confirm('Acionar bomba manualmente para medição?')) return;
  let duration = prompt('Duração em segundos para medir (ex: 10):', '10');
  if (duration === null) return;
  duration = parseInt(duration, 10);
  if (isNaN(duration) || duration <= 0) { toast('Duração inválida', true); return; }

  fetch('/api/measure', { method: 'POST', body: new URLSearchParams({ duration: String(duration) }) })
    .then(r => r.json())
    .then(d => {
      if (d.ok) {
        toast('Medição iniciada por ' + duration + 's');
      } else {
        toast('Falha ao iniciar medição: ' + (d.error || 'unauthorized'), true);
      }
    })
    .catch(() => toast('Falha na comunicação com o irrigador', true));
}

atualizar();
setInterval(atualizar, 3000);
</script>
</body>
</html>
)rawhtml";


// ── Implementacao ─────────────────────────────────────────────────────────────

void WebServerManager::begin(SensorManager& sensors, ActuatorManager& actuator,
                              FlowMeterManager& flow) {
    _sensors  = &sensors;
    _actuator = &actuator;
    _flow     = &flow;
    _instance = this;

    server.on("/",          HTTP_GET,  []{ if (_instance) _instance->handleRoot();   });
    server.on("/api/data",  HTTP_GET,  []{ if (_instance) _instance->handleData();   });
    server.on("/api/auth",  HTTP_POST, []{ if (_instance) _instance->handleAuth();   });
    server.on("/api/logout", HTTP_POST, []{ if (_instance) _instance->handleLogout(); });
    server.on("/api/measure", HTTP_POST, []{ if (_instance) _instance->handleMeasure(); });
    server.on("/api/config",HTTP_POST, []{ if (_instance) _instance->handleConfig(); });
    server.on("/api/schedules/reset", HTTP_POST, []{ if (_instance) _instance->handleResetSchedules(); });

    server.begin();
    Serial.print("[WEB] Servidor de manutencao iniciado em http://");
    Serial.println(WiFi.localIP());

    // Task no Core 0 — junto com firebaseTask e mqttConnectTask.
    // Stack 6 KB: WebServer (~2 KB) + snprintf/JSON (~1 KB) + margem TLS-free.
    // webTask NAO e' registrada no TWDT — requests lentos nao causam panic.
    xTaskCreatePinnedToCore(_task, "webTask", 6144, this, 1, nullptr, 0);
}

void WebServerManager::_task(void* pv) {
    static_cast<WebServerManager*>(pv)->_run();
}

void WebServerManager::_run() {
    for (;;) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));  // cede CPU entre requisicoes
    }
}

// ── Handlers ─────────────────────────────────────────────────────────────────

void WebServerManager::handleRoot() {
    // send_P le diretamente da flash — nao copia o HTML para a heap
    server.send_P(200, "text/html; charset=utf-8", MANUTENCAO_HTML);
}

void WebServerManager::handleData() {
    if (!_sensors || !_actuator || !_flow) {
        server.send(503, "application/json", "{\"error\":\"not ready\"}");
        return;
    }

    // Leitura snapshot — segura pois SensorManager::read() e' idempotente
    // e os campos de offset sao escritos apenas por applyPendingConfig() (Core 1).
    SensorData d = _sensors->read();

    String macAddress = formatMacAddress();

    char json[460];
    snprintf(json, sizeof(json),
        "{"
      "\"deviceId\":\"%s\"," 
      "\"macAddress\":\"%s\"," 
        "\"soil\":%d,"
        "\"temp\":%.1f,"
        "\"humidity\":%.1f,"
        "\"water\":%s,"
        "\"pump\":%s,"
        "\"mode\":\"%s\","
      "\"scheduleCount\":%d,"
      "\"programmedDurationSec\":%d,"
        "\"offSolo\":%.1f,"
        "\"offTemp\":%.1f,"
        "\"offAr\":%.1f,"
        "\"flowScale\":%.6f,"
        "\"flowOffsetMl\":%.3f"
        "}",
        getDeviceIdFromMac().c_str(),
        macAddress.c_str(),
        d.umidadeSolo,
        isnan(d.temperatura) ? 0.0f : d.temperatura,
        isnan(d.umidadeAr)   ? 0.0f : d.umidadeAr,
        d.nivelAgua          ? "true" : "false",
        _actuator->isLigado()    ? "true" : "false",
        _actuator->isModoAuto()  ? "auto" : "manual",
      _actuator->getScheduleCount(),
      _actuator->getDuracaoSegundos(),
        (double)_sensors->getOffsetUmidadeSolo(),
        (double)_sensors->getOffsetTemperatura(),
        (double)_sensors->getOffsetUmidadeAr(),
        _flow->getVolumeCalibrationScale(),
        _flow->getVolumeCalibrationOffsetMl()
    );

    server.send(200, "application/json", json);
}

void WebServerManager::handleAuth() {
  // Espera campo 'password' no body
  if (!server.hasArg("password")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametros ausentes\"}");
    return;
  }

  String pass = server.arg("password");
  // senha definida em secrets.h
  const char* DEFAULT_PASS = WEBSERVER_PASS;

  if (_authValid) {
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (pass.equals(DEFAULT_PASS)) {
    _authValid = true;
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.println("[WEB] usuario autenticado com sucesso");
    return;
  }

  // senha incorreta
  if (_authAttemptsLeft > 0) _authAttemptsLeft -= 1;

  char json[80];
  if (_authAttemptsLeft <= 0) {
    _authAttemptsLeft = 0;
    snprintf(json, sizeof(json), "{\"ok\":false,\"locked\":true,\"attemptsLeft\":0}");
  } else {
    snprintf(json, sizeof(json), "{\"ok\":false,\"locked\":false,\"attemptsLeft\":%d}", _authAttemptsLeft);
  }

  server.send(200, "application/json", json);
}

void WebServerManager::handleLogout() {
  resetAuthState();
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("[WEB] usuario saiu da manutencao");
}

void WebServerManager::handleMeasure() {
  if (!_authValid) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  if (!_actuator) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"not ready\"}");
    return;
  }

  int duration = 10; // default 10s
  if (server.hasArg("duration")) {
    duration = server.arg("duration").toInt();
    if (duration < 1) duration = 1;
    if (duration > 300) duration = 300;
  }

  if (_actuator->isLigado()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"pump already on\"}");
    return;
  }

  _actuator->setCurrentTrigger(TRIGGER_MANUAL);
  _actuator->ligar();
  unsigned long now = millis();
  _actuator->setActiveUntil(now + ((unsigned long)duration * 1000UL));

  server.send(200, "application/json", "{\"ok\":true}");
}

  void WebServerManager::handleResetSchedules() {
    if (!_authValid) {
      server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
      return;
    }

    if (!_actuator) {
      server.send(503, "application/json", "{\"ok\":false,\"error\":\"not ready\"}");
      return;
    }

    _actuator->clearSchedules();
    server.send(200, "application/json", "{\"ok\":true}");
  }

void WebServerManager::handleConfig() {
  if (!_authValid) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
    // Valida presenca dos campos obrigatorios
    if (!server.hasArg("offSolo")    || !server.hasArg("offTemp") ||
        !server.hasArg("offAr")      || !server.hasArg("flowScale") ||
        !server.hasArg("flowOffset")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametros ausentes\"}");
        return;
    }

    float offTemp   = server.arg("offTemp").toFloat();
    float offSolo   = server.arg("offSolo").toFloat();
    float offAr     = server.arg("offAr").toFloat();
    float flowScale = server.arg("flowScale").toFloat();
    float flowOff   = server.arg("flowOffset").toFloat();

    // Validacao de limites antes de enfileirar
    if (offTemp   < -10.0f || offTemp   > 10.0f  ||
        offSolo   < -30.0f || offSolo   > 30.0f   ||
        offAr     < -20.0f || offAr     > 20.0f   ||
        flowScale <=  0.0f || flowScale > 5.0f     ||
        flowOff   < -500.0f|| flowOff   > 500.0f) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"valor fora do limite permitido\"}");
        return;
    }

    // Copia para struct pendente (Core 0 escreve, Core 1 le)
    // O flag volatile garante visibilidade entre nucleos sem mutex completo.
    // Os campos numericos sao escritos antes do flag — sequencia de escrita segura
    // para tipos simples em arquitetura 32-bit como o ESP32.
    _pending.offTemp    = offTemp;
    _pending.offSolo    = offSolo;
    _pending.offUmidAr  = offAr;
    _pending.flowScale  = flowScale;
    _pending.flowOffset = flowOff;
    _pending.pending    = true;   // sinaliza por ultimo

    server.send(200, "application/json", "{\"ok\":true}");
}

// Chamado pelo loop() do main.ino no Core 1.
// Aplica a configuracao pendente nos managers e persiste na NVS.
void WebServerManager::applyPendingConfig() {
    if (!_pending.pending) return;
    _pending.pending = false;   // consome o flag antes de aplicar

    if (_sensors) {
        _sensors->setOffsetTemperatura(_pending.offTemp);
        _sensors->setOffsetUmidadeSolo(_pending.offSolo);
        _sensors->setOffsetUmidadeAr(_pending.offUmidAr);
        _sensors->flushOffsets();   // batch NVS
    }

    if (_flow) {
        _flow->setVolumeCalibration(_pending.flowScale, _pending.flowOffset);
        // setVolumeCalibration ja chama saveCalibration() internamente
    }

    Serial.println("[WEB] calibracao aplicada via pagina de manutencao");
    Serial.print  ("[WEB] offSolo=" );  Serial.print(_pending.offSolo,   1);
    Serial.print  (" offTemp=");        Serial.print(_pending.offTemp,   1);
    Serial.print  (" offAr=");          Serial.print(_pending.offUmidAr, 1);
    Serial.print  (" flowScale=");      Serial.print(_pending.flowScale, 6);
    Serial.print  (" flowOffsetMl=");   Serial.println(_pending.flowOffset, 3);
}
