#include <winsock2.h>
#include <ws2tcpip.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "web/web_server.h"
#include "common/logger.h"

namespace pg {
namespace {

// ============================================================
// 小工具
// ============================================================

std::string lowerCopy(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    o += '"';
    return o;
}

std::string num2(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

std::string httpResponse(int code, const char* type, const std::string& body) {
    const char* reason = code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request";
    std::string r;
    r += "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    r += std::string("Content-Type: ") + type + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Cache-Control: no-store\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

std::string okJson() { return "{\"ok\":true}"; }

void sendAll(SOCKET c, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(c, data.data() + off, (int)(data.size() - off), 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

// 在扁平 JSON 里定位某个 key 的值起点
bool jsonValuePos(const std::string& body, const char* key, size_t& pos) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return false;
    p = body.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < body.size() && std::isspace((unsigned char)body[p])) ++p;
    pos = p;
    return true;
}

bool jsonString(const std::string& body, const char* key, std::string& out) {
    size_t p = 0;
    if (!jsonValuePos(body, key, p)) return false;
    if (p >= body.size() || body[p] != '"') return false;
    ++p;
    std::string s;
    while (p < body.size() && body[p] != '"') {
        char c = body[p++];
        if (c == '\\' && p < body.size()) {
            char e = body[p++];
            switch (e) {
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u':
                    // 进程名都是 ASCII, 只处理 < 0x80 的码点
                    if (p + 4 <= body.size()) {
                        int code = (int)std::strtol(body.substr(p, 4).c_str(), nullptr, 16);
                        p += 4;
                        if (code > 0 && code < 0x80) s += (char)code;
                    }
                    break;
                default: s += e;
            }
        } else {
            s += c;
        }
    }
    if (p >= body.size()) return false;
    out = s;
    return true;
}

bool jsonNumber(const std::string& body, const char* key, double& out) {
    size_t p = 0;
    if (!jsonValuePos(body, key, p)) return false;
    if (body.compare(p, 4, "true") == 0)  { out = 1; return true; }
    if (body.compare(p, 5, "false") == 0) { out = 0; return true; }
    char* end = nullptr;
    double v = std::strtod(body.c_str() + p, &end);
    if (end == body.c_str() + p) return false;
    out = v;
    return true;
}

std::string statusJson(const WebStatus& s) {
    std::string o = "{";
    o += "\"cpu_w\":" + num2(s.cpuW);
    o += ",\"gpu_w\":" + num2(s.gpuW);
    o += ",\"battery_w\":" + num2(s.batteryW);
    o += ",\"battery_pct\":" + std::to_string(s.batteryPct);
    o += ",\"ac_online\":" + std::to_string(s.acOnline);
    o += ",\"discharging\":" + std::to_string(s.discharging);
    o += ",\"charging\":" + std::to_string(s.charging);
    o += ",\"battery_valid\":" + std::string(s.batteryValid ? "true" : "false");

    o += ",\"profile_enabled\":" + std::string(s.profileEnabled ? "true" : "false");
    o += ",\"dry_run\":" + std::string(s.dryRun ? "true" : "false");
    o += ",\"apply_always\":" + std::string(s.applyAlways ? "true" : "false");
    o += ",\"control_applied\":" + std::string(s.controlApplied ? "true" : "false");
    o += ",\"paused\":" + std::string(s.paused ? "true" : "false");
    o += ",\"gpu_control_ok\":" + std::string(s.gpuControlOk ? "true" : "false");
    o += ",\"cpu_control_ok\":" + std::string(s.cpuControlOk ? "true" : "false");
    o += ",\"used_gpu_clock\":" + std::string(s.usedGpuClock ? "true" : "false");
    o += ",\"gpu_limit_w\":" + std::to_string(s.gpuLimitW);
    o += ",\"cpu_max_pct\":" + std::to_string(s.cpuMaxPct);
    o += ",\"gpu_clock_mhz\":" + std::to_string(s.gpuClockMhz);
    o += ",\"applied_gpu_w\":" + std::to_string(s.appliedGpuW);
    o += ",\"applied_cpu_pct\":" + std::to_string(s.appliedCpuPct);
    o += ",\"applied_gpu_clock_mhz\":" + std::to_string(s.appliedGpuClockMhz);

    o += ",\"target\":" + jsonEscape(s.target);
    o += ",\"game_profile\":" + jsonEscape(s.gameProfile);
    o += ",\"game_running\":" + std::string(s.gameRunning ? "true" : "false");
    o += ",\"game_pid\":" + std::to_string((unsigned long)s.gamePid);
    o += ",\"web_port\":" + std::to_string(s.webPort);
    o += ",\"uptime_sec\":" + std::to_string(s.uptimeSec);

    o += ",\"cfg\":{";
    o += "\"gpu_limit_w\":" + std::to_string(s.cfgGpuLimitW);
    o += ",\"cpu_max_pct\":" + std::to_string(s.cfgCpuMaxPct);
    o += ",\"gpu_clock_target_mhz\":" + std::to_string(s.gpuClockTargetMhz);
    o += ",\"min_gpu_w\":" + std::to_string(s.minGpuW);
    o += ",\"max_gpu_w\":" + std::to_string(s.maxGpuW);
    o += ",\"step_w\":" + std::to_string(s.stepW);
    o += ",\"hold_down_sec\":" + std::to_string(s.holdDownSec);
    o += ",\"hold_up_sec\":" + std::to_string(s.holdUpSec);
    o += ",\"cpu_min_pct\":" + std::to_string(s.cpuMinPct);
    o += ",\"min_gpu_clock_mhz\":" + std::to_string(s.minGpuClockMhz);
    o += ",\"clock_step_mhz\":" + std::to_string(s.clockStepMhz);
    o += ",\"auto_clock_mhz\":" + std::to_string(s.autoClockMhz);
    o += ",\"battery_threshold_w\":" + num2(s.batteryThresholdW);
    o += ",\"gpu_clock_auto\":" + std::string(s.gpuClockAuto ? "true" : "false");
    o += ",\"cpu_step_pct\":" + std::to_string(s.cpuStepPct);
    o += ",\"cpu_power_step\":" + std::string(s.cpuPowerStep ? "true" : "false");
    o += ",\"prefer_cpu\":" + std::string(s.preferCpu ? "true" : "false");
    o += ",\"deficit_energy_wh\":" + num2(s.deficitEnergyWh);
    o += ",\"deficit_window_sec\":" + std::to_string(s.deficitWindowSec);
    o += ",\"step_cooldown_sec\":" + std::to_string(s.stepCooldownSec);
    o += "}}";
    return o;
}

std::string processesJson(const std::vector<ProcInfo>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ',';
        o += "{\"name\":" + jsonEscape(v[i].name) +
             ",\"pid\":" + std::to_string((unsigned long)v[i].pid) + "}";
    }
    o += ']';
    return o;
}

// ============================================================
// 内嵌页面
// ============================================================

const char kHtml[] = R"HTML(
<!doctype html>
<html lang="zh-CN" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>PowerGuard 控制台</title>
<script>
/* 在样式生效前确定主题, 避免刷新时闪一下另一种配色 */
(function(){
  try{
    var m = localStorage.getItem('pg-theme') || 'auto';
    var d = m === 'dark' || (m === 'auto' && window.matchMedia('(prefers-color-scheme: dark)').matches);
    document.documentElement.setAttribute('data-theme', d ? 'dark' : 'light');
  }catch(e){}
})();
</script>
<style>
:root{
  --bg:#000;--island:rgba(28,28,30,.72);--panel:rgba(28,28,30,.62);
  --field:rgba(118,118,128,.18);--solid:#1c1c1e;--line:rgba(255,255,255,.10);
  --txt:#f5f5f7;--dim:#98989f;--hover:rgba(255,255,255,.06);
  --accent:#0a84ff;--ok:#30d158;--warn:#ff9f0a;--bad:#ff453a;
  --ring:0 0 0 3px rgba(10,132,255,.25);
  --shadow:0 18px 50px rgba(0,0,0,.55);--shadow-soft:0 8px 24px rgba(0,0,0,.35);
  --track:rgba(120,120,128,.34);--accent-soft:rgba(10,132,255,.14);
  --warn-soft:rgba(255,159,10,.14);--warn-fill:#ff9f0a;
  --bg-img:radial-gradient(1100px 560px at 15% -12%,rgba(10,132,255,.18),transparent 62%),
           radial-gradient(900px 520px at 105% 10%,rgba(94,92,230,.14),transparent 60%);
}
html[data-theme="light"]{
  --bg:#f2f2f7;--island:rgba(255,255,255,.72);--panel:rgba(255,255,255,.66);
  --field:rgba(118,118,128,.12);--solid:#fff;--line:rgba(60,60,67,.14);
  --txt:#1c1c1e;--dim:#6e6e73;--hover:rgba(0,0,0,.045);
  --accent:#007aff;--ok:#248a3d;--warn:#b25000;--bad:#d70015;
  --ring:0 0 0 3px rgba(0,122,255,.20);
  --shadow:0 18px 50px rgba(31,35,41,.16);--shadow-soft:0 8px 24px rgba(31,35,41,.10);
  --track:rgba(120,120,128,.24);--accent-soft:rgba(0,122,255,.10);
  --warn-soft:rgba(255,159,10,.16);--warn-fill:#ff9f0a;
  --bg-img:radial-gradient(1100px 560px at 15% -12%,rgba(10,132,255,.16),transparent 62%),
           radial-gradient(900px 520px at 105% 10%,rgba(175,82,222,.12),transparent 60%);
}
html{color-scheme:dark;background:var(--bg)}
html[data-theme="light"]{color-scheme:light}
*{box-sizing:border-box}
body{
  margin:0;min-height:100vh;background:var(--bg);background-image:var(--bg-img);
  background-attachment:fixed;color:var(--txt);
  font:14px/1.55 -apple-system,BlinkMacSystemFont,"SF Pro Text","Segoe UI",system-ui,
       "PingFang SC","Microsoft YaHei",sans-serif;
  -webkit-font-smoothing:antialiased;transition:background-color .3s,color .3s;
}

/* ---- 悬浮胶囊顶栏 ---- */
header{
  position:sticky;top:10px;z-index:20;max-width:1060px;margin:10px auto 0;
  padding:8px 10px 8px 16px;display:flex;align-items:center;gap:10px;flex-wrap:wrap;
  background:var(--island);border:1px solid var(--line);border-radius:28px;
  backdrop-filter:blur(28px) saturate(180%);-webkit-backdrop-filter:blur(28px) saturate(180%);
  box-shadow:var(--shadow);transition:background .3s,border-color .3s;
}
.brand{display:flex;align-items:center;gap:9px}
.logo{
  width:28px;height:28px;border-radius:9px;display:grid;place-items:center;flex:none;
  font-size:11px;font-weight:700;letter-spacing:.02em;color:#fff;
  background:linear-gradient(135deg,#0a84ff,#5e5ce6);box-shadow:0 4px 12px rgba(10,132,255,.35);
}
h1{font-size:15px;font-weight:600;margin:0;letter-spacing:-.01em}
.badges{display:flex;gap:6px;flex-wrap:wrap}
.spacer{flex:1}
.badge{
  display:inline-flex;align-items:center;gap:6px;font-size:12px;padding:5px 11px;
  border-radius:999px;background:var(--field);color:var(--dim);white-space:nowrap;
  transition:background .25s,color .25s;
}
.badge::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor;opacity:.85}
.badge.on{background:rgba(48,209,88,.16);color:var(--ok)}
.badge.warn{background:rgba(255,159,10,.16);color:var(--warn)}
.badge.bad{background:rgba(255,69,58,.15);color:var(--bad)}

/* ---- 主题切换按钮 ---- */
button{
  font:inherit;font-size:13px;font-weight:500;color:var(--txt);background:var(--field);
  border:1px solid var(--line);border-radius:999px;padding:9px 18px;cursor:pointer;
  transition:transform .12s ease,background .2s,border-color .2s,box-shadow .2s;white-space:nowrap;
}
button:hover{border-color:var(--accent)}
button:active{transform:scale(.96)}
button.primary{
  background:var(--accent);border-color:transparent;color:#fff;font-weight:600;
  box-shadow:0 8px 22px rgba(10,132,255,.30);
}
button.danger{color:var(--bad);border-color:rgba(255,69,58,.35);background:rgba(255,69,58,.10)}
.icon-btn{width:38px;height:38px;padding:0;border-radius:50%;display:grid;place-items:center;flex:none}
.icon-btn svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
html[data-theme="dark"] #ico-sun{display:none}
html[data-theme="light"] #ico-moon{display:none}

/* ---- 指标卡片 ---- */
main{max-width:1060px;margin:0 auto;padding:18px 20px 8px}
.hero{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}
.metric{
  background:var(--panel);border:1px solid var(--line);border-radius:28px;padding:16px 18px;
  backdrop-filter:blur(22px);-webkit-backdrop-filter:blur(22px);box-shadow:var(--shadow-soft);
  transition:background .3s,border-color .3s;
}
.k{font-size:11px;font-weight:500;letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}
.big{font-size:32px;font-weight:600;letter-spacing:-.02em;line-height:1.2;margin-top:4px;font-variant-numeric:tabular-nums}
.big.text{font-size:22px}
.sub{font-size:12px;color:var(--dim);margin-top:3px}

/* ---- 面板 ---- */
.panel{
  background:var(--panel);border:1px solid var(--line);border-radius:30px;
  padding:20px 22px;margin-top:14px;backdrop-filter:blur(22px);-webkit-backdrop-filter:blur(22px);
  box-shadow:var(--shadow-soft);transition:background .3s,border-color .3s;
}
.panel-head{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:14px}
.panel-head h3{font-size:15px;font-weight:600;letter-spacing:-.01em;margin:0}
.hint{font-size:12px;color:var(--dim)}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}
.stat{background:var(--field);border-radius:22px;padding:14px 16px;transition:background .3s}
.stat .v{font-size:26px;font-weight:600;letter-spacing:-.01em;line-height:1.25;margin-top:2px;font-variant-numeric:tabular-nums}
.stat .v.text{font-size:16px;line-height:1.4;word-break:break-all}

/* ---- 参数 ---- */
.group{margin-top:16px}
.group:first-of-type{margin-top:0}
.group h4{
  font-size:11px;font-weight:600;letter-spacing:.1em;text-transform:uppercase;
  color:var(--dim);margin:0 0 8px;padding-left:6px;
}
.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}
.field{
  background:var(--field);border:1px solid transparent;border-radius:18px;padding:8px 14px 9px;
  transition:border-color .2s,box-shadow .2s,background .3s;
}
.field:focus-within{border-color:var(--accent);box-shadow:var(--ring)}
.field label{display:block;font-size:11px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.field input,.field select{
  width:100%;border:0;background:transparent;color:var(--txt);font:inherit;font-size:15px;
  font-weight:500;padding:2px 0;outline:none;font-variant-numeric:tabular-nums;
}
.field select{
  appearance:none;-webkit-appearance:none;cursor:pointer;padding-right:18px;
  background:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='10' height='6' viewBox='0 0 10 6'><path d='M1 1l4 4 4-4' fill='none' stroke='%2398989f' stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round'/></svg>") right center no-repeat;
}
.field select option{background:var(--solid);color:var(--txt)}

/* ---- 模式切换卡片: 整张卡片可点, 点一次换一种状态 ---- */
.toggle{
  display:flex;align-items:center;justify-content:space-between;gap:12px;
  cursor:pointer;user-select:none;-webkit-user-select:none;padding-top:10px;padding-bottom:10px;
}
.toggle:hover{border-color:rgba(128,128,128,.4)}
.toggle:focus-visible{outline:none;border-color:var(--accent);box-shadow:var(--ring)}
.toggle .tval{font-size:15px;font-weight:500;margin-top:2px;transition:color .2s}
.toggle.on{background:var(--accent-soft);border-color:rgba(10,132,255,.28)}
.toggle.on .tval{color:var(--accent)}
.toggle.warn.on{background:var(--warn-soft);border-color:rgba(255,159,10,.32)}
.toggle.warn.on .tval{color:var(--warn)}
.sw{
  position:relative;flex:none;width:46px;height:28px;border-radius:999px;
  background:var(--track);transition:background .25s,border-color .25s;
}
.sw::after{
  content:"";position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;
  background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.32);transition:transform .25s cubic-bezier(.2,.8,.2,1);
}
.toggle.on .sw{background:var(--accent)}
.toggle.warn.on .sw{background:var(--warn-fill)}
.toggle.on .sw::after{transform:translateX(18px)}

.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}

