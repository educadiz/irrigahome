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
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <time.h>

// ── Servidor e instancia global da task ──────────────────────────────────────
static WebServer          server(80);
static WebServerManager* _instance = nullptr;
// Simple authentication state (in-memory). Allows up to 3 attempts.
static bool               _authValid = false;
static int                _authAttemptsLeft = 3;
// Flags de estado de senha pós-autenticação
static bool               _requiresPasswordChange = false;
static Preferences        flowPrefs;
static constexpr const char* FLOW_UI_NAMESPACE = "irrigahome";
static constexpr const char* FLOW_UI_KEY_LAST_TEST_AT = "flow_last_test_at";
static constexpr const char* FLOW_UI_KEY_LAST_CAL_AT = "flow_last_cal_at";
static constexpr const char* FLOW_UI_KEY_LAST_TEST_SEC = "flow_last_test_sec";

// ── NVS namespace dedicado para gerenciamento de senha ───────────────────────
static Preferences        authPrefs;
static constexpr const char* AUTH_NS           = "webauth";
static constexpr const char* KEY_HASH          = "pass_hash";
static constexpr const char* KEY_SALT          = "pass_salt";
static constexpr const char* KEY_KIND          = "pass_kind";
static constexpr const char* KEY_TEMP_EXP      = "temp_exp";

// Tipos de senha — persistido em NVS como uint8_t
// 0=senha-padrao MAC, 1=senha personalizada, 2=senha temporaria
enum PasswordKind : uint8_t { PASS_DEFAULT_MAC = 0, PASS_CUSTOM = 1, PASS_TEMP = 2 };

static String lastFlowTestAtIso;
static String lastFlowCalibrationAtIso;
static int lastFlowTestDurationSec = 0;

// ── Utilitários de hash e aleatoriedade ──────────────────────────────────────

