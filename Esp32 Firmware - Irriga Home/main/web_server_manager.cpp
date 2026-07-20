// Responsabilidade: servir pagina de manutencao via HTTP local e aplicar
// offsets de calibracao de forma thread-safe.
// O que faz: WebServer na porta 80, task FreeRTOS no Core 0, flag de config
// pendente consumida pelo Core 1 em applyPendingConfig() com barreira de memoria.

#include "web_server_manager.h"
#include "config.h"
#include "firmware_logger.h" // Adicionado para padronizacao de logs
#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

// ── Servidor e instancia global da task ──────────────────────────────────────
static WebServer          server(80);
static WebServerManager* _instance = nullptr;
// Simple authentication state (in-memory). Allows up to 3 attempts.
static bool               _authValid = false;
static int                _authAttemptsLeft = 3;
static Preferences        flowPrefs;
static constexpr const char* FLOW_UI_NAMESPACE = "irrigahome";
static constexpr const char* FLOW_UI_KEY_LAST_TEST_AT = "flow_last_test_at";
static constexpr const char* FLOW_UI_KEY_LAST_CAL_AT = "flow_last_cal_at";
static constexpr const char* FLOW_UI_KEY_LAST_TEST_SEC = "flow_last_test_sec";

static String lastFlowTestAtIso;
static String lastFlowCalibrationAtIso;
static int lastFlowTestDurationSec = 0;

static void resetAuthState() {
  _authValid = false;
  _authAttemptsLeft = 3;
}

static String formatMacAddress() {
  String macAddress = WiFi.macAddress();
  macAddress.toLowerCase();
  return macAddress;
}