/* ---- 进程 ---- */
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:flex-end}
.row .field.grow{flex:1 1 260px}
.row2{display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap}
.search{
  flex:1 1 220px;background:var(--field);border:1px solid var(--line);border-radius:999px;
  padding:9px 16px;color:var(--txt);font:inherit;outline:none;
  transition:border-color .2s,box-shadow .2s;
}
.search:focus{border-color:var(--accent);box-shadow:var(--ring)}
.scroll{
  margin-top:10px;max-height:320px;overflow:auto;border:1px solid var(--line);
  border-radius:22px;background:var(--field);
}
.scroll::-webkit-scrollbar{width:10px;height:10px}
.scroll::-webkit-scrollbar-thumb{background:var(--line);border-radius:999px;border:3px solid transparent;background-clip:content-box}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{text-align:left;padding:9px 14px;border-bottom:1px solid var(--line)}
tr:last-child td{border-bottom:0}
th{
  font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--dim);font-weight:600;
  position:sticky;top:0;background:var(--solid);z-index:1;
}
tbody tr:hover{background:var(--hover)}
td button{padding:5px 12px;font-size:12px}
td.num{font-variant-numeric:tabular-nums;color:var(--dim)}

footer{max-width:1060px;margin:0 auto;padding:14px 20px 30px;color:var(--dim);font-size:12px;text-align:center}