static String sha256Hex(const String& input) {
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  const char* hex = "0123456789abcdef";
  char out[65];
  for (int i = 0; i < 32; i++) {
    out[i * 2]     = hex[(digest[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  out[64] = '\0';
  return String(out);
}

static String randomSaltHex(size_t bytes = 16) {
  const char* hex = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; i++) {
    uint8_t b = (uint8_t)(esp_random() & 0xFF);
    out += hex[(b >> 4) & 0x0F];
    out += hex[b & 0x0F];
  }
  return out;
}

static bool constantTimeEquals(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// ── Helpers da senha ─────────────────────────────────────────────────────────

// Retorna os ultimos 4 caracteres do MAC sem separadores, em minusculo.
static String buildDefaultPasswordFromMac() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.replace("-", "");
  mac.toLowerCase();
  if (mac.length() < 4) return "0000";
  return mac.substring(mac.length() - 4);
}

// Persiste hash+salt+tipo em NVS.
static void persistPassword(const String& plaintext, PasswordKind kind) {
  String salt = randomSaltHex();
  String hash = sha256Hex(salt + plaintext);
  if (!authPrefs.begin(AUTH_NS, false)) return;
  authPrefs.putString(KEY_HASH, hash);
  authPrefs.putString(KEY_SALT, salt);
  authPrefs.putUChar(KEY_KIND, (uint8_t)kind);
  authPrefs.remove(KEY_TEMP_EXP); // limpa expiração ao trocar para não-temporaria
  authPrefs.end();
}

// Persiste senha temporária com expiração (epoch Unix).
static void persistTempPassword(const String& plaintext, time_t expiresAt) {
  String salt = randomSaltHex();
  String hash = sha256Hex(salt + plaintext);
  if (!authPrefs.begin(AUTH_NS, false)) return;
  authPrefs.putString(KEY_HASH, hash);
  authPrefs.putString(KEY_SALT, salt);
  authPrefs.putUChar(KEY_KIND, (uint8_t)PASS_TEMP);
  authPrefs.putULong(KEY_TEMP_EXP, (unsigned long)expiresAt);
  authPrefs.end();
}

// Inicializa a senha padrão na primeira execução (se não houver hash na NVS).
static void ensureDefaultPassword() {
  // Tenta ler em modo readonly; se falhar (namespace nao existe), cai no path de escrita.
  bool hasHash = false;
  if (authPrefs.begin(AUTH_NS, true)) {
    String stored = authPrefs.getString(KEY_HASH, "");
    hasHash = (stored.length() > 0);
    authPrefs.end();
  }
  if (hasHash) return; // já tem senha persistida
  // Primeira execução: deriva senha do MAC e persiste
  persistPassword(buildDefaultPasswordFromMac(), PASS_DEFAULT_MAC);
  fwLogLine("INFO", "WEB", "Senha padrao gerada a partir do MAC e persistida na NVS");
}

// Verifica se uma senha em texto plano bate com o hash da NVS.
// Retorna true e preenche outKind/outTempExp se válida.
static bool verifyPassword(const String& plaintext, PasswordKind* outKind, time_t* outTempExp) {
  if (!authPrefs.begin(AUTH_NS, true)) return false;
  String storedHash = authPrefs.getString(KEY_HASH, "");
  String storedSalt = authPrefs.getString(KEY_SALT, "");
  PasswordKind kind = (PasswordKind)authPrefs.getUChar(KEY_KIND, (uint8_t)PASS_DEFAULT_MAC);
  time_t tempExp    = (time_t)authPrefs.getULong(KEY_TEMP_EXP, 0);
  authPrefs.end();

  if (storedHash.length() == 0 || storedSalt.length() == 0) return false;

  String candidate = sha256Hex(storedSalt + plaintext);
  if (!constantTimeEquals(candidate, storedHash)) return false;

  if (outKind)    *outKind = kind;
  if (outTempExp) *outTempExp = tempExp;
  return true;
}

static void resetAuthState() {
  _authValid = false;
  _authAttemptsLeft = 3;
  _requiresPasswordChange = false;
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
<title>Irriga Home · Painel de manutenção</title>
<style>
:root{
  --bg:#f5f6f4;--bg-grad:radial-gradient(1200px 600px at 100% -10%,#eaf2ec 0%,transparent 60%),radial-gradient(900px 500px at -10% 110%,#eef1ec 0%,transparent 55%),#f5f6f4;
  --surface:#fff;--surface-2:#fafbf9;--border:#e5e8e2;--border-strong:#d6dbd1;
  --text:#0f1a15;--text-2:#28372f;--muted:#6a7973;--soft:#3a4a42;
  --primary:#0f6b3f;--primary-d:#0a5030;--primary-soft:#e4f1e9;--primary-ring:rgba(15,107,63,.14);
  --accent:#0a5030;
  --ok:#0f6b3f;--ok-bg:#e4f1e9;--err:#b3261e;--err-bg:#fbecea;--warn:#8a5a00;--warn-bg:#fdf2d6;
  --radius:12px;--radius-lg:16px;
  --shadow-sm:0 1px 2px rgba(15,26,21,.04),0 1px 1px rgba(15,26,21,.03);
  --shadow:0 1px 2px rgba(15,26,21,.04),0 12px 32px -18px rgba(15,26,21,.18);
  --mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace;
}
*{box-sizing:border-box;margin:0;padding:0}
html,body{-webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility}
body{min-height:100vh;padding:36px 20px;background:var(--bg-grad);color:var(--text);
  font:14.5px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  display:flex;justify-content:center;letter-spacing:-.005em}
.page{width:100%;max-width:1040px;display:flex;flex-direction:column;gap:20px}

header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:2px 4px 6px}
.brand{display:flex;align-items:center;gap:14px}
.brand-mark{width:44px;height:44px;border-radius:12px;background:linear-gradient(160deg,var(--primary) 0%,var(--primary-d) 100%);
  display:flex;align-items:center;justify-content:center;color:#fff;flex-shrink:0;
  box-shadow:0 6px 16px -8px rgba(15,107,63,.5),inset 0 1px 0 rgba(255,255,255,.15)}
.brand-mark svg{width:22px;height:22px}
.brand h1{font-size:1.05rem;font-weight:650;letter-spacing:-.015em;color:var(--text)}
.brand .sub{font-size:.75rem;color:var(--muted);font-weight:500;margin-top:2px;letter-spacing:.005em}
.header-actions{display:flex;gap:8px;align-items:center}
.status-pill{display:inline-flex;align-items:center;gap:6px;padding:6px 11px;border-radius:999px;
  background:var(--surface);border:1px solid var(--border);font-size:.72rem;font-weight:600;color:var(--soft);
  box-shadow:var(--shadow-sm)}
.status-dot{width:7px;height:7px;border-radius:50%;background:var(--ok);
  box-shadow:0 0 0 3px var(--ok-bg);animation:pulse 2s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.55}}

.btn{height:36px;padding:0 14px;border:0;border-radius:9px;background:var(--primary);color:#fff;
  font:600 .82rem/1 inherit;letter-spacing:.005em;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;gap:6px;
  transition:background .15s,transform .1s,box-shadow .15s;box-shadow:0 1px 0 rgba(255,255,255,.15) inset,0 1px 2px rgba(15,107,63,.2)}
.btn:hover{background:var(--primary-d)}
.btn:active{transform:translateY(1px)}
.btn:focus-visible{outline:none;box-shadow:0 0 0 3px var(--primary-ring)}
.btn-block{width:100%;height:44px;margin-top:16px;font-size:.88rem}
.btn-ghost{background:var(--surface);color:var(--soft);border:1px solid var(--border);box-shadow:var(--shadow-sm)}
.btn-ghost:hover{background:var(--surface-2);border-color:var(--border-strong);color:var(--text-2)}
.btn-sm{height:32px;padding:0 11px;font-size:.75rem;border-radius:8px}
.btn-danger{background:var(--surface);color:var(--err);border:1px solid #f0c8c3;box-shadow:var(--shadow-sm)}
.btn-danger:hover{background:var(--err-bg)}

.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:20px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius-lg);padding:24px;box-shadow:var(--shadow);position:relative}
.card-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:2px}
.card-head h2{font-size:.95rem;font-weight:650;letter-spacing:-.01em;color:var(--text)}
.card-kicker{font-size:.65rem;font-weight:700;letter-spacing:.14em;text-transform:uppercase;color:var(--muted)}
.card .desc{font-size:.78rem;color:var(--muted);margin:6px 0 18px;line-height:1.5}