static String formatIso8601Now() {
  time_t now = time(nullptr);
  if (now < 1000000000L) {
    return "";
  }

  struct tm timeinfo;
  if (!gmtime_r(&now, &timeinfo)) {
    return "";
  }

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

static void loadFlowUiState() {
  if (!flowPrefs.begin(FLOW_UI_NAMESPACE, true)) {
    return;
  }

  lastFlowTestAtIso = flowPrefs.getString(FLOW_UI_KEY_LAST_TEST_AT, "");
  lastFlowCalibrationAtIso = flowPrefs.getString(FLOW_UI_KEY_LAST_CAL_AT, "");
  lastFlowTestDurationSec = flowPrefs.getInt(FLOW_UI_KEY_LAST_TEST_SEC, 0);
  flowPrefs.end();
}

static void saveFlowUiState() {
  if (!flowPrefs.begin(FLOW_UI_NAMESPACE, false)) {
    return;
  }

  flowPrefs.putString(FLOW_UI_KEY_LAST_TEST_AT, lastFlowTestAtIso);
  flowPrefs.putString(FLOW_UI_KEY_LAST_CAL_AT, lastFlowCalibrationAtIso);
  flowPrefs.putInt(FLOW_UI_KEY_LAST_TEST_SEC, lastFlowTestDurationSec);
  flowPrefs.end();
}

// ── HTML da pagina de manutencao (PROGMEM — nao consome RAM heap) ────────────
static const char MANUTENCAO_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Irriga Home | Painel de Manutenção</title>
<style>
:root{--bg:#0f172a;--surface:#fff;--surface-2:#f8fafc;--border:#e2e8f0;--text:#0f172a;--muted:#64748b;--primary:#2563eb;--primary-d:#1d4ed8;--ok:#059669;--ok-bg:#ecfdf5;--err:#dc2626;--err-bg:#fef2f2;--radius:14px;--shadow:0 1px 2px rgba(15,23,42,.04),0 8px 24px -8px rgba(15,23,42,.08)}
*{box-sizing:border-box;margin:0;padding:0}
body{min-height:100vh;padding:28px 16px;background:var(--surface-2);color:var(--text);font:15px/1.4 -apple-system,system-ui,"Segoe UI",sans-serif;display:flex;justify-content:center}
.page{width:100%;max-width:920px;display:flex;flex-direction:column;gap:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px}
.brand{display:flex;align-items:center;gap:10px}
.brand-mark{width:36px;height:36px;border-radius:10px;background:linear-gradient(135deg,var(--primary),#1e40af);display:flex;align-items:center;justify-content:center;color:#fff;font-size:18px;flex-shrink:0}
.brand h1{font-size:1.05rem;font-weight:700;letter-spacing:-.01em}
.brand .sub{font-size:.78rem;color:var(--muted);font-weight:500}
.btn{height:38px;padding:0 16px;border:0;border-radius:9px;background:var(--primary);color:#fff;font:600 .87rem/1 inherit;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;gap:6px;transition:.15s}
.btn:hover{background:var(--primary-d)}
.btn:active{transform:translateY(1px)}
.btn-block{width:100%;height:44px;margin-top:6px;font-size:.92rem}
.btn-ghost{background:var(--surface);color:var(--text);border:1px solid var(--border)}
.btn-ghost:hover{background:var(--surface-2)}
.btn-sm{height:32px;padding:0 12px;font-size:.8rem}
.btn-danger{background:var(--surface);color:var(--err);border:1px solid #fecaca}
.btn-danger:hover{background:var(--err-bg)}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:18px 20px;box-shadow:var(--shadow)}
.card-head{display:flex;align-items:baseline;justify-content:space-between;margin-bottom:4px}
.card-head h2{font-size:.92rem;font-weight:700}
.card .desc{font-size:.78rem;color:var(--muted);margin-bottom:14px}
.field{display:flex;align-items:center;gap:10px;padding:9px 0;border-bottom:1px solid var(--border)}
.field:last-of-type{border-bottom:0}
.field .ico{width:20px;text-align:center;flex-shrink:0;opacity:.85}
.field .label{flex:1;font-size:.85rem;font-weight:600;color:#334155}
.field .value{font-weight:700;font-size:.86rem;color:var(--primary);text-align:right}
.value.ok{color:var(--ok)}
.value.bad{color:var(--err)}
.tag{display:inline-flex;padding:2px 9px;border-radius:999px;font-size:.72rem;font-weight:700}
.tag.on{background:var(--ok-bg);color:var(--ok)}
.tag.off{background:#f1f5f9;color:var(--muted)}
.cal-row{padding:10px 0;border-bottom:1px solid var(--border)}
.cal-row:last-of-type{border-bottom:0}
.cal-row .top{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:6px}
.cal-row .label{font-size:.83rem;font-weight:600;color:#334155}
.cal-row .range{font-size:.72rem;color:var(--muted)}
.inp-wrap{display:flex;align-items:center;gap:8px}
input[type=number]{flex:1;height:36px;padding:0 10px;border:1px solid var(--border);border-radius:8px;background:var(--surface-2);color:var(--text);font:600 .88rem inherit;text-align:right;min-width:0}
input[type=number]:focus{outline:none;border-color:var(--primary);background:#fff;box-shadow:0 0 0 3px rgba(37,99,235,.1)}
.unit{font-size:.74rem;color:var(--muted);font-weight:600;min-width:24px}
hr{border:0;border-top:1px solid var(--border);margin:16px 0}
.subtitle{font-size:.78rem;font-weight:700;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);margin-bottom:10px}
footer{text-align:center;color:var(--muted);font-size:.76rem;padding:6px 0 0}
footer #lv-lastupdate{font-size:.7rem;opacity:.7}
footer #lv-lastupdate.stale{color:var(--err);opacity:1;font-weight:600}
.toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%) translateY(8px);padding:11px 20px;border-radius:10px;background:var(--text);color:#fff;font:600 .85rem/1 inherit;box-shadow:0 12px 28px -6px rgba(0,0,0,.25);opacity:0;pointer-events:none;transition:.2s}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.toast.err{background:var(--err)}
.overlay{position:fixed;inset:0;background:rgba(15,23,42,.55);display:flex;align-items:center;justify-content:center;padding:18px;z-index:50}
.overlay[hidden]{display:none}
.modal{width:min(100%,360px);background:#fff;border-radius:16px;padding:22px 22px 18px;box-shadow:0 24px 60px -12px rgba(15,23,42,.35)}
.modal-icon{width:44px;height:44px;border-radius:11px;background:var(--ok-bg);display:flex;align-items:center;justify-content:center;font-size:20px;margin:0 auto 12px}
.modal h3{text-align:center;font-size:1.02rem;margin-bottom:4px}
.modal p{text-align:center;color:var(--muted);font-size:.85rem;margin-bottom:18px}
.modal .err-msg{text-align:center;color:var(--err);font-size:.82rem;font-weight:600;min-height:18px;margin-top:10px}
.modal input[type=password]{width:100%;height:42px;padding:0 12px;border:1px solid var(--border);border-radius:9px;font:500 .9rem inherit;background:var(--surface-2)}
.modal input[type=password]:focus{outline:none;border-color:var(--primary);background:#fff}
.actions{display:flex;gap:10px;margin-top:6px}
.actions .btn{flex:1;width:auto;margin-top:0;height:42px}
@media (max-width:760px){.grid{grid-template-columns:1fr}}
@media (max-width:520px){
  body{padding:18px 12px}
  .card{padding:15px 16px}
  .field{flex-wrap:wrap}
  .field .value{flex:1 1 auto;text-align:left}
  .cal-row .inp-wrap{flex-wrap:nowrap}
}
</style>
</head>
<body>
<div class="page">

  <header>
    <div class="brand">
      <div class="brand-mark">IH</div>
      <div>
        <h1>Irriga Home</h1>
        <div class="sub">Painel de manutenção</div>
      </div>
    </div>
    <button class="btn btn-ghost btn-sm" id="logout-btn" type="button" onclick="openLogout()" hidden>Sair</button>
  </header>

  <div class="grid">

    <section class="card">
      <div class="card-head"><h2>Leituras em tempo real</h2></div>
      <p class="desc">Atualizado automaticamente a cada 3 segundos</p>

      <div class="field"><span class="ico">📡</span><span class="label">Endereço MAC</span><span class="value" id="lv-irrigador">—</span></div>
      <div class="field"><span class="ico">💧</span><span class="label">Umidade do solo</span><span class="value" id="lv-solo">—</span></div>
      <div class="field"><span class="ico">🌡️</span><span class="label">Temperatura</span><span class="value" id="lv-temp">—</span></div>
      <div class="field"><span class="ico">💨</span><span class="label">Umidade do ar</span><span class="value" id="lv-ar">—</span></div>
      <div class="field"><span class="ico">🚰</span><span class="label">Reservatório</span><span class="value" id="lv-agua">—</span></div>
      <div class="field"><span class="ico">🔌</span><span class="label">Bomba de água</span><span class="value" id="lv-bomba">—</span></div>
      <div class="field"><span class="ico">⚙️</span><span class="label">Modo de acionamento</span><span class="value" id="lv-modo">—</span></div>
      <div class="field"><span class="ico">⏱️</span><span class="label">Rega programada</span><span class="value" id="lv-programmed">—</span></div>
      <div class="field">
        <span class="ico">🗓️</span>
        <span class="label">Agendamentos salvos</span>
        <span class="value" id="lv-sched-count">—</span>
        <button class="btn btn-danger btn-sm" onclick="resetSchedules()">Limpar</button>
      </div>
    </section>

    <section class="card">
      <div class="card-head"><h2>Calibração de sensores</h2></div>
      <p class="desc">Offsets somados à leitura bruta — afetam exibição e controle automático</p>

      <div class="cal-row">
        <div class="top"><span class="label">Umidade do solo</span><span class="range">−30,0 a +30,0</span></div>
        <div class="inp-wrap"><input type="number" id="off-solo" min="-30" max="30" step="0.1" value="0.0"><span class="unit">%</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Temperatura</span><span class="range">−10,0 a +10,0</span></div>
        <div class="inp-wrap"><input type="number" id="off-temp" min="-10" max="10" step="0.1" value="0.0"><span class="unit">°C</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Umidade do ar</span><span class="range">−20,0 a +20,0</span></div>
        <div class="inp-wrap"><input type="number" id="off-ar" min="-20" max="20" step="0.1" value="0.0"><span class="unit">%</span></div>
      </div>

      <hr>
      <p class="subtitle">Calibração de vazão estimada</p>

      <div class="cal-row">
        <div class="top"><span class="label">Vazão nominal atual</span><span class="range">mL/min</span></div>
        <div class="inp-wrap"><input type="number" id="flow-nominal" disabled value="0.0"><span class="unit">mL/min</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Tempo de teste</span><span class="range">5 a 600 s</span></div>
        <div class="inp-wrap"><input type="number" id="flow-test-seconds" min="5" max="600" step="1" value="30"><span class="unit">s</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Volume real medido</span><span class="range">1 a 15000 mL</span></div>
        <div class="inp-wrap"><input type="number" id="flow-real-ml" min="1" max="15000" step="1" value="300"><span class="unit">mL</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Passo 1</span><span class="range">executa a bomba</span></div>
        <div class="inp-wrap"><button class="btn btn-ghost" onclick="iniciarTesteVazao()">Iniciar teste temporizado</button></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Passo 2</span><span class="range">aplica nova vazão nominal</span></div>
        <div class="inp-wrap"><button class="btn" onclick="calibrarVazaoEstimada()">Aplicar calibração</button></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Último teste executado</span><span class="range">data e hora</span></div>
        <div class="inp-wrap"><input type="text" id="flow-last-test" disabled value="Nunca"><span class="unit">—</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Última calibração aplicada</span><span class="range">data e hora</span></div>
        <div class="inp-wrap"><input type="text" id="flow-last-cal" disabled value="Nunca"><span class="unit">—</span></div>
      </div>

      <button class="btn btn-block" onclick="salvar()">Salvar calibração</button>
    </section>

  </div>

  <footer>Irriga Home · E.Cadiz © 2026 · <span id="fw-version">v-</span><br><span id="lv-lastupdate">Aguardando dados…</span></footer>
</div>

<div class="overlay" id="auth-overlay">
  <div class="modal">
    <div class="modal-icon">🔒</div>
    <h3>Acesso restrito</h3>
    <p>Informe a senha de manutenção (3 tentativas)</p>
    <input type="password" id="auth-pass" placeholder="Senha">
    <div class="err-msg" id="auth-msg"></div>
    <button class="btn btn-block" id="auth-btn">Entrar</button>
  </div>
</div>

<div class="overlay" id="logout-modal" hidden>
  <div class="modal">
    <h3>Encerrar sessão?</h3>
    <p>Você precisará informar a senha novamente para fazer ajustes.</p>
    <div class="actions">
      <button class="btn btn-ghost" onclick="closeLogout()">Cancelar</button>
      <button class="btn btn-danger" onclick="confirmLogout()">Sair</button>
    </div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
const $=id=>document.getElementById(id);
const badge=on=>on?'<span class="tag on">Ligada</span>':'<span class="tag off">Desligada</span>';
const fmtDur=s=>{s=Math.max(0,s|0);const m=(s/60)|0,r=s%60;return m?m+' min'+(r?' '+r+' s':''):s+' s';};

function toast(msg,err){const t=$('toast');t.textContent=msg;t.className='toast show'+(err?' err':'');setTimeout(()=>t.className='toast',3000);}

// ---- Autenticação ----
function setLogoutVisible(v){$('logout-btn').hidden=!v;}
function openLogout(){$('logout-modal').hidden=false;}
function closeLogout(){$('logout-modal').hidden=true;}

function confirmLogout(){
  fetch('/api/logout',{method:'POST'}).then(r=>r.json()).then(d=>{
    closeLogout();
    if(d.ok){
      setLogoutVisible(false);
      $('auth-overlay').style.display='flex';
      $('auth-pass').value='';$('auth-pass').disabled=false;$('auth-btn').disabled=false;
      $('auth-msg').textContent='';
      toast('Sessão encerrada');
    }else toast('Não foi possível sair',true);
  }).catch(()=>{closeLogout();toast('Falha na comunicação com o irrigador',true);});
}

function tryAuth(){
  const pass=$('auth-pass').value||'';
  fetch('/api/auth',{method:'POST',body:new URLSearchParams({password:pass})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){
        $('auth-overlay').style.display='none';
        $('auth-msg').textContent='';
        setLogoutVisible(true);
        toast('Acesso liberado');
      }else if(d.locked){
        $('auth-msg').textContent='Bloqueado: limite de tentativas excedido';
        $('auth-btn').disabled=true;$('auth-pass').disabled=true;
      }else{
        $('auth-msg').textContent='Senha inválida — tentativas restantes: '+(d.attemptsLeft??0);
      }
    }).catch(()=>{$('auth-msg').textContent='Falha na comunicação';});
}

document.addEventListener('DOMContentLoaded',()=>{
  $('auth-btn').addEventListener('click',tryAuth);
  $('auth-pass').addEventListener('keyup',e=>{if(e.key==='Enter')tryAuth();});
  setLogoutVisible(false);
  $('logout-modal').addEventListener('click',e=>{if(e.target===$('logout-modal'))closeLogout();});
});

function fmtHora(d){
  return d.toLocaleTimeString('pt-BR',{hour:'2-digit',minute:'2-digit',second:'2-digit'})
    +' · '+d.toLocaleDateString('pt-BR');
}

function atualizar(){
  fetch('/api/data').then(r=>r.json()).then(d=>{
    $('lv-irrigador').textContent=d.macAddress||d.deviceId||'—';
    $('lv-solo').textContent=d.soil+'%';
    $('lv-temp').textContent=d.temp+'\u00b0C';
    $('lv-ar').textContent=d.humidity+'%';
    $('lv-agua').textContent=d.water?'Cheio':'Vazio';
    $('lv-agua').className='value '+(d.water?'ok':'bad');
    $('lv-bomba').innerHTML=badge(d.pump);
    $('lv-modo').textContent=d.mode==='auto'?'Automático':'Manual';
    $('lv-programmed').textContent=fmtDur(d.programmedDurationSec);
    $('lv-sched-count').textContent=String(d.scheduleCount??0);

    $('lv-lastupdate').textContent='Última atualização: '+fmtHora(new Date());
    $('lv-lastupdate').classList.remove('stale');
    $('fw-version').textContent='v'+(d.firmwareVersion || 'n/d');

    if(!window._loaded){
      window._loaded=true;
      $('off-solo').value=d.offSolo;
      $('off-temp').value=d.offTemp;
      $('off-ar').value=d.offAr;
      $('flow-nominal').value=d.nominalFlowMlPerMin;
      $('flow-last-test').value=d.lastFlowTestAt || 'Nunca';
      $('flow-last-cal').value=d.lastFlowCalibrationAt || 'Nunca';
    }
    $('flow-nominal').value=d.nominalFlowMlPerMin;
    $('flow-last-test').value=d.lastFlowTestAt || 'Nunca';
    $('flow-last-cal').value=d.lastFlowCalibrationAt || 'Nunca';
  }).catch(()=>{
    $('lv-lastupdate').classList.add('stale');
  });
}

function resetSchedules(){
  if(!confirm('Confirma zerar todos os agendamentos salvos no ESP32?'))return;
  fetch('/api/schedules/reset',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.ok){toast('Agendamentos apagados com sucesso!');atualizar();}
    else toast('Falha ao apagar agendamentos',true);
  }).catch(()=>toast('Falha na comunicação com o irrigador',true));
}

function salvar(){
  const body=new URLSearchParams({
    offSolo:$('off-solo').value,offTemp:$('off-temp').value,offAr:$('off-ar').value
  });
  fetch('/api/config',{method:'POST',body}).then(r=>r.json())
    .then(d=>toast(d.ok?'Calibração salva com sucesso!':'Erro ao salvar: '+d.error,!d.ok))
    .catch(()=>toast('Falha na comunicação com o irrigador',true));
}

function iniciarTesteVazao(){
  const duration=parseInt($('flow-test-seconds').value,10);
  if(isNaN(duration)||duration<5||duration>600){toast('Tempo de teste inválido',true);return;}
  fetch('/api/flow-test/start',{method:'POST',body:new URLSearchParams({durationSec:String(duration)})})
    .then(r=>r.json()).then(d=>{
      if(d.ok)toast('Teste iniciado por '+d.durationSec+'s. Meça o volume real coletado.');
      else toast('Falha ao iniciar teste: '+(d.error||'erro'),true);
    }).catch(()=>toast('Falha na comunicação com o irrigador',true));
}

function calibrarVazaoEstimada(){
  const duration=parseInt($('flow-test-seconds').value,10);
  const realMl=parseFloat($('flow-real-ml').value);
  if(isNaN(duration)||duration<5||duration>600){toast('Tempo de teste inválido',true);return;}
  if(isNaN(realMl)||realMl<=0){toast('Volume real inválido',true);return;}
  fetch('/api/flow-calibration',{method:'POST',body:new URLSearchParams({durationSec:String(duration),realVolumeMl:String(realMl)})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){
        $('flow-nominal').value=d.nominalFlowMlPerMin;
        toast('Calibração aplicada: '+d.nominalFlowMlPerMin+' mL/min');
      }else{
        toast('Falha na calibração: '+(d.error||'erro'),true);
      }
    }).catch(()=>toast('Falha na comunicação com o irrigador',true));
}

atualizar();
setInterval(atualizar,3000);
</script>
</body>
</html>
)rawhtml";


// ── Implementacao ─────────────────────────────────────────────────────────────

void WebServerManager::begin(SensorManager& sensors, ActuatorManager& actuator,
                IrrigationEventManager& eventManager) {
    _sensors  = &sensors;
    _actuator = &actuator;
  _eventManager = &eventManager;
    _instance = this;
    loadFlowUiState();

    // Registra as rotas HTTP (seguro fazer antes de abrir o servidor)
    server.on("/",          HTTP_GET,  []{ if (_instance) _instance->handleRoot();   });
    server.on("/api/data",  HTTP_GET,  []{ if (_instance) _instance->handleData();   });
    server.on("/api/auth",  HTTP_POST, []{ if (_instance) _instance->handleAuth();   });
    server.on("/api/logout", HTTP_POST, []{ if (_instance) _instance->handleLogout(); });
    server.on("/api/config",HTTP_POST, []{ if (_instance) _instance->handleConfig(); });
    server.on("/api/flow-test/start", HTTP_POST, []{ if (_instance) _instance->handleFlowTestStart(); });
    server.on("/api/flow-calibration", HTTP_POST, []{ if (_instance) _instance->handleFlowCalibration(); });
    server.on("/api/schedules/reset", HTTP_POST, []{ if (_instance) _instance->handleResetSchedules(); });

    // Apenas informa que as rotas estao prontas. A porta 80 sera aberta na _task apos o WiFi conectar.
    fwLogLine("INFO", "WEB", "Rotas HTTP configuradas. Aguardando Wi-Fi para abrir a porta 80...");

    // Task no Core 0 — junto com firebaseTask e mqttConnectTask.
    xTaskCreatePinnedToCore(_task, "webTask", 6144, this, 1, nullptr, 0);
}

void WebServerManager::_task(void* pv) {
    static_cast<WebServerManager*>(pv)->_run();
}

void WebServerManager::_run() {
    bool serverStarted = false;

    for (;;) {
        // Só abre a porta 80 e exibe o log na primeira vez que detectar o Wi-Fi conectado
        if (!serverStarted && WiFi.status() == WL_CONNECTED) {
            server.begin();
            String ipStr = WiFi.localIP().toString();
            fwLogf("INFO", "WEB", "Servidor de manutencao disponivel em http://%s", ipStr.c_str());
            serverStarted = true;
        }

        // Só processa clientes se o servidor já estiver rodando
        if (serverStarted) {
            server.handleClient();
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));  // cede CPU entre requisicoes
    }
}

// ── Handlers ─────────────────────────────────────────────────────────────────

void WebServerManager::handleRoot() {
    server.send_P(200, "text/html; charset=utf-8", MANUTENCAO_HTML);
}

void WebServerManager::handleData() {
  if (!_sensors || !_actuator) {
        server.send(503, "application/json", "{\"error\":\"not ready\"}");
        return;
    }

    SensorData d = _sensors->read();
    String macAddress = formatMacAddress();

    char json[620];
    snprintf(json, sizeof(json),
        "{"
      "\"deviceId\":\"%s\"," 
      "\"macAddress\":\"%s\"," 
      "\"firmwareVersion\":\"%s\"," 
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
          "\"nominalFlowMlPerMin\":%.1f,"
          "\"lastFlowTestAt\":\"%s\","
          "\"lastFlowCalibrationAt\":\"%s\""
        "}",
        getDeviceIdFromMac().c_str(),
        macAddress.c_str(),
        IRRIGAHOME_FIRMWARE_VERSION,
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
        _eventManager ? _eventManager->getNominalFlowRateMlPerMin() : (double)PUMP_NOMINAL_FLOW_ML_PER_MIN,
        lastFlowTestAtIso.c_str(),
        lastFlowCalibrationAtIso.c_str()
    );

    server.send(200, "application/json", json);
}

void WebServerManager::handleAuth() {
  if (!server.hasArg("password")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametros ausentes\"}");
    return;
  }

  String pass = server.arg("password");
  const char* DEFAULT_PASS = WEBSERVER_PASS;

  if (_authValid) {
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (pass.equals(DEFAULT_PASS)) {
    _authValid = true;
    server.send(200, "application/json", "{\"ok\":true}");
    fwLogLine("INFO", "WEB", "Usuario autenticado com sucesso");
    return;
  }

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
  fwLogLine("INFO", "WEB", "Usuario saiu da manutencao");
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
    
    if (!server.hasArg("offSolo") || !server.hasArg("offTemp") || !server.hasArg("offAr")) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametros ausentes\"}");
        return;
    }

    float offTemp   = server.arg("offTemp").toFloat();
    float offSolo   = server.arg("offSolo").toFloat();
    float offAr     = server.arg("offAr").toFloat();

    if (offTemp   < -10.0f || offTemp   > 10.0f  ||
        offSolo   < -30.0f || offSolo   > 30.0f   ||
      offAr     < -20.0f || offAr     > 20.0f) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"valor fora do limite permitido\"}");
        return;
    }

    // ── CORREÇÃO: Seção Crítica (Spinlock) ──
    portENTER_CRITICAL(&_pendingMux);
    _pending.offTemp    = offTemp;
    _pending.offSolo    = offSolo;
    _pending.offUmidAr  = offAr;
    _pending.pending    = true;   
    portEXIT_CRITICAL(&_pendingMux);

    server.send(200, "application/json", "{\"ok\":true}");
}