/* ---- 提示气泡 ---- */
#msg{
  position:fixed;left:50%;bottom:26px;z-index:50;pointer-events:none;
  transform:translate(-50%,16px);opacity:0;transition:opacity .2s,transform .25s;
  background:var(--solid);border:1px solid var(--line);border-radius:999px;
  padding:10px 20px;font-size:13px;box-shadow:var(--shadow);white-space:nowrap;max-width:90vw;
  overflow:hidden;text-overflow:ellipsis;
}
#msg.show{opacity:1;transform:translate(-50%,0)}

@media (prefers-reduced-motion:reduce){*{transition:none!important;animation:none!important}}
</style>
</head>
<body>
<header>
  <div class="brand"><span class="logo">PG</span><h1>PowerGuard</h1></div>
  <div class="badges">
    <span id="b-state" class="badge">连接中…</span>
    <span id="b-game" class="badge">目标未运行</span>
    <span id="b-dry" class="badge warn" style="display:none">DRY-RUN</span>
    <span id="b-paused" class="badge warn" style="display:none">已暂停</span>
  </div>
  <span class="spacer"></span>
  <button id="theme-btn" class="icon-btn" onclick="toggleTheme()" title="切换主题" aria-label="切换主题">
    <svg id="ico-moon" viewBox="0 0 24 24" aria-hidden="true"><path d="M20.5 14.2A8.5 8.5 0 1 1 9.8 3.5a6.8 6.8 0 0 0 10.7 10.7Z"/></svg>
    <svg id="ico-sun" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M18.7 5.3l-1.4 1.4M6.7 17.3l-1.4 1.4"/></svg>
  </button>