.field{display:flex;align-items:center;gap:12px;padding:12px 0;border-bottom:1px solid var(--border)}
.field:last-of-type{border-bottom:0}
.field .ico{width:28px;height:28px;border-radius:8px;background:var(--surface-2);border:1px solid var(--border);
  display:flex;align-items:center;justify-content:center;flex-shrink:0;color:var(--soft)}
.field .ico svg{width:14px;height:14px}
.field .label{flex:1;font-size:.82rem;font-weight:500;color:var(--text-2);letter-spacing:-.003em}
.field .value{font-family:var(--mono);font-weight:600;font-size:.82rem;color:var(--text);text-align:right;letter-spacing:-.01em}
.value.ok{color:var(--ok)}
.value.bad{color:var(--err)}

.tag{display:inline-flex;align-items:center;gap:5px;padding:3px 9px;border-radius:999px;font:600 .68rem/1.4 inherit;
  letter-spacing:.02em;font-family:inherit}
.tag::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor}
.tag.on{background:var(--ok-bg);color:var(--ok)}
.tag.off{background:#eef1ec;color:var(--muted)}

.section-title{font-size:.65rem;font-weight:700;text-transform:uppercase;letter-spacing:.14em;color:var(--muted);margin:20px 0 10px;
  display:flex;align-items:center;gap:8px}
.section-title::after{content:"";flex:1;height:1px;background:var(--border)}

.cal-row{padding:12px 0;border-bottom:1px solid var(--border)}
.cal-row:last-of-type{border-bottom:0}
.cal-row .top{display:flex;justify-content:space-between;align-items:baseline;gap:10px;margin-bottom:8px}
.cal-row .label{font-size:.82rem;font-weight:550;color:var(--text-2)}
.cal-row .range{font-size:.68rem;color:var(--muted);font-family:var(--mono);letter-spacing:-.005em}
.inp-wrap{display:flex;align-items:center;gap:8px}
input[type=checkbox]{width:16px;height:16px;accent-color:var(--primary);cursor:pointer}
input[type=number],input[type=text]{flex:1;height:38px;padding:0 12px;border:1px solid var(--border);border-radius:8px;
  background:var(--surface-2);color:var(--text);font:600 .84rem var(--mono);text-align:right;min-width:0;transition:border-color .12s,box-shadow .12s,background .12s}
input[type=text]{text-align:left;font-family:inherit;font-weight:500;color:var(--muted)}
input[type=number]:focus,input[type=text]:focus{outline:none;border-color:var(--primary);background:#fff;box-shadow:0 0 0 3px var(--primary-ring)}
input:disabled{opacity:.7;cursor:not-allowed}
.unit{font-size:.7rem;color:var(--muted);font-weight:600;min-width:36px;text-align:left;font-family:var(--mono);letter-spacing:.02em}
.toggle-line{display:flex;align-items:center;gap:10px;font-size:.82rem;font-weight:500;color:var(--text-2);cursor:pointer;line-height:1.4}

hr{border:0;border-top:1px solid var(--border);margin:22px 0 0}

footer{display:flex;justify-content:space-between;align-items:center;gap:12px;color:var(--muted);font-size:.72rem;padding:12px 4px 4px;flex-wrap:wrap}
footer .brand-line{font-weight:500;letter-spacing:.005em}
footer #fw-version{font-family:var(--mono);color:var(--soft);font-weight:600}
footer #lv-lastupdate{font-family:var(--mono);font-size:.68rem;letter-spacing:-.005em;opacity:.85}
footer #lv-lastupdate.stale{color:var(--err);opacity:1;font-weight:600}

.toast{position:fixed;left:50%;bottom:28px;transform:translateX(-50%) translateY(8px);padding:12px 20px;border-radius:10px;
  background:var(--text);color:#fff;font:600 .82rem/1.2 inherit;box-shadow:0 16px 40px -8px rgba(0,0,0,.35);
  opacity:0;pointer-events:none;transition:opacity .2s,transform .2s;max-width:calc(100vw - 40px)}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
.toast.err{background:var(--err)}

.overlay{position:fixed;inset:0;background:rgba(15,26,21,.55);backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);
  display:flex;align-items:center;justify-content:center;padding:20px;z-index:50}
.overlay[hidden]{display:none}
.modal{width:min(100%,400px);background:#fff;border-radius:16px;padding:28px 26px 22px;
  box-shadow:0 24px 60px -12px rgba(15,26,21,.4),0 0 0 1px var(--border)}
.modal-icon{width:48px;height:48px;border-radius:12px;background:var(--primary-soft);color:var(--primary);
  display:flex;align-items:center;justify-content:center;margin:0 auto 16px}
.modal-icon svg{width:22px;height:22px}
.modal h3{text-align:center;font-size:1.05rem;font-weight:650;margin-bottom:6px;letter-spacing:-.015em}
.modal p{text-align:center;color:var(--muted);font-size:.82rem;margin-bottom:18px;line-height:1.55}
.modal .err-msg{text-align:center;color:var(--err);font-size:.78rem;font-weight:600;min-height:18px;margin-top:10px}
.modal input[type=password],.modal input[type=email]{width:100%;height:44px;padding:0 14px;border:1px solid var(--border);
  border-radius:10px;font:500 .88rem inherit;background:var(--surface-2);text-align:left;transition:border-color .12s,box-shadow .12s,background .12s}
.modal input[type=password]:focus,.modal input[type=email]:focus{outline:none;border-color:var(--primary);background:#fff;box-shadow:0 0 0 3px var(--primary-ring)}
.actions{display:flex;gap:10px;margin-top:14px}
.actions .btn{flex:1;width:auto;margin-top:0;height:44px}

@media (max-width:820px){.grid{grid-template-columns:1fr}}
@media (max-width:520px){
  body{padding:20px 12px}
  .card{padding:20px 18px;border-radius:14px}
  header{flex-wrap:wrap;gap:10px}
  .header-actions{width:100%;justify-content:flex-end}
  .field{flex-wrap:wrap}
  .field .value{flex:1 1 auto;text-align:left}
  footer{flex-direction:column;align-items:flex-start;gap:6px}
}
</style>
</head>
<body>
<div class="page">

  <header>
    <div class="brand">
      <div class="brand-mark" aria-hidden="true">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M7 20c0-6 5-10 10-10-2 8-6 10-10 10z"/><path d="M7 20c0-3-1-6-3-8"/></svg>
      </div>
      <div>
        <h1>Irriga Home</h1>
        <div class="sub">Painel de manutenção · sistema de irrigação</div>
      </div>
    </div>
    <div class="header-actions">
      <span class="status-pill"><span class="status-dot"></span>Conectado</span>
      <button class="btn btn-ghost btn-sm" id="chpass-btn" type="button" onclick="openChangePassword()" hidden>Alterar senha</button>
      <button class="btn btn-ghost btn-sm" id="logout-btn" type="button" onclick="openLogout()" hidden>Sair</button>
    </div>
  </header>

  <div class="grid">

    <section class="card">
      <div class="card-head">
        <h2>Estado atual do sistema</h2>
        <span class="card-kicker">Ao vivo</span>
      </div>
      <p class="desc">Leituras dos sensores atualizadas automaticamente a cada 3 segundos.</p>

      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12a7 7 0 0 1 14 0"/><path d="M8.5 12a3.5 3.5 0 0 1 7 0"/><circle cx="12" cy="12" r="1"/></svg></span><span class="label">Identificação do dispositivo</span><span class="value" id="lv-irrigador">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3s6 7 6 12a6 6 0 0 1-12 0c0-5 6-12 6-12z"/></svg></span><span class="label">Umidade do solo</span><span class="value" id="lv-solo">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 4a2 2 0 1 1 4 0v10.5a4 4 0 1 1-4 0z"/></svg></span><span class="label">Temperatura ambiente</span><span class="value" id="lv-temp">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 8h13a3 3 0 1 0-3-3"/><path d="M3 13h17a3 3 0 1 1-3 3"/><path d="M3 18h9"/></svg></span><span class="label">Umidade do ar</span><span class="value" id="lv-ar">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="5" y="4" width="14" height="16" rx="2"/><path d="M5 14h14"/><path d="M9 4v3"/><path d="M15 4v3"/></svg></span><span class="label">Nível do reservatório</span><span class="value ok" id="lv-agua">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8"/><path d="M12 4v4"/><path d="M12 16v4"/><path d="M4 12h4"/><path d="M16 12h4"/></svg></span><span class="label">Bomba de água</span><span class="value" id="lv-bomba">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></svg></span><span class="label">Modo de acionamento</span><span class="value" id="lv-modo">—</span></div>
      <div class="field"><span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg></span><span class="label">Próxima rega programada</span><span class="value" id="lv-programmed">—</span></div>
      <div class="field">
        <span class="ico"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="5" width="18" height="16" rx="2"/><path d="M3 10h18"/><path d="M8 3v4"/><path d="M16 3v4"/></svg></span>
        <span class="label">Agendamentos salvos</span>
        <span class="value" id="lv-sched-count">—</span>
        <button class="btn btn-danger btn-sm" onclick="resetSchedules()">Limpar</button>
      </div>
    </section>

    <section class="card">
      <div class="card-head">
        <h2>Configurações e calibração</h2>
        <span class="card-kicker">Ajustes</span>
      </div>
      <p class="desc">Aplique correções aos sensores comparando com um instrumento de referência.</p>

      <p class="section-title">Offset dos sensores</p>

      <div class="cal-row">
        <div class="top"><span class="label">Umidade do solo</span><span class="range">−30 a +30</span></div>
        <div class="inp-wrap"><input type="number" id="off-solo" min="-30" max="30" step="0.1" value="0.0"><span class="unit">%</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Temperatura</span><span class="range">−10 a +10</span></div>
        <div class="inp-wrap"><input type="number" id="off-temp" min="-10" max="10" step="0.1" value="0.0"><span class="unit">°C</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Umidade do ar</span><span class="range">−20 a +20</span></div>
        <div class="inp-wrap"><input type="number" id="off-ar" min="-20" max="20" step="0.1" value="0.0"><span class="unit">%</span></div>
      </div>

      <p class="section-title">Notificações</p>
      <div class="cal-row" style="border-bottom:0;padding-top:4px">
        <label class="toggle-line" for="email-notify">
          <input type="checkbox" id="email-notify" checked>
          <span>Receber e-mail sempre que uma rega for realizada</span>
        </label>
      </div>

      <p class="section-title">Calibração da bomba</p>

      <div class="cal-row">
        <div class="top"><span class="label">Vazão nominal em uso</span><span class="range">valor atual</span></div>
        <div class="inp-wrap"><input type="number" id="flow-nominal" disabled value="0.0"><span class="unit">mL/min</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Duração do teste</span><span class="range">5 a 600 s</span></div>
        <div class="inp-wrap"><input type="number" id="flow-test-seconds" min="5" max="600" step="1" value="30"><span class="unit">s</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Volume medido</span><span class="range">1 a 15000 mL</span></div>
        <div class="inp-wrap"><input type="number" id="flow-real-ml" min="1" max="15000" step="1" value="300"><span class="unit">mL</span></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">1. Executar teste da bomba</span><span class="range">colete a água num recipiente</span></div>
        <div class="inp-wrap"><button class="btn btn-ghost" onclick="iniciarTesteVazao()">Iniciar teste</button></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">2. Aplicar calibração</span><span class="range">usa o volume medido</span></div>
        <div class="inp-wrap"><button class="btn" onclick="calibrarVazaoEstimada()">Aplicar calibração</button></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Último teste executado</span><span class="range">registro</span></div>
        <div class="inp-wrap"><input type="text" id="flow-last-test" disabled value="Nunca"></div>
      </div>
      <div class="cal-row">
        <div class="top"><span class="label">Última calibração aplicada</span><span class="range">registro</span></div>
        <div class="inp-wrap"><input type="text" id="flow-last-cal" disabled value="Nunca"></div>
      </div>

      <button class="btn btn-block" onclick="salvar()">Salvar configurações</button>
    </section>

  </div>

  <footer>
    <span class="brand-line">Irriga Home · E.Cadiz © 2026 · <span id="fw-version">v-</span></span>
    <span id="lv-lastupdate">Aguardando dados…</span>
  </footer>
</div>

<div class="overlay" id="auth-overlay">
  <div class="modal">
    <div class="modal-icon"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg></div>
    <h3>Acesso restrito</h3>
    <p>Digite a senha de manutenção para continuar. Você tem 3 tentativas disponíveis.</p>
    <input type="password" id="auth-pass" placeholder="Senha de acesso">
    <div class="err-msg" id="auth-msg"></div>
    <button class="btn btn-block" id="auth-btn">Entrar</button>
    <button class="btn btn-ghost btn-block" style="margin-top:8px;font-size:.8rem" onclick="openRecover()">Esqueci minha senha</button>
  </div>
</div>

<div class="overlay" id="logout-modal" hidden>
  <div class="modal">
    <h3>Encerrar sessão?</h3>
    <p>Será necessário autenticar novamente para acessar o painel de manutenção.</p>
    <div class="actions">
      <button class="btn btn-ghost" onclick="closeLogout()">Cancelar</button>
      <button class="btn btn-danger" onclick="confirmLogout()">Sair</button>
    </div>
  </div>
</div>

<div class="overlay" id="chpass-modal" hidden>
  <div class="modal">
    <div class="modal-icon"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="8" cy="15" r="4"/><path d="M10.85 12.15 20 3"/><path d="m18 5 2 2"/><path d="m15 8 2 2"/></svg></div>
    <h3>Alterar senha</h3>
    <p>A nova senha é armazenada de forma segura no próprio dispositivo.</p>
    <input type="password" id="cp-new" placeholder="Nova senha" autocomplete="new-password">
    <input type="password" id="cp-confirm" placeholder="Confirmar nova senha" autocomplete="new-password" style="margin-top:8px">
    <div class="err-msg" id="cp-msg"></div>
    <div class="actions">
      <button class="btn btn-ghost" id="cp-cancel-btn" onclick="closeChangePassword()">Cancelar</button>
      <button class="btn" onclick="submitChangePassword()">Salvar</button>
    </div>
  </div>
</div>

<div class="overlay" id="recover-modal" hidden>
  <div class="modal">
    <div class="modal-icon"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="m3 7 9 6 9-6"/></svg></div>
    <h3>Recuperar senha</h3>
    <p>Informe o e-mail cadastrado no aplicativo. Enviaremos uma senha temporária válida por 30 minutos.</p>
    <input type="email" id="rec-email" placeholder="seu@email.com" autocomplete="email">
    <div class="err-msg" id="rec-msg"></div>
    <div class="actions">
      <button class="btn btn-ghost" onclick="closeRecover()">Cancelar</button>
      <button class="btn" onclick="submitRecover()">Enviar</button>
    </div>
  </div>
</div>

<div class="overlay" id="force-chpass-modal" hidden>
  <div class="modal">
    <div class="modal-icon" style="background:var(--warn-bg);color:var(--warn)"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3 2 21h20L12 3z"/><path d="M12 10v5"/><path d="M12 18h.01"/></svg></div>
    <h3>Defina uma nova senha</h3>
    <p>Você entrou com uma senha temporária. Crie uma senha pessoal para continuar utilizando o painel.</p>
    <input type="password" id="fc-new" placeholder="Nova senha" autocomplete="new-password">
    <input type="password" id="fc-confirm" placeholder="Confirmar nova senha" autocomplete="new-password" style="margin-top:8px">
    <div class="err-msg" id="fc-msg"></div>
    <div class="actions">
      <button class="btn" style="width:100%" onclick="submitForceChangePassword()">Definir senha</button>
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
function setLogoutVisible(v){$('logout-btn').hidden=!v;$('chpass-btn').hidden=!v;}
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
    }else toast('Não foi possível sair agora',true);
  }).catch(()=>{closeLogout();toast('Sem conexão com o irrigador',true);});
}

