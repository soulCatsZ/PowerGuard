#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
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
    const char* reason = code == 200 ? "OK"
                       : code == 403 ? "Forbidden"
                       : code == 404 ? "Not Found"
                       : code == 415 ? "Unsupported Media Type"
                                     : "Bad Request";
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

// 取请求头里某个字段的值 (名字大小写不敏感, 只认行首, 去掉首尾空白)
std::string headerValue(const std::string& head, const std::string& name) {
    std::string key = lowerCopy(name) + ":";
    std::string lower = lowerCopy(head);

    size_t at = std::string::npos;
    if (lower.compare(0, key.size(), key) == 0) {
        at = 0;                                   // 极少见: 头出现在首行
    } else {
        size_t p = lower.find("\n" + key);
        if (p != std::string::npos) at = p + 1;
    }
    if (at == std::string::npos) return {};

    size_t vs = at + key.size();
    size_t ve = head.find("\r\n", vs);
    if (ve == std::string::npos) ve = head.size();

    std::string v = head.substr(vs, ve - vs);
    size_t b = v.find_first_not_of(" \t");
    size_t e = v.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
}

// Host 必须是本机回环地址。绑定 127.0.0.1 只挡住外部设备,
// 挡不住 DNS rebinding —— 恶意域名解析到 127.0.0.1 后,
// 浏览器发来的 Host 仍是那个域名, 这里把它挡掉。
bool isLoopbackHost(const std::string& host) {
    if (host.empty()) return false;
    std::string h = lowerCopy(host);

    if (h == "[::1]" || h == "::1") return true;

    size_t colon = h.rfind(':');
    if (colon != std::string::npos) {
        std::string port = h.substr(colon + 1);
        bool numeric = !port.empty() &&
                       std::all_of(port.begin(), port.end(),
                                   [](unsigned char c) { return std::isdigit(c) != 0; });
        if (numeric) h.resize(colon);
    }
    return h == "127.0.0.1" || h == "localhost";
}

// POST 只收 application/json: 跨站表单 / no-cors fetch 只能发
// text/plain 这类"简单请求", 于是不会触发写操作。
bool isJsonContentType(const std::string& type) {
    return lowerCopy(type).compare(0, 16, "application/json") == 0;
}

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
  --field:rgba(118,118,128,.18);--field-hover:rgba(118,118,128,.26);
  --solid:#1c1c1e;--line:rgba(255,255,255,.10);
  --txt:#f5f5f7;--dim:#98989f;--hover:rgba(255,255,255,.06);
  --accent:#0a84ff;--ok:#30d158;--warn:#ff9f0a;--bad:#ff453a;
  --ring:0 0 0 3px rgba(10,132,255,.25);
  --shadow:0 18px 50px rgba(0,0,0,.55);--shadow-soft:0 8px 24px rgba(0,0,0,.35);
  --track:rgba(120,120,128,.34);--accent-soft:rgba(10,132,255,.14);
  --warn-soft:rgba(255,159,10,.14);--warn-fill:#ff9f0a;
  --ok-soft:rgba(48,209,88,.14);--bad-soft:rgba(255,69,58,.12);
  --ease:cubic-bezier(.22,1,.36,1);
  --blob-a:rgba(10,132,255,.20);
  --blob-b:rgba(94,92,230,.18);
  --blob-c:rgba(175,82,222,.14);
  --grid-line:rgba(255,255,255,.07);
  --spot:rgba(10,132,255,.20);
}
html[data-theme="light"]{
  --bg:#f2f2f7;--island:rgba(255,255,255,.72);--panel:rgba(255,255,255,.66);
  --field:rgba(118,118,128,.12);--field-hover:rgba(118,118,128,.19);
  --solid:#fff;--line:rgba(60,60,67,.14);
  --txt:#1c1c1e;--dim:#6e6e73;--hover:rgba(0,0,0,.045);
  --accent:#007aff;--ok:#248a3d;--warn:#b25000;--bad:#d70015;
  --ring:0 0 0 3px rgba(0,122,255,.20);
  --shadow:0 18px 50px rgba(31,35,41,.16);--shadow-soft:0 8px 24px rgba(31,35,41,.10);
  --track:rgba(120,120,128,.24);--accent-soft:rgba(0,122,255,.10);
  --warn-soft:rgba(255,159,10,.16);--warn-fill:#ff9f0a;
  --ok-soft:rgba(36,138,61,.10);--bad-soft:rgba(215,0,21,.08);
  --blob-a:rgba(0,122,255,.14);
  --blob-b:rgba(94,92,230,.11);
  --blob-c:rgba(175,82,222,.09);
  --grid-line:rgba(60,60,67,.08);
  --spot:rgba(0,122,255,.14);
}
html{color-scheme:dark;background:var(--bg)}
html[data-theme="light"]{color-scheme:light}
*{box-sizing:border-box}
::selection{background:rgba(10,132,255,.35);color:var(--txt)}
body{
  margin:0;min-height:100vh;background:var(--bg);color:var(--txt);
  font:14px/1.55 -apple-system,BlinkMacSystemFont,"SF Pro Text","Segoe UI",system-ui,
       "PingFang SC","Microsoft YaHei",sans-serif;
  -webkit-font-smoothing:antialiased;transition:background-color .3s,color .3s;
}