</header>

<main>
  <div class="hero">
    <div class="metric"><div class="k">CPU 功率</div><div class="big" id="v-cpu">--</div><div class="sub">RAPL Energy Meter</div></div>
    <div class="metric"><div class="k">GPU 功率</div><div class="big" id="v-gpu">--</div><div class="sub" id="s-gpu">NVML</div></div>
    <div class="metric"><div class="k">电池净功率</div><div class="big" id="v-bat">--</div><div class="sub" id="s-bat">--</div></div>
    <div class="metric"><div class="k">供电</div><div class="big text" id="v-ac">--</div><div class="sub" id="s-ac">--</div></div>
  </div>

  <section class="panel">
    <div class="panel-head"><h3>控制状态</h3><span class="hint">每秒刷新</span></div>
    <div class="stats">
      <div class="stat"><div class="k">CPU 最大状态</div><div class="v" id="v-cpumax">--</div><div class="sub" id="s-cpumax">--</div></div>
      <div class="stat"><div class="k">GPU 上限 / 锁频</div><div class="v" id="v-gpulim">--</div><div class="sub" id="s-gpulim">--</div></div>
      <div class="stat"><div class="k">监控目标</div><div class="v text" id="v-target">--</div><div class="sub" id="s-target">--</div></div>
    </div>
  </section>

  <section class="panel">
    <div class="panel-head"><h3>参数</h3><span class="hint">保存后会立即重新应用</span></div>

    <div class="group"><h4>GPU</h4><div class="fields">
      <div class="field"><label for="f-gpu-limit">功耗上限 (W)</label><input id="f-gpu-limit" type="number" min="5" max="250"></div>
      <div class="field"><label for="f-max-gpu">回升上限 (W)</label><input id="f-max-gpu" type="number" min="5" max="250"></div>
      <div class="field"><label for="f-min-gpu">降档下限 (W)</label><input id="f-min-gpu" type="number" min="5" max="250"></div>
      <div class="field"><label for="f-gpu-clock">锁频目标 (MHz, 0=关)</label><input id="f-gpu-clock" type="number" min="0" max="4000" step="50"></div>
      <div class="field"><label for="f-min-clock">锁频下限 (MHz)</label><input id="f-min-clock" type="number" min="100" step="50"></div>
      <div class="field"><label for="f-clock-step">锁频步进 (MHz)</label><input id="f-clock-step" type="number" min="10" step="10"></div>
      <div class="field"><label for="f-auto-clock">自动锁频起始 (MHz)</label><input id="f-auto-clock" type="number" min="100" step="50"></div>
    </div></div>

    <div class="group"><h4>CPU</h4><div class="fields">
      <div class="field"><label for="f-cpu-max">最大状态 (%)</label><input id="f-cpu-max" type="number" min="5" max="100"></div>
      <div class="field"><label for="f-cpu-min">降档下限 (%)</label><input id="f-cpu-min" type="number" min="5" max="100"></div>
      <div class="field"><label for="f-cpu-step">降档步进 (%)</label><input id="f-cpu-step" type="number" min="1" max="50"></div>
      <div class="field toggle" id="t-cpustep" role="switch" aria-checked="true" tabindex="0"
           onclick="flip('f-cpu-power-step')"
           onkeydown="if(event.key===' '||event.key==='Enter'){event.preventDefault();flip('f-cpu-power-step');}">
        <div class="tinfo"><label>CPU 参与降档</label><div class="tval" id="tv-cpustep">参与</div></div>
        <span class="sw" aria-hidden="true"></span>
      </div>
      <input type="hidden" id="f-cpu-power-step" value="1">
      <div class="field toggle" id="t-prefer" role="switch" aria-checked="false" tabindex="0"
           onclick="flip('f-prefer-cpu')"
           onkeydown="if(event.key===' '||event.key==='Enter'){event.preventDefault();flip('f-prefer-cpu');}">
        <div class="tinfo"><label>降档顺序</label><div class="tval" id="tv-prefer">GPU 优先</div></div>
        <span class="sw" aria-hidden="true"></span>
      </div>
      <input type="hidden" id="f-prefer-cpu" value="0">
    </div></div>

    <div class="group"><h4>反馈与升降档</h4><div class="fields">
      <div class="field"><label for="f-bat">电池阈值 (W)</label><input id="f-bat" type="number" min="0" step="0.5"></div>
      <div class="field"><label for="f-energy">缺口能量阈值 (Wh)</label><input id="f-energy" type="number" min="0" step="0.05"></div>
      <div class="field"><label for="f-cooldown">降档冷却 (秒)</label><input id="f-cooldown" type="number" min="0"></div>
      <div class="field"><label for="f-step">降档步进 (W)</label><input id="f-step" type="number" min="1" max="50"></div>
      <div class="field"><label for="f-down">降档等待 (秒)</label><input id="f-down" type="number" min="1"></div>
      <div class="field"><label for="f-up">回升等待 (秒)</label><input id="f-up" type="number" min="1"></div>
    </div></div>

    <div class="group"><h4>模式</h4><div class="fields">
      <div class="field toggle warn" id="t-dry" role="switch" aria-checked="false" tabindex="0"
           onclick="flip('f-dry')"
           onkeydown="if(event.key===' '||event.key==='Enter'){event.preventDefault();flip('f-dry');}">
        <div class="tinfo"><label>演练模式</label><div class="tval" id="tv-dry">关闭</div></div>
        <span class="sw" aria-hidden="true"></span>
      </div>
      <div class="field toggle" id="t-always" role="switch" aria-checked="false" tabindex="0"
           onclick="flip('f-always')"
           onkeydown="if(event.key===' '||event.key==='Enter'){event.preventDefault();flip('f-always');}">
        <div class="tinfo"><label>生效方式</label><div class="tval" id="tv-always">仅游戏运行时</div></div>
        <span class="sw" aria-hidden="true"></span>
      </div>
      <input type="hidden" id="f-dry" value="0">
      <input type="hidden" id="f-always" value="0">
    </div></div>

    <div class="actions">
      <button class="primary" onclick="saveCfg()">保存并应用</button>
      <button onclick="post('/api/apply')">立即应用</button>
      <button class="danger" onclick="post('/api/restore')">恢复原始设置</button>
    </div>
  </section>

  <section class="panel">
    <div class="panel-head"><h3>进程</h3><span class="hint">点“设为目标”即可切换监控对象</span></div>
    <div class="row">
      <div class="field grow"><label for="f-target">监控目标（可手填，自动补 .exe）</label><input id="f-target" type="text" placeholder="Starfield.exe"></div>
      <button onclick="saveTarget()">设为目标</button>
      <button onclick="clearTarget()">清除</button>
    </div>
    <div class="row2">
      <input id="proc-filter" class="search" type="text" placeholder="搜索进程…" oninput="renderProcs()">
      <button onclick="loadProcs()">刷新列表</button>
    </div>
    <div class="scroll">
      <table><thead><tr><th>进程</th><th>PID</th><th></th></tr></thead><tbody id="proc-body"></tbody></table>
    </div>
  </section>