function tryAuth(){
  const pass=$('auth-pass').value||'';
  fetch('/api/auth',{method:'POST',body:new URLSearchParams({password:pass})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){
        $('auth-overlay').style.display='none';
        $('auth-msg').textContent='';
        setLogoutVisible(true);
        if(d.requiresPasswordChange){
          $('force-chpass-modal').hidden=false;
        }else{
          toast('Tudo certo, acesso liberado');
        }
      }else if(d.locked){
        $('auth-msg').textContent='Bloqueado: limite de tentativas atingido';
        $('auth-btn').disabled=true;$('auth-pass').disabled=true;
      }else{
        $('auth-msg').textContent=(d.error||'Senha incorreta')+' — restam '+(d.attemptsLeft??0)+' tentativa(s)';
      }
    }).catch(()=>{$('auth-msg').textContent='Sem conexão com o irrigador';});
}

// ---- Alterar senha ----
function openChangePassword(){$('cp-new').value='';$('cp-confirm').value='';$('cp-msg').textContent='';$('chpass-modal').hidden=false;}
function closeChangePassword(){$('chpass-modal').hidden=true;}

function submitChangePassword(){
  const np=$('cp-new').value||'';
  const nc=$('cp-confirm').value||'';
  if(np.length<4){$('cp-msg').textContent='A senha precisa ter ao menos 4 caracteres';return;}
  if(np!==nc){$('cp-msg').textContent='As senhas não são iguais';return;}
  fetch('/api/password/change',{method:'POST',body:new URLSearchParams({newPassword:np})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){closeChangePassword();toast('Senha alterada com sucesso!');}
      else{$('cp-msg').textContent=d.error||'Não foi possível alterar a senha';}
    }).catch(()=>{$('cp-msg').textContent='Sem conexão com o irrigador';});
}

