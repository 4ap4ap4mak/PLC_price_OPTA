#include "SettingsPage.h"

#include <ForceOnTimer.h>
#include <Planner.h>
#include <PriceSeries.h>
#include <RigaTime.h>

#include "ClientWriter.h"

using channelconfig::ChannelConfig;
using channelconfig::kChannelCount;

static void printTimeOfDay(Print& out, uint16_t minuteOfDay) {
    char text[8];
    rigatime::formatTimeOfDay(minuteOfDay, text, sizeof(text));
    out.print(text);
}

static String htmlEscape(const char* value) {
    String escaped;
    while (*value != '\0') {
        switch (*value) {
            case '&': escaped += F("&amp;"); break;
            case '<': escaped += F("&lt;"); break;
            case '>': escaped += F("&gt;"); break;
            case '"': escaped += F("&quot;"); break;
            case '\'': escaped += F("&#39;"); break;
            default: escaped += *value; break;
        }
        ++value;
    }
    return escaped;
}

// Данные графика для скрипта страницы: цены вчера–завтра и план автоматики
// выбранного реле (тот же buildPlan, что и для решения, KI-005). Рисует браузер.
// Отдельного запроса картинки нет: сервер обслуживает одно соединение за раз, и
// параллельный запрос /chart.svg сбрасывался.
//   n  — сейчас, epoch UTC (0 — время не синхронизировано); d — выбранные сутки
//   c  — текущая цена, ц/кВт·ч; t0 — начало первого интервала
//   t  — номер интервала от t0; p — цена, ц/кВт·ч
//   r  — бит 16: план включает реле; младшие 4 бита — planner::Reason
//   rn — названия причин по номеру planner::Reason
//   ro — причины, которые выключают реле (отпуск без интервала, аварийный предел)
//   f  — конец принудительного включения выбранного реле, epoch UTC (0 — не идет)
static void printChartData(Print& client, controller::Controller& channels,
    const controller::Clock& clock, uint8_t channel, int8_t chartDay) {
    const uint32_t now = clock.synced() ? clock.nowUtc() : 0;
    const planner::ChannelPlan& channelPlan = channels.chartPlan(channel, now);
    const prices::PriceSeries& forecast = channels.forecast();
    const uint32_t first = forecast.size() > 0 ? forecast[0].start : 0;
    client.print(F("<script>var CHART={n:"));
    client.print(now);
    client.print(F(",d:"));
    client.print(static_cast<int>(chartDay));
    client.print(F(",c:"));
    if (channels.hasLatestPrice()) {
        client.print(channels.latestPriceEurKwh() * 100.0f, 2);
    } else {
        client.print(F("null"));
    }
    client.print(F(",t0:"));
    client.print(first);
    client.print(F(",t:["));
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (i > 0) client.print(',');
        client.print((forecast[i].start - first) / prices::kSlotSeconds);
    }
    client.print(F("],p:["));
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (i > 0) client.print(',');
        client.print(forecast[i].eurKwh * 100.0f, 2);
    }
    client.print(F("],r:["));
    for (size_t i = 0; i < forecast.size(); ++i) {
        if (i > 0) client.print(',');
        int code = 0;
        if (i < channelPlan.slotCount) {
            const uint8_t mask = channelPlan.mask[i];
            code = static_cast<int>(planner::slotReason(mask)) | (planner::slotOn(mask) ? 16 : 0);
        }
        client.print(code);
    }
    client.print(F("],rn:["));
    for (uint8_t reason = 0; reason <= static_cast<uint8_t>(planner::Reason::Ceiling); ++reason) {
        if (reason > 0) client.print(',');
        client.print('"');
        client.print(planner::reasonName(static_cast<planner::Reason>(reason)));
        client.print('"');
    }
    client.print(F("],ro:["));
    client.print(static_cast<int>(planner::Reason::HolidayOff));
    client.print(',');
    client.print(static_cast<int>(planner::Reason::Ceiling));
    client.print(F("],f:"));
    const uint32_t forceLeftMs = channels.forceOnRemainingMs(channel);
    client.print(now != 0 && forceLeftMs > 0 ? now + (forceLeftMs + 999UL) / 1000UL : 0UL);
    client.print(F("};</script>"));
}