</main>
<footer id="foot">--</footer>
<div id="msg" role="status" aria-live="polite"></div>

<script>
var procs = [];
var cfgSig = '';
var inflight = false;

function $(id){ return document.getElementById(id); }
function setText(id, t){ var e = $(id); if (e) e.textContent = t; }
function isEditing(){
  var a = document.activeElement;
  return a && (a.tagName === 'INPUT' || a.tagName === 'SELECT');
}

/* ---- 主题: 深色 / 浅色, 首次跟随系统, 选择后记住 ---- */
var mq = window.matchMedia('(prefers-color-scheme: dark)');
var themeMode = 'auto';
try { themeMode = localStorage.getItem('pg-theme') || 'auto'; } catch(e) {}
/* 支持 ?theme=dark|light 临时预览, 不影响已记住的选择 */
try {
  var qp = new URLSearchParams(location.search).get('theme');
  if (qp === 'dark' || qp === 'light') themeMode = qp;
} catch(e) {}

function themeIsDark(){
  return themeMode === 'dark' || (themeMode === 'auto' && mq.matches);
}
function applyTheme(){
  var dark = themeIsDark();
  document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
  var b = $('theme-btn');
  if (b) b.title = dark ? '当前：深色 · 点击切换到浅色' : '当前：浅色 · 点击切换到深色';
}
function toggleTheme(){
  themeMode = themeIsDark() ? 'light' : 'dark';
  try { localStorage.setItem('pg-theme', themeMode); } catch(e) {}
  applyTheme();
}
if (mq.addEventListener) mq.addEventListener('change', function(){ if (themeMode === 'auto') applyTheme(); });
else if (mq.addListener) mq.addListener(function(){ if (themeMode === 'auto') applyTheme(); });
applyTheme();