// ---- Forçar troca (pós senha temporária) ----
function submitForceChangePassword(){
  const np=$('fc-new').value||'';
  const nc=$('fc-confirm').value||'';
  if(np.length<4){$('fc-msg').textContent='A senha precisa ter ao menos 4 caracteres';return;}
  if(np!==nc){$('fc-msg').textContent='As senhas não são iguais';return;}
  fetch('/api/password/change',{method:'POST',body:new URLSearchParams({newPassword:np})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){$('force-chpass-modal').hidden=true;toast('Senha definida! Acesso liberado.');}
      else{$('fc-msg').textContent=d.error||'Não foi possível definir a senha';}
    }).catch(()=>{$('fc-msg').textContent='Sem conexão com o irrigador';});
}

// ---- Recuperação de senha ----
function openRecover(){$('rec-email').value='';$('rec-msg').textContent='';$('recover-modal').hidden=false;}
function closeRecover(){$('recover-modal').hidden=true;}

function submitRecover(){
  const email=$('rec-email').value.trim();
  if(!email||!email.includes('@')){$('rec-msg').textContent='Informe um e-mail válido';return;}
  $('rec-msg').textContent='Enviando…';
  fetch('/api/password/recover',{method:'POST',body:new URLSearchParams({email:email})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){closeRecover();toast('Enviamos uma senha temporária. Verifique seu e-mail (válida por 30 min).');}
      else{$('rec-msg').textContent=d.error||'Não foi possível solicitar a recuperação';}
    }).catch(()=>{$('rec-msg').textContent='Sem conexão com o irrigador';});
}