/* ---- 背景动效: 极光光斑 + 极淡网格 + 鼠标光晕 ---- */
.bg-fx{position:fixed;inset:0;z-index:0;overflow:hidden;pointer-events:none}
.bg-fx i{
  position:absolute;display:block;width:50vmax;height:50vmax;border-radius:50%;
  filter:blur(72px);will-change:transform;
  animation:drift 40s ease-in-out infinite alternate;
}
.bg-fx .b1{left:-18vmax;top:-20vmax;background:var(--blob-a)}
.bg-fx .b2{right:-20vmax;top:-10vmax;background:var(--blob-b);
  animation-duration:52s;animation-delay:-14s}
.bg-fx .b3{left:28%;bottom:-28vmax;background:var(--blob-c);
  animation-duration:60s;animation-delay:-28s}
.bg-grid{
  position:fixed;inset:0;z-index:0;pointer-events:none;opacity:.45;
  background-image:linear-gradient(var(--grid-line) 1px,transparent 1px),
                   linear-gradient(90deg,var(--grid-line) 1px,transparent 1px);
  background-size:56px 56px;
  -webkit-mask-image:radial-gradient(ellipse at 50% 0,#000 0%,transparent 72%);
  mask-image:radial-gradient(ellipse at 50% 0,#000 0%,transparent 72%);
}
.bg-spot{
  position:fixed;left:0;top:0;width:520px;height:520px;z-index:0;
  border-radius:50%;pointer-events:none;
  background:radial-gradient(circle,var(--spot),transparent 68%);
  transform:translate3d(calc(var(--mx,50vw) - 50%),
                       calc(var(--my,30vh) - 50%),0);
  transition:transform .12s linear;
}
main,footer{position:relative;z-index:1}
html.bg-paused .bg-fx i{animation-play-state:paused}

@keyframes drift{
  0%  {transform:translate3d(0,0,0) scale(1)}
  50% {transform:translate3d(5vw,4vh,0) scale(1.08)}
  100%{transform:translate3d(-4vw,7vh,0) scale(.97)}
}

/* ---- 悬浮胶囊顶栏 ---- */
header{
  position:sticky;top:10px;z-index:20;max-width:1060px;margin:10px auto 0;
  padding:8px 10px 8px 16px;display:flex;align-items:center;gap:10px;flex-wrap:wrap;
  background:var(--island);border:1px solid var(--line);border-radius:28px;
  backdrop-filter:blur(28px) saturate(180%);-webkit-backdrop-filter:blur(28px) saturate(180%);
  box-shadow:var(--shadow);transition:background .3s,border-color .3s;
  animation:drop .5s var(--ease) backwards;
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
.badge.on{background:var(--ok-soft);color:var(--ok)}
.badge.on::before{animation:dotPulse 2.2s ease-in-out infinite}
.badge.warn{background:var(--warn-soft);color:var(--warn)}
.badge.bad{background:var(--bad-soft);color:var(--bad)}

/* ---- 主题切换按钮 ---- */
button{
  font:inherit;font-size:13px;font-weight:500;color:var(--txt);background:var(--field);
  border:1px solid var(--line);border-radius:999px;padding:9px 18px;cursor:pointer;
  transition:transform .16s var(--ease),background .2s,border-color .2s,box-shadow .2s,color .2s;
  white-space:nowrap;
}
button:hover{border-color:var(--accent);transform:translateY(-1px)}
button:active{transform:translateY(0) scale(.97)}
button:focus-visible{outline:none;border-color:var(--accent);box-shadow:var(--ring)}
button.primary{
  background:var(--accent);border-color:transparent;color:#fff;font-weight:600;
  box-shadow:0 8px 22px rgba(10,132,255,.30);
}
button.primary:hover{box-shadow:0 12px 28px rgba(10,132,255,.40)}
button.danger{color:var(--bad);border-color:rgba(255,69,58,.35);background:rgba(255,69,58,.10)}
button.danger:hover{border-color:var(--bad);background:var(--bad-soft)}
.icon-btn{width:38px;height:38px;padding:0;border-radius:50%;display:grid;place-items:center;flex:none}
.icon-btn svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;transition:transform .3s var(--ease)}
.icon-btn:hover svg{transform:rotate(-10deg)}
html[data-theme="dark"] #ico-sun{display:none}
html[data-theme="light"] #ico-moon{display:none}

/* ---- 指标卡片 ---- */
main{max-width:1060px;margin:0 auto;padding:18px 20px 8px}
.hero{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}
.metric{
  background:var(--panel);border:1px solid var(--line);border-radius:28px;padding:16px 18px;
  backdrop-filter:blur(22px);-webkit-backdrop-filter:blur(22px);box-shadow:var(--shadow-soft);
  transition:transform .25s var(--ease),background .3s,border-color .25s,box-shadow .25s;
  animation:rise .55s var(--ease) backwards;
}
.metric:nth-child(1){animation-delay:.03s}
.metric:nth-child(2){animation-delay:.07s}
.metric:nth-child(3){animation-delay:.11s}
.metric:nth-child(4){animation-delay:.15s}
.metric:hover{transform:translateY(-3px);border-color:rgba(10,132,255,.24);box-shadow:var(--shadow)}
.metric-top{display:flex;align-items:center;justify-content:space-between;gap:10px}
.metric-ico{
  width:32px;height:32px;border-radius:11px;display:grid;place-items:center;flex:none;
  background:var(--field);color:var(--dim);
  transition:color .25s,background .25s,transform .25s var(--ease);
}
.metric:hover .metric-ico{color:var(--accent);background:var(--accent-soft);transform:translateY(-1px)}
.metric-ico svg{width:17px;height:17px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.k{font-size:11px;font-weight:500;letter-spacing:.08em;text-transform:uppercase;color:var(--dim)}
.big{font-size:32px;font-weight:600;letter-spacing:-.02em;line-height:1.2;margin-top:4px;font-variant-numeric:tabular-nums}
.big.text{font-size:22px}
.sub{font-size:12px;color:var(--dim);margin-top:3px}

/* ---- 面板 ---- */
.panel{
  background:var(--panel);border:1px solid var(--line);border-radius:30px;
  padding:20px 22px;margin-top:14px;backdrop-filter:blur(22px);-webkit-backdrop-filter:blur(22px);
  box-shadow:var(--shadow-soft);transition:background .3s,border-color .3s;
  animation:rise .6s var(--ease) backwards;
}
section.panel:nth-of-type(1){animation-delay:.08s}
section.panel:nth-of-type(2){animation-delay:.14s}
section.panel:nth-of-type(3){animation-delay:.20s}
.panel-head{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:14px}
.panel-head h3{
  display:flex;align-items:center;gap:8px;font-size:15px;font-weight:600;
  letter-spacing:-.01em;margin:0;
}
.panel-head h3::before{
  content:"";width:3px;height:14px;border-radius:2px;flex:none;
  background:linear-gradient(180deg,var(--accent),#5e5ce6);opacity:.9;
}
.hint{font-size:12px;color:var(--dim)}
.hint.live{display:inline-flex;align-items:center;gap:6px}
.hint.live::before{content:"";width:6px;height:6px;border-radius:50%;background:var(--ok);animation:dotPulse 2.2s ease-in-out infinite}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}
.stat{background:var(--field);border-radius:22px;padding:14px 16px;transition:background .3s}
.stat .v{font-size:26px;font-weight:600;letter-spacing:-.01em;line-height:1.25;margin-top:2px;font-variant-numeric:tabular-nums}
.stat .v.text{font-size:16px;line-height:1.4;word-break:break-all}

/* ---- 参数 ---- */
.group{margin-top:16px}
.group:first-of-type{margin-top:0}
.group h4{
  display:flex;align-items:center;gap:6px;
  font-size:11px;font-weight:600;letter-spacing:.1em;text-transform:uppercase;
  color:var(--dim);margin:0 0 8px;padding-left:6px;
}
.group h4::before{content:"";width:5px;height:5px;border-radius:50%;background:var(--accent);opacity:.75;flex:none}
.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}
.field{
  background:var(--field);border:1px solid transparent;border-radius:18px;padding:8px 14px 9px;
  transition:border-color .2s,box-shadow .2s,background .2s;
}
.field:hover{background:var(--field-hover)}
.field:focus-within{border-color:var(--accent);box-shadow:var(--ring);background:var(--field)}
.field label{display:block;font-size:11px;color:var(--dim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.field input,.field select{
  width:100%;border:0;background:transparent;color:var(--txt);font:inherit;font-size:15px;
  font-weight:500;padding:2px 0;outline:none;font-variant-numeric:tabular-nums;caret-color:var(--accent);
}
.field input::placeholder{color:var(--dim)}
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
.toggle:hover{border-color:rgba(128,128,128,.4);background:var(--field-hover)}
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

/* ---- 进程选择器 ---- */
.panel-proc{position:relative;z-index:3}
.proc-picker{display:flex;gap:10px;align-items:flex-end;flex-wrap:wrap}
.combo-wrap{flex:1 1 320px;min-width:0}
.combo-label{display:flex;justify-content:space-between;gap:8px;font-size:11px;color:var(--dim);margin:0 0 6px 4px}
.combo-label span{opacity:.78;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.combo{position:relative}
.combo-field{
  display:flex;align-items:center;gap:10px;min-height:50px;padding:6px 6px 6px 12px;
  background:var(--field);border:1px solid var(--line);border-radius:16px;
  transition:border-color .2s,box-shadow .2s,background .2s;
}
.combo-field:hover{background:var(--field-hover)}
.combo-field:focus-within,.combo.open .combo-field{
  border-color:var(--accent);box-shadow:var(--ring);background:var(--field);
}
.combo-glyph{
  width:30px;height:30px;border-radius:10px;display:grid;place-items:center;flex:none;
  background:var(--accent-soft);color:var(--accent);transition:transform .25s var(--ease);
}
.combo.open .combo-glyph{transform:scale(1.04)}
.combo-glyph svg{width:16px;height:16px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.combo-field input{
  flex:1;min-width:0;border:0;outline:none;background:transparent;color:var(--txt);
  font:inherit;font-size:15px;font-weight:500;padding:8px 0;letter-spacing:-.01em;
  caret-color:var(--accent);
}
.combo-field input::placeholder{color:var(--dim);font-weight:400}
.combo-toggle{
  width:34px;height:34px;padding:0;border-radius:12px;border:1px solid transparent;
  display:grid;place-items:center;flex:none;background:transparent;color:var(--dim);
  box-shadow:none;
}
.combo-toggle:hover{background:var(--hover);border-color:transparent;color:var(--txt);transform:none}
.combo-toggle svg{width:17px;height:17px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;transition:transform .25s var(--ease)}
.combo.open .combo-toggle{color:var(--accent);background:var(--accent-soft)}
.combo.open .combo-toggle svg{transform:rotate(180deg)}

.combo-menu{
  position:absolute;left:0;right:0;top:calc(100% + 8px);z-index:6;
  background:var(--solid);border:1px solid var(--line);border-radius:20px;
  box-shadow:var(--shadow);overflow:hidden;
  opacity:0;visibility:hidden;pointer-events:none;
  transform:translateY(-7px) scale(.985);transform-origin:top center;
  transition:opacity .18s var(--ease),transform .22s var(--ease),visibility 0s .22s,border-color .3s,background .3s;
}
.combo.open .combo-menu{
  opacity:1;visibility:visible;pointer-events:auto;
  transform:translateY(0) scale(1);
  transition:opacity .18s var(--ease),transform .22s var(--ease),visibility 0s 0s;
}
.combo-search{
  display:flex;align-items:center;gap:8px;padding:9px 10px 9px 13px;
  border-bottom:1px solid var(--line);background:var(--field);
  transition:background .2s;
}
.combo-search:focus-within{background:var(--field-hover)}
.combo-search > svg{width:15px;height:15px;flex:none;color:var(--dim);fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.combo-search input{
  flex:1;min-width:0;border:0;outline:none;background:transparent;color:var(--txt);
  font:inherit;font-size:13px;padding:5px 0;caret-color:var(--accent);
}
.combo-search input::placeholder{color:var(--dim)}
.combo-refresh{
  width:30px;height:30px;padding:0;border-radius:9px;display:grid;place-items:center;flex:none;
  background:transparent;border:1px solid transparent;color:var(--dim);box-shadow:none;
}
.combo-refresh:hover{background:var(--hover);border-color:transparent;color:var(--txt);transform:none}
.combo-refresh svg{width:15px;height:15px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round;transition:transform .3s var(--ease)}
.combo.loading .combo-refresh{pointer-events:none;opacity:.55}
.combo.loading .combo-refresh svg{animation:spin .9s linear infinite}

.combo-list{
  max-height:min(340px,48vh);overflow:auto;overscroll-behavior:contain;
  padding:6px;display:flex;flex-direction:column;gap:2px;
}
.combo-list::-webkit-scrollbar{width:10px;height:10px}
.combo-list::-webkit-scrollbar-thumb{
  background:var(--track);border-radius:999px;border:3px solid transparent;background-clip:content-box;
}
.combo-list::-webkit-scrollbar-thumb:hover{background:var(--dim);background-clip:content-box}
.proc-row{
  display:flex;align-items:center;gap:10px;width:100%;padding:8px 10px;
  border:0;border-radius:13px;background:transparent;color:var(--txt);text-align:left;
  cursor:pointer;font:inherit;box-shadow:none;
  transition:background .16s,box-shadow .16s;
}
.proc-row:hover{background:var(--hover);border-color:transparent;transform:none;box-shadow:none}
.proc-row.active{background:var(--hover);box-shadow:inset 0 0 0 1px var(--line)}
.proc-row.selected{background:var(--accent-soft);box-shadow:inset 3px 0 0 var(--accent)}
.proc-row.selected:hover{background:var(--accent-soft);border-color:transparent}
.proc-row.enter{animation:procIn .26s var(--ease) backwards}
.proc-avatar{
  width:30px;height:30px;border-radius:9px;display:grid;place-items:center;flex:none;
  font-size:12px;font-weight:700;color:var(--accent);
  background:linear-gradient(135deg,rgba(10,132,255,.22),rgba(94,92,230,.16));
}
html[data-theme="light"] .proc-avatar{
  background:linear-gradient(135deg,rgba(0,122,255,.14),rgba(94,92,230,.10));
}
.proc-meta{min-width:0;flex:1;display:flex;flex-direction:column;gap:1px}
.proc-name{font-size:13px;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.proc-pid{font-size:11px;color:var(--dim);font-variant-numeric:tabular-nums}
.proc-check{
  width:18px;height:18px;flex:none;color:var(--accent);
  opacity:0;transform:scale(.72);transition:opacity .16s,transform .18s var(--ease);
}
.proc-check::after{
  content:"";display:block;width:5px;height:9px;margin:-2px auto 0;
  border:solid currentColor;border-width:0 2px 2px 0;transform:rotate(45deg);
}
.proc-row.selected .proc-check{opacity:1;transform:none}
.combo-empty{padding:26px 16px;text-align:center;color:var(--dim);font-size:13px}
.combo-empty .empty-ico{width:26px;height:26px;display:block;margin:0 auto 8px;opacity:.45;fill:none;stroke:currentColor;stroke-width:1.6;stroke-linecap:round;stroke-linejoin:round}
.combo-empty button{margin-top:10px;padding:7px 15px;font-size:12px}
.combo-foot{
  display:flex;justify-content:space-between;gap:10px;align-items:center;
  padding:8px 12px;border-top:1px solid var(--line);
  font-size:11px;color:var(--dim);background:var(--field);
}
.combo-foot span:first-child{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.combo-tip{white-space:nowrap;flex:none}
.combo-actions{display:flex;gap:8px;flex-wrap:wrap}
.skel{
  height:38px;margin:2px 4px;border-radius:12px;
  background:linear-gradient(90deg,var(--field) 0%,var(--field-hover) 50%,var(--field) 100%);
  background-size:200% 100%;animation:shimmer 1.4s linear infinite;
}

footer{max-width:1060px;margin:0 auto;padding:14px 20px 30px;color:var(--dim);font-size:12px;text-align:center}

/* ---- 提示气泡 ---- */
#msg{
  position:fixed;left:50%;bottom:26px;z-index:50;pointer-events:none;
  transform:translate(-50%,16px);opacity:0;transition:opacity .2s,transform .25s;
  background:var(--solid);border:1px solid var(--line);border-radius:999px;
  padding:10px 20px 10px 32px;font-size:13px;box-shadow:var(--shadow);
  white-space:nowrap;max-width:90vw;overflow:hidden;text-overflow:ellipsis;
}
#msg::before{
  content:"";position:absolute;left:16px;top:50%;width:7px;height:7px;border-radius:50%;
  background:var(--ok);box-shadow:0 0 0 3px var(--ok-soft);transform:translateY(-50%);
}
#msg.err::before{background:var(--bad);box-shadow:0 0 0 3px var(--bad-soft)}
#msg.show{opacity:1;transform:translate(-50%,0)}

@keyframes rise{from{opacity:0;transform:translateY(12px)}to{opacity:1;transform:none}}
@keyframes drop{from{opacity:0;transform:translateY(-10px)}to{opacity:1;transform:none}}
@keyframes procIn{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
@keyframes shimmer{0%{background-position:200% 0}100%{background-position:-200% 0}}
@keyframes spin{to{transform:rotate(360deg)}}
@keyframes dotPulse{
  0%,100%{box-shadow:0 0 0 0 rgba(48,209,88,0)}
  50%{box-shadow:0 0 0 4px rgba(48,209,88,.14)}
}

@media (max-width:640px){
  header{top:6px;margin:6px 8px 0;border-radius:24px;padding:7px 8px 7px 14px}
  main{padding:12px 10px 8px}
  .panel{padding:16px;border-radius:24px;margin-top:10px}
  .metric{padding:14px 16px;border-radius:24px}
  .big{font-size:28px}
  .hero{gap:8px}
  .fields{grid-template-columns:repeat(auto-fit,minmax(135px,1fr))}
  .combo-wrap{flex-basis:100%}
  .combo-actions{width:100%}
  .combo-actions button{flex:1 1 0}
  .combo-tip{display:none}
  footer{padding:12px 16px 24px}
}
@media (prefers-reduced-motion:reduce){
  *,*::before,*::after{transition:none!important;animation:none!important;scroll-behavior:auto!important}
}
</style>
</head>
<body>
<div class="bg-fx" aria-hidden="true">
  <i class="b1"></i><i class="b2"></i><i class="b3"></i>
</div>
<div class="bg-grid" aria-hidden="true"></div>
<div class="bg-spot" aria-hidden="true"></div>
<header>
  <div class="brand"><span class="logo">PG</span><h1>PowerGuard</h1></div>
  <div class="badges">
    <span id="b-state" class="badge">连接中…</span>
    <span id="b-game" class="badge">目标未运行</span>
    <span id="b-dry" class="badge warn" style="display:none">DRY-RUN</span>
    <span id="b-paused" class="badge warn" style="display:none">已暂停</span>
  </div>
  <span class="spacer"></span>
  <button id="theme-btn" class="icon-btn" type="button" onclick="toggleTheme()" title="切换主题" aria-label="切换主题">
    <svg id="ico-moon" viewBox="0 0 24 24" aria-hidden="true"><path d="M20.5 14.2A8.5 8.5 0 1 1 9.8 3.5a6.8 6.8 0 0 0 10.7 10.7Z"/></svg>
    <svg id="ico-sun" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="4"/><path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M18.7 5.3l-1.4 1.4M6.7 17.3l-1.4 1.4"/></svg>
  </button>
</header>

<main>
  <div class="hero">
    <div class="metric">
      <div class="metric-top"><div class="k">CPU 功率</div><span class="metric-ico" aria-hidden="true"><svg viewBox="0 0 24 24"><rect x="7" y="7" width="10" height="10" rx="2"/><path d="M9 3v2M15 3v2M9 19v2M15 19v2M3 9h2M3 15h2M19 9h2M19 15h2"/></svg></span></div>
      <div class="big" id="v-cpu">--</div><div class="sub">RAPL Energy Meter</div>
    </div>
    <div class="metric">
      <div class="metric-top"><div class="k">GPU 功率</div><span class="metric-ico" aria-hidden="true"><svg viewBox="0 0 24 24"><rect x="3" y="6" width="18" height="12" rx="2"/><circle cx="12" cy="12" r="3"/><path d="M12 9c0-2.2.8-3 2.5-3M15 12c2.2 0 3 .8 3 2.5M12 15c0 2.2-.8 3-2.5 3M9 12c-2.2 0-3-.8-3-2.5"/></svg></span></div>
      <div class="big" id="v-gpu">--</div><div class="sub" id="s-gpu">NVML</div>
    </div>
    <div class="metric">
      <div class="metric-top"><div class="k">电池净功率</div><span class="metric-ico" aria-hidden="true"><svg viewBox="0 0 24 24"><rect x="2" y="7" width="18" height="10" rx="3"/><path d="M22 10v4M6 10v4M10 10v4M14 10v4"/></svg></span></div>
      <div class="big" id="v-bat">--</div><div class="sub" id="s-bat">--</div>
    </div>
    <div class="metric">
      <div class="metric-top"><div class="k">供电</div><span class="metric-ico" aria-hidden="true"><svg viewBox="0 0 24 24"><path d="M13 2 4.5 13.5H11L10 22l8.5-11.5H12L13 2Z"/></svg></span></div>
      <div class="big text" id="v-ac">--</div><div class="sub" id="s-ac">--</div>
    </div>
  </div>

  <section class="panel">
    <div class="panel-head"><h3>控制状态</h3><span class="hint live">每秒刷新</span></div>
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
      <button class="primary" type="button" onclick="saveCfg()">保存并应用</button>
      <button type="button" onclick="send('/api/apply', {}, '已重新应用')">立即应用</button>
      <button class="danger" type="button" onclick="send('/api/restore', {}, '已恢复原始设置')">恢复原始设置</button>
    </div>
  </section>

  <section class="panel panel-proc">
    <div class="panel-head"><h3>进程</h3><span class="hint">点选即切换目标，也可手动输入</span></div>
    <div class="proc-picker">
      <div class="combo-wrap">
        <label class="combo-label" for="f-target">监控目标 <span>可手填，自动补 .exe</span></label>
        <div class="combo" id="proc-combo">
          <div class="combo-field">
            <span class="combo-glyph" aria-hidden="true"><svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="3"/><path d="M3 9h18M7 6.5h.01M10 6.5h.01"/></svg></span>
            <input id="f-target" type="text" placeholder="选择或输入进程名" autocomplete="off" spellcheck="false">
            <button id="combo-toggle" class="combo-toggle" type="button" aria-haspopup="listbox" aria-expanded="false" aria-controls="proc-menu" aria-label="选择进程" title="选择进程">
              <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg>
            </button>
          </div>
          <div class="combo-menu" id="proc-menu">
            <div class="combo-search">
              <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="6.5"/><path d="m16 16 4 4"/></svg>
              <input id="proc-filter" type="text" role="combobox" aria-autocomplete="list" aria-expanded="false" aria-controls="proc-list" placeholder="搜索进程…" autocomplete="off" spellcheck="false">
              <button id="proc-refresh" class="combo-refresh" type="button" aria-label="刷新进程列表" title="刷新进程列表">
                <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>
              </button>
            </div>
            <div class="combo-list" id="proc-list" role="listbox" aria-label="进程列表"></div>
            <div class="combo-foot">
              <span id="proc-count">加载中…</span>
              <span class="combo-tip">↑↓ 选择 · Enter 确认 · Esc 关闭</span>
            </div>
          </div>
        </div>
      </div>
      <div class="combo-actions">
        <button class="primary" type="button" onclick="saveTarget()">设为目标</button>
        <button class="danger" type="button" onclick="clearTarget()">清除</button>
      </div>
    </div>
  </section>
</main>
<footer id="foot">--</footer>
<div id="msg" role="status" aria-live="polite"></div>

<script>
var procs = [];
var cfgSig = '';
var inflight = false;
var targetName = '';
var targetDirty = false;
var gamePid = 0;
var visibleProcs = [];
var activeIndex = -1;
var menuOpen = false;
var procsLoading = false;
var procsError = false;
var pollTimer = null;

var ICON_SEARCH = '<svg class="empty-ico" viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="6.5"/><path d="m16 16 4 4"/></svg>';

function $(id){ return document.getElementById(id); }
function esc(s){
  return String(s == null ? '' : s).replace(/[&<>"']/g, function(c){
    return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];
  });
}
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

function msg(t, isErr){
  var e = $('msg');
  if (!e) return;
  e.textContent = t;
  e.classList.toggle('err', !!isErr);
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

  var tgt = s.target || '';
  var newPid = s.game_pid || 0;
  var changed = (tgt !== targetName) || (newPid !== gamePid);
  targetName = tgt;
  gamePid = newPid;
  setText('v-target', tgt || '(未设置)');
  setText('s-target', (s.apply_always ? '启动即生效' :
          (tgt ? '仅目标运行时生效' : '未设置则任何进程都不触发')) +
          (s.game_profile ? ' · 方案 ' + s.game_profile : ''));
  var ti = $('f-target');
  if (ti && !targetDirty && ti.value !== tgt) ti.value = tgt;
  if (changed && menuOpen) renderProcs();

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
  }).then(function(r){
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  });
}
function send(url, body, okMsg, onOk){
  post(url, body).then(function(r){
    if (okMsg) msg(okMsg);
    if (onOk) onOk(r);
  }).catch(function(){
    msg('操作失败，请重试', true);
  });
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
  send('/api/config', body, '参数已保存；控制中会立即重新应用', function(){
    cfgSig = '';
  });
}

/* ---- 进程选择器 ---- */
function loadProcs(quiet){
  procsLoading = true;
  procsError = false;
  activeIndex = -1;
  renderProcs();
  fetch('/api/processes', {cache: 'no-store'})
    .then(function(r){ if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function(v){
      procs = Array.isArray(v) ? v : [];
      procsLoading = false;
      procsError = false;
      activeIndex = -1;
      renderProcs();
      if (!quiet) msg('进程列表已刷新');
    })
    .catch(function(){
      procsLoading = false;
      procsError = true;
      renderProcs();
      if (!quiet) msg('获取进程列表失败', true);
    });
}

function renderProcs(){
  var list = $('proc-list');
  if (!list) return;
  var combo = $('proc-combo');
  if (combo) combo.classList.toggle('loading', procsLoading);
  var count = $('proc-count');
  var input = $('proc-filter');
  var q = ((input && input.value) || '').toLowerCase().trim();
  activeIndex = -1;
  visibleProcs = [];

  if (procsLoading) {
    if (count) count.textContent = '加载中…';
    var sk = '';
    for (var s = 0; s < 5; s++) sk += '<div class="skel"></div>';
    list.innerHTML = sk;
    return;
  }

  if (procsError) {
    if (count) count.textContent = '加载失败';
    list.innerHTML = '<div class="combo-empty">' + ICON_SEARCH +
      '<div>无法获取进程列表</div>' +
      '<button class="primary" type="button" onclick="loadProcs()">重试</button></div>';
    return;
  }

  var matched = 0;
  for (var i = 0; i < procs.length; i++) {
    var p = procs[i];
    if (q && p.name.toLowerCase().indexOf(q) < 0) continue;
    matched++;
    if (visibleProcs.length < 400) visibleProcs.push(i);
  }

  var pidMatch = false;
  if (gamePid && targetName) {
    for (var j = 0; j < procs.length; j++) {
      if (procs[j].pid === gamePid && procs[j].name.toLowerCase() === targetName.toLowerCase()) {
        pidMatch = true;
        break;
      }
    }
  }

  var selectedShown = false;
  var rows = '';
  for (var k = 0; k < visibleProcs.length; k++) {
    var idx = visibleProcs[k];
    var proc = procs[idx];
    var sel = !selectedShown && !!targetName &&
              proc.name.toLowerCase() === targetName.toLowerCase() &&
              (!pidMatch || proc.pid === gamePid);
    if (sel) selectedShown = true;
    var initial = esc((proc.name.replace(/\.exe$/i, '').charAt(0) || '?').toUpperCase());
    var delay = k < 24 ? (k * 14) : 0;
    rows += '<button type="button" class="proc-row' + (sel ? ' selected' : '') +
      (k < 24 ? ' enter' : '') + '" role="option" aria-selected="' + (sel ? 'true' : 'false') +
      '" tabindex="-1" id="proc-opt-' + idx + '" onclick="pick(' + idx + ')"' +
      ' title="' + esc(proc.name) + '"' +
      (k < 24 ? ' style="animation-delay:' + delay + 'ms"' : '') + '>' +
      '<span class="proc-avatar">' + initial + '</span>' +
      '<span class="proc-meta"><span class="proc-name">' + esc(proc.name) + '</span>' +
      '<span class="proc-pid">PID ' + (Number(proc.pid) || 0) + '</span></span>' +
      '<span class="proc-check" aria-hidden="true"></span>' +
      '</button>';
  }

  if (!rows) {
    if (q) {
      rows = '<div class="combo-empty">' + ICON_SEARCH +
        '<div>没有匹配的进程</div>' +
        '<button class="primary" type="button" onclick="clearFilter()">清除筛选</button></div>';
    } else {
      rows = '<div class="combo-empty">' + ICON_SEARCH +
        '<div>进程列表为空</div>' +
        '<button class="primary" type="button" onclick="loadProcs()">刷新</button></div>';
    }
  }
  list.innerHTML = rows;

  if (count) {
    var label = q ? ('匹配 ' + matched + ' / 共 ' + procs.length + ' 个')
                  : ('共 ' + procs.length + ' 个进程');
    if (matched > visibleProcs.length) label += ' · 仅显示前 ' + visibleProcs.length + ' 个';
    count.textContent = label;
  }
  updateActiveDescendant();
}

function updateActiveDescendant(){
  var f = $('proc-filter');
  if (!f) return;
  if (menuOpen && activeIndex >= 0 && visibleProcs[activeIndex] !== undefined) {
    f.setAttribute('aria-activedescendant', 'proc-opt-' + visibleProcs[activeIndex]);
  } else {
    f.removeAttribute('aria-activedescendant');
  }
}

function setActive(pos){
  if (!visibleProcs.length) return;
  if (pos < 0) pos = visibleProcs.length - 1;
  if (pos >= visibleProcs.length) pos = 0;
  activeIndex = pos;
  var rows = $('proc-list') ? $('proc-list').querySelectorAll('.proc-row') : [];
  for (var i = 0; i < rows.length; i++) rows[i].classList.toggle('active', i === pos);
  updateActiveDescendant();
  if (rows[pos] && rows[pos].scrollIntoView) rows[pos].scrollIntoView({block: 'nearest'});
}

function openMenu(){
  if (menuOpen) return;
  menuOpen = true;
  var c = $('proc-combo');
  if (c) c.classList.add('open');
  var b = $('combo-toggle');
  if (b) { b.setAttribute('aria-expanded', 'true'); b.title = '收起进程列表'; }
  var f = $('proc-filter');
  if (f) { f.setAttribute('aria-expanded', 'true'); }
  renderProcs();
  var sel = document.querySelector('#proc-list .proc-row.selected');
  if (sel && sel.scrollIntoView) sel.scrollIntoView({block: 'nearest'});
  if (f) { f.focus(); if (f.select) f.select(); }
}

function closeMenu(focusToggle){
  if (!menuOpen) return;
  menuOpen = false;
  var c = $('proc-combo');
  if (c) c.classList.remove('open');
  var b = $('combo-toggle');
  if (b) { b.setAttribute('aria-expanded', 'false'); b.title = '选择进程'; }
  var f = $('proc-filter');
  if (f) { f.setAttribute('aria-expanded', 'false'); f.blur(); }
  activeIndex = -1;
  updateActiveDescendant();
  if (focusToggle && b) b.focus();
}

function toggleMenu(){
  if (menuOpen) closeMenu(true); else openMenu();
}

function filterProcs(){
  activeIndex = -1;
  renderProcs();
}

function clearFilter(){
  var f = $('proc-filter');
  if (f) { f.value = ''; f.focus(); }
  activeIndex = -1;
  renderProcs();
}

function pick(i){
  var p = procs[i];
  if (!p) return;
  targetDirty = false;
  targetName = p.name;
  var ti = $('f-target');
  if (ti) ti.value = p.name;
  renderProcs();
  closeMenu(true);
  send('/api/target', {name: p.name}, '监控目标已设为 ' + p.name);
}

function saveTarget(){
  var ti = $('f-target');
  var name = ti ? ti.value.trim() : '';
  targetDirty = false;
  if (!name) { clearTarget(); return; }
  targetName = name;
  renderProcs();
  closeMenu(true);
  send('/api/target', {name: name}, '监控目标已更新');
}

function clearTarget(){
  targetDirty = false;
  targetName = '';
  var ti = $('f-target');
  if (ti) ti.value = '';
  renderProcs();
  closeMenu(true);
  send('/api/target', {name: ''}, '已清除监控目标');
}

function onTargetKey(e){
  if (e.key === 'ArrowDown') { e.preventDefault(); openMenu(); }
  else if (e.key === 'Enter') { e.preventDefault(); saveTarget(); }
  else if (e.key === 'Escape' && menuOpen) { e.preventDefault(); closeMenu(true); }
}

function onFilterKey(e){
  if (e.key === 'Escape') { e.preventDefault(); closeMenu(true); }
  else if (e.key === 'ArrowDown') { e.preventDefault(); setActive(activeIndex + 1); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); setActive(activeIndex - 1); }
  else if (e.key === 'Home') { e.preventDefault(); setActive(0); }
  else if (e.key === 'End') { e.preventDefault(); setActive(visibleProcs.length - 1); }
  else if (e.key === 'Enter') {
    e.preventDefault();
    var pos = activeIndex >= 0 ? activeIndex : 0;
    if (visibleProcs[pos] !== undefined) pick(visibleProcs[pos]);
  }
}

(function attachProcUi(){
  var toggle = $('combo-toggle');
  if (toggle) toggle.addEventListener('click', function(e){ e.stopPropagation(); toggleMenu(); });

  var refresh = $('proc-refresh');
  if (refresh) refresh.addEventListener('click', function(e){ e.stopPropagation(); loadProcs(); });

  var target = $('f-target');
  if (target) {
    target.addEventListener('input', function(){ targetDirty = true; });
    target.addEventListener('keydown', onTargetKey);
  }

  var filter = $('proc-filter');
  if (filter) {
    filter.addEventListener('input', filterProcs);
    filter.addEventListener('keydown', onFilterKey);
  }

  document.addEventListener('click', function(e){
    if (!menuOpen) return;
    var combo = $('proc-combo');
    if (combo && !combo.contains(e.target)) closeMenu(false);
  });
})();

/* 鼠标光晕: 只在支持悬停的设备上跟随; 降低动态效果时保持静止。 */
(function attachBgSpot(){
  var reduce = window.matchMedia &&
               window.matchMedia('(prefers-reduced-motion: reduce)');
  if (reduce && reduce.matches) return;
  var root = document.documentElement;
  var frame = 0, mx = 0, my = 0;
  function paint(){
    frame = 0;
    root.style.setProperty('--mx', mx + 'px');
    root.style.setProperty('--my', my + 'px');
  }
  window.addEventListener('pointermove', function(e){
    if (e.pointerType && e.pointerType !== 'mouse' && e.pointerType !== 'pen') return;
    mx = e.clientX;
    my = e.clientY;
    if (!frame) frame = requestAnimationFrame(paint);
  }, {passive:true});
})();

function startPolling(){
  if (pollTimer) return;
  tick();
  pollTimer = setInterval(tick, 1000);
}
function stopPolling(){
  if (!pollTimer) return;
  clearInterval(pollTimer);
  pollTimer = null;
}
document.addEventListener('visibilitychange', function(){
  document.documentElement.classList.toggle('bg-paused', document.hidden);
  if (document.hidden) stopPolling(); else startPolling();
});

syncToggles();
loadProcs(true);        /* 打开页面就先填好进程列表 */
startPolling();
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

    // 只接受发往回环地址的请求, 防止 DNS rebinding 借本机地址改参数
    if (!isLoopbackHost(headerValue(head, "host"))) {
        sendAll(c, httpResponse(403, "text/plain; charset=utf-8", "forbidden host"));
        return;
    }
    // 写操作必须是 JSON, 挡住跨站表单和 no-cors fetch 这类"简单请求"
    if (method == "POST" && !isJsonContentType(headerValue(head, "content-type"))) {
        sendAll(c, httpResponse(415, "text/plain; charset=utf-8",
                                "expected application/json"));
        return;
    }

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