/* ---- 模式卡片: 隐藏的 f-dry / f-always 仍和表单保持一致 ---- */
function setToggle(cardId, valId, on, onText, offText){
  var c = $(cardId), v = $(valId);
  if (!c || !v) return;
  c.classList.toggle('on', on);
  c.setAttribute('aria-checked', on ? 'true' : 'false');
  v.textContent = on ? onText : offText;
}
function syncToggles(){
  setToggle('t-dry', 'tv-dry', $('f-dry').value === '1', '开启', '关闭');
  setToggle('t-always', 'tv-always', $('f-always').value === '1', '启动即生效', '仅游戏运行时');
  setToggle('t-cpustep', 'tv-cpustep', $('f-cpu-power-step').value === '1', '参与', '只压 GPU');
  setToggle('t-prefer', 'tv-prefer', $('f-prefer-cpu').value === '1', 'CPU 优先', 'GPU 优先');
}
function flip(id){
  var el = $(id);
  el.value = el.value === '1' ? '0' : '1';
  syncToggles();
}

function msg(t){
  var e = $('msg');
  if (!e) return;
  e.textContent = t;
  e.classList.add('show');
  clearTimeout(window._msgTimer);
  window._msgTimer = setTimeout(function(){ e.classList.remove('show'); }, 3000);
}
function fmt(v, d, unit){
  return (typeof v === 'number' && v >= 0) ? v.toFixed(d) + ' ' + unit : '--';
}

function tick(){
  if (inflight) return;
  inflight = true;
  fetch('/api/status', {cache: 'no-store'})
    .then(function(r){ if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(render)
    .catch(function(){
      var b = $('b-state');
      b.className = 'badge bad';
      b.textContent = '连接断开';
    })
    .then(function(){ inflight = false; });
}

function render(s){
  var b = $('b-state');
  b.className = 'badge ' + (s.control_applied ? 'on' : '');
  b.textContent = s.control_applied ? '控制已应用' : '未应用';

  var bg = $('b-game');
  bg.className = 'badge ' + (s.game_running ? 'on' : '');
  bg.textContent = s.game_running ? ('目标运行中 PID ' + s.game_pid) : '目标未运行';

  $('b-dry').style.display = s.dry_run ? '' : 'none';
  $('b-paused').style.display = s.paused ? '' : 'none';

  setText('v-cpu', fmt(s.cpu_w, 1, 'W'));
  setText('v-gpu', fmt(s.gpu_w, 1, 'W'));
  setText('s-gpu', s.used_gpu_clock ? '锁频模式' : 'NVML');

  if (!s.battery_valid) {
    setText('v-bat', '--');
    setText('s-bat', '电池信息不可用');
    setText('v-ac', '--');
    setText('s-ac', '--');
  } else {
    var w = s.battery_w;
    setText('v-bat', (w >= 0 ? '+' : '') + w.toFixed(1) + ' W');
    setText('s-bat', (s.discharging ? '放电（供电缺口）'
              : s.charging ? '充电（供电有余量）' : '待机') + ' · ' + s.battery_pct + '%');
    $('v-bat').style.color = (s.ac_online === 1 && s.discharging && w > 2) ? 'var(--bad)' : '';
    setText('v-ac', s.ac_online === 1 ? '已插电' : '电池供电');
    setText('s-ac', s.ac_online === 1 ? '外部供电中' : '未接电源');
  }

  setText('v-cpumax', s.cpu_max_pct >= 0 ? s.cpu_max_pct + '%' : '--');
  setText('s-cpumax', s.cpu_control_ok ? '已由本程序控制' : '未控制（显示当前系统值）');

  if (s.used_gpu_clock) {
    setText('v-gpulim', s.gpu_clock_mhz + ' MHz');
    setText('s-gpulim', '锁频模式 · 锁定核心频率');
  } else {
    setText('v-gpulim', s.gpu_limit_w >= 0 ? s.gpu_limit_w + ' W' : '--');
    setText('s-gpulim', s.gpu_control_ok ? '功耗上限已由本程序设置' : '未控制（显示当前生效值）');
  }

  setText('v-target', s.target || '(未设置)');
  setText('s-target', (s.apply_always ? '启动即生效' :
          (s.target ? '仅目标运行时生效' : '未设置则任何进程都不触发')) +
          (s.game_profile ? ' · 方案 ' + s.game_profile : ''));

  var cfg = s.cfg;
  var sig = JSON.stringify(cfg) + (s.dry_run ? '1' : '0') + (s.apply_always ? '1' : '0');
  if (sig !== cfgSig && !isEditing()) {
    cfgSig = sig;
    $('f-gpu-limit').value = cfg.gpu_limit_w;
    $('f-max-gpu').value = cfg.max_gpu_w;
    $('f-min-gpu').value = cfg.min_gpu_w;
    $('f-gpu-clock').value = cfg.gpu_clock_target_mhz;
    $('f-cpu-max').value = cfg.cpu_max_pct;
    $('f-cpu-min').value = cfg.cpu_min_pct;
    $('f-cpu-step').value = cfg.cpu_step_pct;
    $('f-bat').value = cfg.battery_threshold_w;
    $('f-energy').value = cfg.deficit_energy_wh;
    $('f-cooldown').value = cfg.step_cooldown_sec;
    $('f-step').value = cfg.step_w;
    $('f-down').value = cfg.hold_down_sec;
    $('f-up').value = cfg.hold_up_sec;
    $('f-min-clock').value = cfg.min_gpu_clock_mhz;
    $('f-clock-step').value = cfg.clock_step_mhz;
    $('f-auto-clock').value = cfg.auto_clock_mhz;
    $('f-dry').value = s.dry_run ? '1' : '0';
    $('f-always').value = s.apply_always ? '1' : '0';
    $('f-cpu-power-step').value = cfg.cpu_power_step ? '1' : '0';
    $('f-prefer-cpu').value = cfg.prefer_cpu ? '1' : '0';
    $('f-target').value = s.target;
    syncToggles();
  }

  var mins = Math.floor(s.uptime_sec / 60);
  setText('foot', '端口 ' + s.web_port + ' · 已运行 ' + mins + ' 分钟 · 仅本机可访问');
}

function post(url, body){
  return fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body || {})
  }).then(function(r){ return r.json(); });
}