document.addEventListener('DOMContentLoaded',()=>{
  $('auth-btn').addEventListener('click',tryAuth);
  $('auth-pass').addEventListener('keyup',e=>{if(e.key==='Enter')tryAuth();});
  setLogoutVisible(false);
  $('logout-modal').addEventListener('click',e=>{if(e.target===$('logout-modal'))closeLogout();});
  $('chpass-modal').addEventListener('click',e=>{if(e.target===$('chpass-modal'))closeChangePassword();});
  $('recover-modal').addEventListener('click',e=>{if(e.target===$('recover-modal'))closeRecover();});
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
      $('email-notify').checked=d.emailNotificationEnabled!==false;
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
  if(!confirm('Tem certeza que deseja apagar todos os agendamentos salvos no irrigador?'))return;
  fetch('/api/schedules/reset',{method:'POST'}).then(r=>r.json()).then(d=>{
    if(d.ok){toast('Agendamentos apagados com sucesso!');atualizar();}
    else toast('Não foi possível apagar os agendamentos',true);
  }).catch(()=>toast('Sem conexão com o irrigador',true));
}

function salvar(){
  const body=new URLSearchParams({
    offSolo:$('off-solo').value,offTemp:$('off-temp').value,offAr:$('off-ar').value,
    emailNotify:$('email-notify').checked?'1':'0'
  });
  fetch('/api/config',{method:'POST',body}).then(r=>r.json())
    .then(d=>toast(d.ok?'Ajustes salvos!':'Não foi possível salvar: '+d.error,!d.ok))
    .catch(()=>toast('Sem conexão com o irrigador',true));
}

function iniciarTesteVazao(){
  const duration=parseInt($('flow-test-seconds').value,10);
  if(isNaN(duration)||duration<5||duration>600){toast('Duração do teste inválida',true);return;}
  fetch('/api/flow-test/start',{method:'POST',body:new URLSearchParams({durationSec:String(duration)})})
    .then(r=>r.json()).then(d=>{
      if(d.ok)toast('Teste iniciado por '+d.durationSec+' s. Meça o volume coletado.');
      else toast('Não foi possível iniciar o teste: '+(d.error||'erro'),true);
    }).catch(()=>toast('Sem conexão com o irrigador',true));
}

function calibrarVazaoEstimada(){
  const duration=parseInt($('flow-test-seconds').value,10);
  const realMl=parseFloat($('flow-real-ml').value);
  if(isNaN(duration)||duration<5||duration>600){toast('Duração do teste inválida',true);return;}
  if(isNaN(realMl)||realMl<=0){toast('Volume medido inválido',true);return;}
  fetch('/api/flow-calibration',{method:'POST',body:new URLSearchParams({durationSec:String(duration),realVolumeMl:String(realMl)})})
    .then(r=>r.json()).then(d=>{
      if(d.ok){
        $('flow-nominal').value=d.nominalFlowMlPerMin;
        toast('Calibração aplicada: '+d.nominalFlowMlPerMin+' mL/min');
      }else{
        toast('Não foi possível calibrar: '+(d.error||'erro'),true);
      }
    }).catch(()=>toast('Sem conexão com o irrigador',true));
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
    ensureDefaultPassword();

    // Registra as rotas HTTP (seguro fazer antes de abrir o servidor)
    server.on("/",          HTTP_GET,  []{ if (_instance) _instance->handleRoot();   });
    server.on("/api/data",  HTTP_GET,  []{ if (_instance) _instance->handleData();   });
    server.on("/api/auth",  HTTP_POST, []{ if (_instance) _instance->handleAuth();   });
    server.on("/api/logout", HTTP_POST, []{ if (_instance) _instance->handleLogout(); });
    server.on("/api/config",HTTP_POST, []{ if (_instance) _instance->handleConfig(); });
    server.on("/api/flow-test/start", HTTP_POST, []{ if (_instance) _instance->handleFlowTestStart(); });
    server.on("/api/flow-calibration", HTTP_POST, []{ if (_instance) _instance->handleFlowCalibration(); });
    server.on("/api/schedules/reset", HTTP_POST, []{ if (_instance) _instance->handleResetSchedules(); });
    server.on("/api/password/change",  HTTP_POST, []{ if (_instance) _instance->handlePasswordChange(); });
    server.on("/api/password/recover", HTTP_POST, []{ if (_instance) _instance->handlePasswordRecover(); });
    server.on("/api/password/status",  HTTP_GET,  []{ if (_instance) _instance->handlePasswordStatus(); });

    // Apenas informa que as rotas estao prontas. A porta 80 sera aberta na _task apos o WiFi conectar.
    fwLogLine("INFO", "WEB", "Rotas HTTP configuradas. Aguardando Wi-Fi para abrir a porta 80...");

    // Task no Core 0 — junto com firebaseTask e mqttConnectTask.
    xTaskCreatePinnedToCore(_task, "webTask", 8192, this, 1, nullptr, 0); // aumentado: handlePasswordRecover usa WiFiClientSecure + HTTPClient
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
    bool emailNotificationEnabled = getEmailNotificationEnabled();

    char json[700];
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
        "\"emailNotificationEnabled\":%s,"
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
        emailNotificationEnabled ? "true" : "false",
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

  if (_authValid) {
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (_authAttemptsLeft <= 0) {
    server.send(200, "application/json", "{\"ok\":false,\"locked\":true,\"attemptsLeft\":0}");
    return;
  }

  String pass = server.arg("password");

  PasswordKind kind = PASS_DEFAULT_MAC;
  time_t tempExp    = 0;
  bool ok = verifyPassword(pass, &kind, &tempExp);

  if (ok) {
    // Senha temporária: verificar validade
    if (kind == PASS_TEMP) {
      time_t now = time(nullptr);
      if (now > 1000000000L && tempExp > 0 && now > tempExp) {
        server.send(200, "application/json", "{\"ok\":false,\"locked\":false,\"error\":\"Senha temporaria expirada. Solicite uma nova recuperacao.\",\"attemptsLeft\":3}");
        resetAuthState();
        return;
      }
      _requiresPasswordChange = true;
    } else {
      _requiresPasswordChange = false;
    }
    _authValid = true;
    char json[100];
    snprintf(json, sizeof(json), "{\"ok\":true,\"requiresPasswordChange\":%s,\"passKind\":%d}",
             _requiresPasswordChange ? "true" : "false", (int)kind);
    server.send(200, "application/json", json);
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
    bool emailNotifyEnabled = getEmailNotificationEnabled();

    if (server.hasArg("emailNotify")) {
      String emailNotifyArg = server.arg("emailNotify");
      emailNotifyArg.trim();
      emailNotifyArg.toLowerCase();
      emailNotifyEnabled = !(emailNotifyArg == "0" || emailNotifyArg == "false" || emailNotifyArg == "off");
    }

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

    setEmailNotificationEnabled(emailNotifyEnabled);

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

// ── Handlers de gerenciamento de senha ───────────────────────────────────────

void WebServerManager::handlePasswordStatus() {
  if (!authPrefs.begin(AUTH_NS, true)) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"nvs unavailable\"}");
    return;
  }
  PasswordKind kind  = (PasswordKind)authPrefs.getUChar(KEY_KIND, (uint8_t)PASS_DEFAULT_MAC);
  time_t tempExp     = (time_t)authPrefs.getULong(KEY_TEMP_EXP, 0);
  authPrefs.end();

  bool expired = false;
  if (kind == PASS_TEMP) {
    time_t now = time(nullptr);
    expired = (now > 1000000000L && tempExp > 0 && now > tempExp);
  }

  char json[120];
  snprintf(json, sizeof(json),
    "{\"ok\":true,\"passKind\":%d,\"requiresPasswordChange\":%s,\"temporaryExpired\":%s}",
    (int)kind,
    (_requiresPasswordChange && _authValid) ? "true" : "false",
    expired ? "true" : "false");
  server.send(200, "application/json", json);
}

void WebServerManager::handlePasswordChange() {
  if (!_authValid) {
    server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  if (!server.hasArg("newPassword")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametro newPassword ausente\"}");
    return;
  }

  String newPass = server.arg("newPassword");
  if (newPass.length() < 4) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Senha deve ter no minimo 4 caracteres\"}");
    return;
  }
  if (newPass.length() > 64) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Senha muito longa (max 64)\"}");
    return;
  }

  persistPassword(newPass, PASS_CUSTOM);
  _requiresPasswordChange = false;
  fwLogLine("INFO", "WEB", "Senha de manutencao alterada pelo usuario");
  server.send(200, "application/json", "{\"ok\":true}");
}