// Chamado pelo loop() do main.ino no Core 1.
void WebServerManager::applyPendingConfig() {
    // ── CORREÇÃO: Leitura e cópia segura dentro do Spinlock ──
    PendingConfig localCopy;
    
    portENTER_CRITICAL(&_pendingMux);
    if (!_pending.pending) {
        portEXIT_CRITICAL(&_pendingMux);
        return;
    }
    
    localCopy = _pending;
    _pending.pending = false;
    portEXIT_CRITICAL(&_pendingMux);

    if (_sensors) {
        _sensors->setOffsetTemperatura(localCopy.offTemp);
        _sensors->setOffsetUmidadeSolo(localCopy.offSolo);
        _sensors->setOffsetUmidadeAr(localCopy.offUmidAr);
        _sensors->flushOffsets();   
    }

    fwLogLine("INFO", "WEB", "Calibracao aplicada via pagina de manutencao");
    fwLogf("INFO", "WEB", "offSolo=%.1f | offTemp=%.1f | offAr=%.1f",
         localCopy.offSolo, localCopy.offTemp, localCopy.offUmidAr);
}

void WebServerManager::handleFlowTestStart() {
  if (!_authValid) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  if (!_actuator || !_eventManager || !_sensors) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"not ready\"}");
    return;
  }

  if (!server.hasArg("durationSec")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametros ausentes\"}");
    return;
  }

  int durationSec = server.arg("durationSec").toInt();
  if (durationSec < 5 || durationSec > 600) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"duracao invalida\"}");
    return;
  }

  if (_actuator->isLigado()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"pump already on\"}");
    return;
  }

  SensorData data = _sensors->read();
  if (!data.nivelAgua) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"no water\"}");
    return;
  }

  _actuator->setCurrentTrigger(TRIGGER_MANUAL);
  _actuator->ligar();
  _eventManager->onIrrigationStart(TRIGGER_MANUAL);
  unsigned long now = millis();
  _actuator->setActiveUntil(now + ((unsigned long)durationSec * 1000UL));
  lastFlowTestAtIso = formatIso8601Now();
  lastFlowTestDurationSec = durationSec;
  saveFlowUiState();

  char json[120];
  snprintf(json, sizeof(json), "{\"ok\":true,\"durationSec\":%d}", durationSec);
  server.send(200, "application/json", json);
}