function saveCfg(){
  var body = {
    gpu_limit_w: +$('f-gpu-limit').value,
    max_gpu_w: +$('f-max-gpu').value,
    min_gpu_w: +$('f-min-gpu').value,
    gpu_clock_mhz: +$('f-gpu-clock').value,
    cpu_max_pct: +$('f-cpu-max').value,
    cpu_min_pct: +$('f-cpu-min').value,
    cpu_step_pct: +$('f-cpu-step').value,
    cpu_power_step: +$('f-cpu-power-step').value,
    prefer_cpu: +$('f-prefer-cpu').value,
    battery_threshold_w: +$('f-bat').value,
    deficit_energy_wh: +$('f-energy').value,
    step_cooldown_sec: +$('f-cooldown').value,
    step_w: +$('f-step').value,
    hold_down_sec: +$('f-down').value,
    hold_up_sec: +$('f-up').value,
    min_gpu_clock_mhz: +$('f-min-clock').value,
    clock_step_mhz: +$('f-clock-step').value,
    auto_clock_mhz: +$('f-auto-clock').value,
    dry_run: +$('f-dry').value,
    apply_always: +$('f-always').value
  };
  post('/api/config', body).then(function(){
    msg('参数已保存；控制中会立即重新应用');
    cfgSig = '';
  });
}

function saveTarget(){
  var name = $('f-target').value.trim();
  post('/api/target', {name: name}).then(function(){ msg('监控目标已更新'); });
}

function clearTarget(){
  $('f-target').value = '';
  post('/api/target', {name: ''}).then(function(){ msg('已清除监控目标'); });
}

function loadProcs(quiet){
  fetch('/api/processes', {cache: 'no-store'})
    .then(function(r){ return r.json(); })
    .then(function(v){ procs = v; renderProcs(); if (!quiet) msg('进程列表已刷新'); })
    .catch(function(){ if (!quiet) msg('获取进程列表失败'); });
}

function renderProcs(){
  var q = ($('proc-filter').value || '').toLowerCase();
  var rows = '';
  var shown = 0;
  for (var i = 0; i < procs.length && shown < 400; i++) {
    var p = procs[i];
    if (q && p.name.toLowerCase().indexOf(q) < 0) continue;
    shown++;
    rows += '<tr><td>' + p.name.replace(/</g, '&lt;') + '</td><td class="num">' + p.pid +
            '</td><td style="text-align:right"><button onclick="pick(' + i + ')">设为目标</button></td></tr>';
  }
  $('proc-body').innerHTML = rows || '<tr><td colspan="3" style="color:var(--dim)">没有匹配的进程</td></tr>';
}

function pick(i){
  var p = procs[i];
  $('f-target').value = p.name;
  post('/api/target', {name: p.name}).then(function(){
    msg('监控目标已设为 ' + p.name);
  });
}

syncToggles();
loadProcs(true);        /* 打开页面就先填好进程列表 */
tick();
setInterval(tick, 1000);
</script>
</body>
</html>
)HTML";

} // namespace

// ============================================================
// WebServer
// ============================================================