void WebServerManager::handlePasswordRecover() {
  if (!server.hasArg("email")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"parametro email ausente\"}");
    return;
  }

  String email = server.arg("email");
  email.trim();
  if (email.length() < 5 || email.indexOf('@') < 0) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"e-mail invalido\"}");
    return;
  }

  // Chama a Cloud Function de recuperação via HTTPS.
  // A CF valida o e-mail contra o owner do dispositivo, gera a senha
  // temporária, envia o e-mail e retorna o hash+salt+expiração para o firmware.
  String deviceId = getDeviceIdFromMac();
  String url      = String(FIREBASE_RECOVERY_URL);

  String body = "deviceId=" + deviceId + "&email=" + email;

  WiFiClientSecure httpsClient;
  httpsClient.setInsecure();
  httpsClient.setTimeout(15000);

  HTTPClient http;
  http.begin(httpsClient, url);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.setTimeout(15000);

  int httpCode = http.POST(body);
  String resp  = http.getString();
  http.end();

  if (httpCode != 200 && httpCode != 201) {
    fwLogf("WARN", "WEB", "CF recovery retornou HTTP %d", httpCode);
    // Retorna mensagem genérica para não expor detalhes internos ao browser
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Nao foi possivel processar a solicitacao. Verifique o e-mail informado.\"}");
    return;
  }

  // A CF retorna: {"ok":true,"hash":"…","salt":"…","expiresAt":1234567890}
  // Parseia manualmente sem ArduinoJson para nao adicionar dependencia nova.
  auto extractStr = [&](const String& key) -> String {
    String token = "\"" + key + "\":\"";
    int pos = resp.indexOf(token);
    if (pos < 0) return "";
    int start = pos + token.length();
    int end   = resp.indexOf('"', start);
    if (end < 0) return "";
    return resp.substring(start, end);
  };
  auto extractLong = [&](const String& key) -> long {
    String token = "\"" + key + "\":";
    int pos = resp.indexOf(token);
    if (pos < 0) return 0;
    return resp.substring(pos + token.length()).toInt();
  };

  String ok = extractStr("ok");
  // CF pode retornar ok como boolean (sem aspas), verifica ambos
  bool cfOk = (resp.indexOf("\"ok\":true") >= 0);

  if (!cfOk) {
    String cfErr = extractStr("error");
    if (cfErr.length() == 0) cfErr = "Operacao negada pelo servidor";
    server.send(200, "application/json", ("{\"ok\":false,\"error\":\"" + cfErr + "\"}").c_str());
    return;
  }

  String hash = extractStr("hash");
  String salt = extractStr("salt");
  long   exp  = extractLong("expiresAt");

  if (hash.length() == 0 || salt.length() == 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"Resposta invalida do servidor de recuperacao\"}");
    return;
  }

  // Persiste diretamente o hash+salt recebidos da CF (já calculados com a senha temporária).
  if (!authPrefs.begin(AUTH_NS, false)) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"nvs unavailable\"}");
    return;
  }
  authPrefs.putString(KEY_HASH, hash);
  authPrefs.putString(KEY_SALT, salt);
  authPrefs.putUChar(KEY_KIND, (uint8_t)PASS_TEMP);
  if (exp > 0) authPrefs.putULong(KEY_TEMP_EXP, (unsigned long)exp);
  authPrefs.end();

  fwLogLine("INFO", "WEB", "Senha temporaria recebida e persistida via CF recovery");
  server.send(200, "application/json", "{\"ok\":true}");
}