// Отрисовка графика в браузере: SVG по данным CHART. Время — по правилам Риги
// (переходы в последнее воскресенье марта/октября в 01:00 UTC), независимо от часового
// пояса браузера. Сутки 23 и 25 часов рисуются по реальной длине.
static const char kChartScript[] = R"js(<script>(function(){
var C=window.CHART,el=document.getElementById('chart'),tip=document.getElementById('chart-tip');
if(!C||!el)return;
if(!C.n){el.textContent='Время еще не синхронизировано, графика пока нет.';return;}
function p2(v){return(v<10?'0':'')+v;}
function ls(y,m){var d=new Date(Date.UTC(y,m+1,0));return d.getUTCDate()-d.getUTCDay();}
function of(e){var y=new Date(e*1e3).getUTCFullYear(),a=Date.UTC(y,2,ls(y,2),1)/1e3,b=Date.UTC(y,9,ls(y,9),1)/1e3;return e>=a&&e<b?10800:7200;}
function lc(e){return e+of(e);}
function ds(d){var e=d*86400-7200;return lc(e)==d*86400?e:d*86400-10800;}
function hm(e){var s=lc(e)%86400;return p2(Math.floor(s/3600))+':'+p2(Math.floor(s%3600/60));}
function fmt(v){return v.toFixed(2).replace('.',',');}
var S=[];for(var i=0;i<C.t.length;i++)S.push({s:C.t0+C.t[i]*900,p:C.p[i],r:C.r[i]});
var today=Math.floor(lc(C.n)/86400),day=C.d,W=700,H=300,L=56,T=22,PW=626,PH=220;
function draw(){
var a=ds(today+day),b=ds(today+day+1),q=S.filter(function(x){return x.s>=a&&x.s<b;});
var lo=0,hi=10;q.forEach(function(x){lo=Math.min(lo,x.p);hi=Math.max(hi,x.p);});
lo=Math.floor(lo/5)*5;hi=Math.ceil(hi/5)*5;var st=hi-lo>40?10:5,i,j,x,y;
function X(e){return L+(e-a)/(b-a)*PW;}
function Y(v){return T+PH-(v-lo)/(hi-lo)*PH;}
var h='<svg class="price-graph" viewBox="0 0 '+W+' '+H+'" role="img" aria-label="График цены и плана реле">';
for(i=0;i<q.length;i++){if(!(q[i].r&16))continue;j=i;while(j+1<q.length&&(q[j+1].r&16)&&q[j+1].s==q[j].s+900)j++;
h+='<rect class="work-period" x="'+X(q[i].s).toFixed(1)+'" y="'+T+'" width="'+(X(q[j].s+900)-X(q[i].s)+0.4).toFixed(1)+'" height="'+PH+'"/>';i=j;}
if(C.f>C.n&&C.f>a&&C.n<b){x=X(Math.max(C.n,a));h+='<rect class="force-period" x="'+x.toFixed(1)+'" y="'+T+'" width="'+(X(Math.min(C.f,b))-x).toFixed(1)+'" height="'+PH+'"/>';}
for(var v=lo;v<=hi;v+=st){y=Y(v).toFixed(1);h+='<line class="'+(v==0?'zero-line':'grid-line')+'" x1="'+L+'" x2="'+(L+PW)+'" y1="'+y+'" y2="'+y+'"/><text class="axis-label" x="'+(L-8)+'" y="'+(+y+4)+'" text-anchor="end">'+v+'</text>';}
for(var e=a,k=0;e<=b;e+=3600,k++){if(k%2)continue;x=X(e).toFixed(1);
h+='<line class="grid-line" x1="'+x+'" x2="'+x+'" y1="'+T+'" y2="'+(T+PH)+'"/><text class="axis-label" x="'+x+'" y="'+(T+PH+18)+'" text-anchor="middle">'+(e==b?'24:00':hm(e))+'</text>';}
if(q.length){var d='';for(i=0;i<q.length;i++){y=Y(q[i].p).toFixed(1);d+=(i&&q[i].s==q[i-1].s+900?'L':'M')+X(q[i].s).toFixed(1)+','+y+'L'+X(q[i].s+900).toFixed(1)+','+y;}
h+='<path class="price-line" d="'+d+'"/>';}
else h+='<text class="axis-title" x="'+(L+PW/2)+'" y="'+(T+PH/2)+'" text-anchor="middle">Цен на эти сутки пока нет</text>';
if(C.n>=a&&C.n<b){x=X(C.n).toFixed(1);h+='<line class="now-line" x1="'+x+'" x2="'+x+'" y1="'+T+'" y2="'+(T+PH)+'"/><text class="now-label" x="'+x+'" y="14" text-anchor="middle">Сейчас</text>';
if(C.c!==null)h+='<circle class="current-price" cx="'+x+'" cy="'+Y(Math.max(lo,Math.min(hi,C.c))).toFixed(1)+'" r="5"/>';}
h+='<text class="axis-title" x="'+(L+PW/2)+'" y="'+(H-6)+'" text-anchor="middle">Местное время (Europe/Riga)</text>';
h+='<text class="axis-title" transform="translate(14 '+(T+PH/2)+') rotate(-90)" text-anchor="middle">Цена, центов/кВт·ч</text>';
h+='<line id="cur" class="hover-line" y1="'+T+'" y2="'+(T+PH)+'" visibility="hidden"/><rect id="hit" x="'+L+'" y="'+T+'" width="'+PW+'" height="'+PH+'" fill="transparent"/></svg>';
el.innerHTML=h;tip.style.display='none';
var svg=el.firstChild,cur=document.getElementById('cur');
function hide(){tip.style.display='none';cur.setAttribute('visibility','hidden');}
function show(ev){var r=svg.getBoundingClientRect(),pt=ev.touches?ev.touches[0]:ev,sx=(pt.clientX-r.left)/r.width*W,t=a+(sx-L)/PW*(b-a),f=null;
for(var n=0;n<q.length;n++)if(t>=q[n].s&&t<q[n].s+900){f=q[n];break;}
if(!f){hide();return;}
var rs=f.r&15,txt=hm(f.s)+'–'+hm(f.s+900)+' · '+fmt(f.p)+' ц/кВт·ч · ';
if(t>=C.n&&t<C.f)txt+='включено: принудительное включение';
else txt+=f.r&16?'включено: '+C.rn[rs]:(C.ro.indexOf(rs)>=0?'выключено: '+C.rn[rs]:'выключено');
tip.textContent=txt;tip.style.display='block';var cx=X(f.s+450);cur.setAttribute('x1',cx);cur.setAttribute('x2',cx);cur.setAttribute('visibility','visible');
var px=cx/W*r.width-tip.offsetWidth/2;tip.style.left=Math.max(0,Math.min(r.width-tip.offsetWidth,px))+'px';}
var hit=document.getElementById('hit');hit.addEventListener('mousemove',show);hit.addEventListener('mouseleave',hide);hit.addEventListener('touchstart',show,{passive:true});
[].forEach.call(document.querySelectorAll('#days button'),function(bt){bt.className=+bt.getAttribute('data-d')==day?'active':'';});}
[].forEach.call(document.querySelectorAll('#days button'),function(bt){bt.onclick=function(){day=+bt.getAttribute('data-d');draw();};});
draw();})();</script>)js";