WebServer::WebServer(int port, StatusFn status, ProcessFn processes, CommandFn command)
    : port_(port), status_(std::move(status)),
      processes_(std::move(processes)), command_(std::move(command)) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    if (running_) return true;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("web", "WSAStartup 失败, 无法启动 Web 控制台");
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        LOG_ERROR("web", "创建监听 socket 失败, code=%d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               (const char*)&exclusive, sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port_);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只监听本机

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("web", "Web 端口 %d 绑定失败, code=%d (端口被占用?)",
                  port_, WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return false;
    }
    if (listen(s, 8) != 0) {
        LOG_ERROR("web", "Web 端口 %d 监听失败, code=%d", port_, WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return false;
    }

    listenSock_ = (unsigned long long)s;
    running_ = true;
    thread_ = CreateThread(nullptr, 0,
                           [](LPVOID p) -> DWORD { ((WebServer*)p)->run(); return 0; },
                           this, 0, nullptr);
    if (!thread_) {
        LOG_ERROR("web", "创建 Web 线程失败, code=%lu", (unsigned long)GetLastError());
        running_ = false;
        closesocket(s);
        WSACleanup();
        listenSock_ = ~0ull;
        return false;
    }
    return true;
}

void WebServer::stop() {
    if (!running_ && thread_ == nullptr) return;
    running_ = false;
    if (thread_) {
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
}

void WebServer::run() {
    SOCKET s = (SOCKET)listenSock_;

    while (running_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{0, 500000};                       // 500ms 超时, 便于退出
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        sockaddr_in ca{};
        int len = sizeof(ca);
        SOCKET c = accept(s, (sockaddr*)&ca, &len);
        if (c == INVALID_SOCKET) continue;
        handleClient(c);
        closesocket(c);
    }

    closesocket(s);
    WSACleanup();
}

void WebServer::handleClient(SOCKET c) {
    DWORD timeout = 3000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    std::string req;
    char buf[4096];
    size_t headerEnd = std::string::npos;

    while (req.size() < 65536) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
    }
    if (headerEnd == std::string::npos) return;

    std::string head = req.substr(0, headerEnd);

    // 请求行: METHOD SP PATH SP VERSION
    size_t sp1 = head.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                            : head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        std::string resp = httpResponse(400, "text/plain; charset=utf-8", "bad request");
        sendAll(c, resp);
        return;
    }
    std::string method = head.substr(0, sp1);
    std::string path   = head.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);

    // Content-Length
    size_t contentLen = 0;
    std::string lowerHead = lowerCopy(head);
    size_t cl = lowerHead.find("content-length:");
    if (cl != std::string::npos)
        contentLen = std::strtoul(head.c_str() + cl + 15, nullptr, 10);

    std::string body = req.substr(headerEnd + 4);
    while (body.size() < contentLen && body.size() < 65536) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, (size_t)n);
    }
    if (body.size() > contentLen) body.resize(contentLen);

    std::string resp;
    route(method, path, body, resp);
    sendAll(c, resp);
}

void WebServer::route(const std::string& method, const std::string& path,
                      const std::string& body, std::string& out) {
    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            out = httpResponse(200, "text/html; charset=utf-8", kHtml);
            return;
        }
        if (path == "/api/status") {
            WebStatus s = status_ ? status_() : WebStatus{};
            out = httpResponse(200, "application/json", statusJson(s));
            return;
        }
        if (path == "/api/processes") {
            std::vector<ProcInfo> v = processes_ ? processes_() : std::vector<ProcInfo>{};
            out = httpResponse(200, "application/json", processesJson(v));
            return;
        }
        out = httpResponse(404, "text/plain; charset=utf-8", "not found");
        return;
    }

    if (method == "POST") {
        WebCommand cmd;

        if (path == "/api/target") {
            cmd.kind = WebCommand::SetTarget;
            jsonString(body, "name", cmd.target);
            if (command_) command_(cmd);
            out = httpResponse(200, "application/json", okJson());
            return;
        }
        if (path == "/api/config") {
            cmd.kind = WebCommand::SetConfig;
            double v = 0;

            if (jsonNumber(body, "gpu_limit_w", v))         cmd.config.gpuLimitW = (int)v;
            if (jsonNumber(body, "max_gpu_w", v))           cmd.config.maxGpuW = (int)v;
            if (jsonNumber(body, "cpu_max_pct", v))         cmd.config.cpuMaxPct = (int)v;
            if (jsonNumber(body, "cpu_min_pct", v))         cmd.config.cpuMinPct = (int)v;
            if (jsonNumber(body, "battery_threshold_w", v)) cmd.config.batteryThresholdW = v;
            if (jsonNumber(body, "step_w", v))              cmd.config.stepW = (int)v;
            if (jsonNumber(body, "hold_down_sec", v))       cmd.config.holdDownSec = (int)v;
            if (jsonNumber(body, "hold_up_sec", v))         cmd.config.holdUpSec = (int)v;
            if (jsonNumber(body, "gpu_clock_mhz", v))       cmd.config.gpuClockMhz = (int)v;
            if (jsonNumber(body, "min_gpu_clock_mhz", v))   cmd.config.minGpuClockMhz = (int)v;
            if (jsonNumber(body, "clock_step_mhz", v))      cmd.config.clockStepMhz = (int)v;
            if (jsonNumber(body, "auto_clock_mhz", v))      cmd.config.autoClockMhz = (int)v;
            if (jsonNumber(body, "min_gpu_w", v))           cmd.config.minGpuW = (int)v;
            if (jsonNumber(body, "cpu_step_pct", v))        cmd.config.cpuStepPct = (int)v;
            if (jsonNumber(body, "cpu_power_step", v))      cmd.config.cpuPowerStep = (int)v;
            if (jsonNumber(body, "prefer_cpu", v))          cmd.config.preferCpu = (int)v;
            if (jsonNumber(body, "deficit_energy_wh", v))   cmd.config.deficitEnergyWh = v;
            if (jsonNumber(body, "deficit_window_sec", v))  cmd.config.deficitWindowSec = (int)v;
            if (jsonNumber(body, "step_cooldown_sec", v))   cmd.config.stepCooldownSec = (int)v;
            if (jsonNumber(body, "dry_run", v))             cmd.config.dryRun = (int)v;
            if (jsonNumber(body, "apply_always", v))        cmd.config.applyAlways = (int)v;

            if (command_) command_(cmd);
            out = httpResponse(200, "application/json", okJson());
            return;
        }
        if (path == "/api/apply") {
            cmd.kind = WebCommand::Apply;
            if (command_) command_(cmd);
            out = httpResponse(200, "application/json", okJson());
            return;
        }
        if (path == "/api/restore") {
            cmd.kind = WebCommand::Restore;
            if (command_) command_(cmd);
            out = httpResponse(200, "application/json", okJson());
            return;
        }
        out = httpResponse(404, "text/plain; charset=utf-8", "not found");
        return;
    }

    out = httpResponse(404, "text/plain; charset=utf-8", "not found");
}

} // namespace pg