void WebServerManager::handleFlowCalibration() {
  if (!_authValid) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  if (!_eventManager) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"not ready\"}");
    return;
  }

  if (_actuator && _actuator->isLigado()) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"pump already on\"}");
    return;
  }

  if (!server.hasArg("durationSec") || !server.hasArg("realVolumeMl")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametros ausentes\"}");
    return;
  }

  float durationSec = server.arg("durationSec").toFloat();
  float realVolumeMl = server.arg("realVolumeMl").toFloat();

  if (!(durationSec >= 5.0f && durationSec <= 600.0f) || !(realVolumeMl > 0.0f && realVolumeMl <= 15000.0f)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"valores invalidos\"}");
    return;
  }

  float nominalMlPerMin = (realVolumeMl * 60.0f) / durationSec;
  if (!(nominalMlPerMin >= 50.0f && nominalMlPerMin <= 5000.0f)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"resultado fora da faixa (50-5000 mL/min)\"}");
    return;
  }

  _eventManager->setNominalFlowRateMlPerMin(nominalMlPerMin);
  lastFlowCalibrationAtIso = formatIso8601Now();
  saveFlowUiState();

  char json[180];
  snprintf(json, sizeof(json),
       "{\"ok\":true,\"nominalFlowMlPerMin\":%.1f,\"durationSec\":%.0f,\"realVolumeMl\":%.1f}",
       nominalMlPerMin, durationSec, realVolumeMl);
  server.send(200, "application/json", json);
}