// Состояние выбранного реле и кнопки принудительного включения (docs/SCENARIOS.md §6).
// Отдельная форма POST /force: не отправляет и не проверяет поля настроек.
static void printForceOnPanel(Print& client, const controller::Controller& channels,
    uint8_t channel) {
    const uint8_t relay = channel;
    const bool forceOnActive = channels.forceOnActive(relay);
    client.print(F("<form method='POST' action='/force'><input type='hidden' name='relay' value='"));
    client.print(relay + 1);
    client.print(F("'><fieldset class='force-box'><legend>Принудительное включение</legend><p>Реле "));
    client.print(relay + 1);
    client.print(F(" сейчас: <strong>"));
    client.print(channels.outputOn(relay) ? F("включено") : F("выключено"));
    client.print(F("</strong>. Режим: "));
    client.print(channels.modeName(relay));
    client.print(F(".</p>"));
    if (forceOnActive) {
        char remaining[24];
        forceon::formatRemaining(channels.forceOnRemainingMs(relay), remaining,
            sizeof(remaining));
        client.print(F("<p>Включено принудительно, осталось <strong>"));
        client.print(remaining);
        client.print(F("</strong>.</p>"));
    }
    client.print(F("<div class='form-actions'><button type='submit' name='action' value='start'>"));
    client.print(forceOnActive ? F("Перезапустить на ") : F("Включить на "));
    client.print(channels.config(relay).forceOnHours);
    client.print(F(" ч</button><button type='submit' name='action' value='stop' class='secondary'"));
    if (!forceOnActive) client.print(F(" disabled"));
    client.print(F(">Остановить</button></div><small>Реле включается сразу и работает "
        "заданное время, не глядя на цену, аварийный предел, отпуск и наличие интернета, "
        "затем возвращается к автоматике. Ручное включение или выключение отменяет его. "
        "После перезапуска устройства не возобновляется.</small></fieldset></form>"));
}

void sendSettingsPage(arduino::Client& networkClient, controller::Controller& channels,
    const controller::Clock& clock, uint8_t channel, int8_t chartDay, bool wifiFields,
    const String& message) {
    const ChannelConfig& deviceConfig = channels.config(channel);
    String page;
    if (!page.reserve(24000)) {
        Serial.println("[WEB] Недостаточно памяти для формирования страницы настроек.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }
    StringPageWriter client(page);

    client.print(F("<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Opta Settings</title>"
        // Пустая иконка: иначе браузер параллельно просит /favicon.ico, а сервер
        // обслуживает одно соединение за раз
        "<link rel='icon' href='data:,'>"
        "<style>body{font:16px sans-serif;max-width:1100px;margin:2em auto;padding:0 1em;color:#202124}"
        ".settings-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}"
        "fieldset{margin:0;padding:1em;border:1px solid #bbb;border-radius:8px;min-width:0}"
        "input,select,button{box-sizing:border-box;padding:10px;margin:5px 0;max-width:100%}"
        "input[type=number],input[type=datetime-local],select{width:100%}"
        "label{display:block;margin:10px 0}.row{display:flex;gap:12px}"
        ".row label{flex:1}button{background:#1769aa;color:white;border:0;padding:12px 20px}"
        ".price-chart,.form-actions{grid-column:1/-1}.price-graph{display:block;width:100%;height:auto}"
        ".grid-line{stroke:#d9dee5;stroke-width:1}.axis-label{font-size:12px;fill:#4b5563}"
        ".axis-title{font-size:13px;fill:#202124}.price-line{fill:none;stroke:#1769aa;stroke-width:3}"
        ".work-period{fill:#d1d5db;fill-opacity:.7}.force-period{fill:#f9ab00;fill-opacity:.35}"
        "button.secondary{background:#5f6368}button:disabled{opacity:.45}"
        ".force-box{margin:0 0 12px}.force-box p{margin:6px 0}"
        ".now-line{stroke:#d93025;stroke-width:2;stroke-dasharray:5 4}"
        ".now-label{font-size:12px;fill:#d93025}"
        ".current-price{fill:#d93025;stroke:white;stroke-width:2}.chart-note{margin:0;color:#5f6368}"
        ".zero-line{stroke:#9ca3af;stroke-width:1}.hover-line{stroke:#5f6368;stroke-dasharray:3 3}"
        ".chart-box{position:relative;min-height:120px}"
        ".chart-tip{display:none;position:absolute;top:0;max-width:100%;box-sizing:border-box;"
        "background:#202124;color:white;padding:4px 8px;border-radius:4px;font-size:13px;"
        "pointer-events:none}"
        ".day-switch{display:flex;gap:8px;margin:8px 0 12px}.day-switch button{margin:0;padding:8px 14px;"
        "background:white;border:1px solid #1769aa;border-radius:5px;color:#1769aa}"
        ".day-switch button.active{background:#1769aa;color:white}"
        ".nav{display:flex;gap:16px;margin:0 0 16px}.nav a{color:#1769aa}"
        ".form-actions{display:flex;gap:10px;flex-wrap:wrap}"
        "@media(max-width:640px){.settings-grid{grid-template-columns:1fr}"
        ".price-chart,.form-actions{grid-column:auto}}</style>"
        "</head><body><h1>Настройки Opta</h1><p>Устройство: "
        "<a href='http://opta-settings.local'>http://opta-settings.local</a></p>"
        "<nav class='nav'><strong>Настройки</strong>"
        "<a href='/history'>История переключений</a></nav>"
        "<form method='GET' action='/'><label>Настраиваемое реле"
        "<select name='relay' onchange='this.form.submit()'>"));

    for (uint8_t i = 0; i < kChannelCount; ++i) {
        client.print(F("<option value='"));
        client.print(i + 1);
        client.print(F("'"));
        if (i == channel) client.print(F(" selected"));
        client.print(F(">Реле "));
        client.print(i + 1);
        client.print(F(" (D"));
        client.print(i);
        client.print(F(")</option>"));
    }

    client.print(F("</select></label></form>"));
    printForceOnPanel(client, channels, channel);
    client.print(F("<form method='POST' action='/save'><input type='hidden' name='relay' value='"));
    client.print(channel + 1);
    client.print(F("'><div class='settings-grid'><fieldset class='price-chart'><legend>График цены</legend>"
        "<div class='day-switch' id='days'><button type='button' data-d='-1'>Вчера</button>"
        "<button type='button' data-d='0'>Сегодня</button>"
        "<button type='button' data-d='1'>Завтра</button></div>"
        "<div class='chart-box'><div id='chart'><noscript>Для графика нужен JavaScript."
        "</noscript></div><div id='chart-tip' class='chart-tip'></div></div>"
        "<p class='chart-note'>Синяя линия — цены по 15-минутным интервалам. Серым отмечен "
        "план автоматики выбранного реле, оранжевым — оставшееся время принудительного "
        "включения; ручной режим на графике не показывается. "
        "Наведите на график или коснитесь его, чтобы увидеть цену и причину. Вчерашний "
        "план пересчитан по текущим настройкам, фактические переключения — на странице "
        "<a href='/history'>История переключений</a>.</p>"));
    printChartData(client, channels, clock, channel, chartDay);
    client.print(kChartScript);

    client.print(F("</fieldset>"));
    if (wifiFields) {
        client.print(F("<fieldset><legend>Подключение Wi-Fi</legend>"
                       "<label>Имя сети (SSID)<input name='manual_ssid' maxlength='32' required></label>"
                       "<label>Пароль Wi-Fi<input name='password' type='password' maxlength='63' "
                       "autocomplete='new-password'></label></fieldset>"));
    }

    client.print(F("<fieldset><legend>Максимально допустимая цена (EUR/kWh)</legend>"
        "<label>Аварийный предел<input name='safety' type='number' step='0.00001' min='0' max='100' value='"));
    client.print(deviceConfig.safetyCeilingEurKwh, 5);
    client.print(F("'></label><small>Выше этого значения реле всегда выключено.</small></fieldset>"
        "<fieldset><legend>Порог включения (EUR/kWh)</legend>"
        "<label>Порог включения<input name='threshold' type='number' step='0.00001' min='-100' max='100' value='"));
    client.print(deviceConfig.alwaysOnBelowPriceEurKwh, 5);
    client.print(F("'></label><small>Постоянно включен, когда цена ниже этого порога.</small></fieldset>"
        "<fieldset><legend>Умный порог включения</legend><label><input type='checkbox' name='smart_enabled' value='1'"));
    if (deviceConfig.smartThresholdEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Количество самых дешевых часов за день"
        "<input name='smart_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.smartThresholdHours);
    client.print(F("'></label><small>N часов — это 4·N самых дешевых 15-минутных интервалов "
        "текущих суток, не обязательно подряд.</small></fieldset>"
        "<fieldset><legend>Включение перед пробуждением</legend>"
        "<label><input type='checkbox' name='wakeup_enabled' value='1'"));
    if (deviceConfig.wakeupEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Количество часов до пробуждения"
        "<input name='wakeup_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.wakeupHours);
    client.print(F("'></label><label>Время пробуждения"
        "<input name='wakeup_time' type='time' value='"));
    printTimeOfDay(client, deviceConfig.wakeupTimeMinuteOfDay);
    client.print(F("'></label><small>Реле включено ровно N часов до этого времени, "
        "без учета цены.</small></fieldset>"
        "<fieldset><legend>Включение ночью</legend><label><input type='checkbox' name='night_enabled' value='1'"));
    if (deviceConfig.nightPrepEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><div class='row'><label>Начало окна"
        "<input name='night_start' type='time' value='"));
    printTimeOfDay(client, deviceConfig.nightStartMinuteOfDay);
    client.print(F("'></label><label>Конец окна<input name='night_end' type='time' value='"));
    printTimeOfDay(client, deviceConfig.nightEndMinuteOfDay);
    client.print(F("'></label></div><label>Количество самых дешевых часов<input name='night_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.nightCheapHours);
    client.print(F("'></label><small>4·N самых дешевых 15-минутных интервалов окна. "
        "Если интервал ближе к концу окна дороже не больше чем на 1 цент/кВт·ч, "
        "выбирается он.</small></fieldset>"
        "<fieldset><legend>Принудительное включение</legend><label>Длительность, часов"
        "<input name='force_hours' type='number' min='1' max='24' value='"));
    client.print(deviceConfig.forceOnHours);
    client.print(F("'></label><small>На сколько часов кнопка «Включить» над настройками "
        "включает реле. Уже идущее включение не меняется.</small></fieldset>"
        "<fieldset><legend>Календарь / отпуск</legend><label><input type='checkbox' name='holiday_enabled' value='1'"));
    if (deviceConfig.holidayEnabled) client.print(F(" checked"));
    client.print(F("> Включить сценарий</label><label>Дата и время возвращения (Europe/Riga)"
        "<input name='return_local' type='datetime-local' value='"));
    if (deviceConfig.holidayReturnEpoch != 0) {
        char returnText[17];
        rigatime::formatLocalDateTime(deviceConfig.holidayReturnEpoch, returnText, sizeof(returnText));
        client.print(returnText);
    }
    client.print(F("'></label><label>Часы работы перед возвращением"
        "<input name='holiday_hours' type='number' min='1' max='10' value='"));
    client.print(deviceConfig.holidayOnHours);
    client.print(F("'></label><small>Реле работает в N·4 самых дешевых 15-минутных "
        "интервалах последних 10 часов перед возвращением, ближе к возвращению при разнице "
        "до 1 цента/кВт·ч. До этого окна реле выключено. "
        "Приоритет: ручной режим и принудительное включение, аварийный предел, отпуск, "
        "порог, умный порог, пробуждение, включение ночью.</small></fieldset>"
        "<div class='form-actions'><button type='submit'>Сохранить настройки</button>"
        "<button type='submit' name='action' value='save_restart'>Сохранить и перезапустить</button></div>"
        "</div></form>"));

    if (message.length() > 0) {
        client.print(F("<p><strong>"));
        client.print(htmlEscape(message.c_str()));
        client.print(F("</strong></p>"));
    }
    client.println(F("</body></html>"));

    if (client.hasFailed()) {
        Serial.println("[WEB] Не удалось сформировать полную страницу настроек.");
        networkClient.println(F("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 0\r\n"));
        return;
    }

    networkClient.println(F("HTTP/1.1 200 OK"));
    networkClient.println(F("Content-Type: text/html; charset=utf-8"));
    networkClient.println(F("Cache-Control: no-store"));
    networkClient.println(F("Connection: close"));
    networkClient.print(F("Content-Length: "));
    networkClient.println(page.length());
    networkClient.println();
    writeClientData(networkClient,
        reinterpret_cast<const uint8_t*>(page.c_str()), page.length());
}
