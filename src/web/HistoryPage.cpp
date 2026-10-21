#include "HistoryPage.h"

#include "ClientWriter.h"

void sendHistoryPage(arduino::Client& networkClient, const history::SwitchHistory& switchHistory) {
    BufferedClientWriter client(networkClient);
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
        "<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Opta History</title><link rel='icon' href='data:,'>"
        "<style>body{font:16px sans-serif;max-width:1100px;margin:2em auto;padding:0 1em;color:#202124}"
        ".nav{display:flex;gap:16px;margin:0 0 16px}.nav a{color:#1769aa}"
        ".table-wrap{overflow-x:auto}table{border-collapse:collapse;width:100%}"
        "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid #d9dee5;white-space:nowrap}"
        "td.cause{white-space:normal}.on{color:#137333;font-weight:bold}.off{color:#5f6368}"
        "tr.device td{background:#f1f3f4;font-weight:bold}</style></head><body>"
        "<h1>История переключений</h1>"
        "<nav class='nav'><a href='/'>Настройки</a><strong>История переключений</strong></nav>"
        "<p>История хранится в оперативной памяти до перезапуска или пропадания питания, "
        "последние "));
    client.print(static_cast<unsigned>(history::SwitchHistory::kCapacity));
    client.print(F(" событий. Записано: "));
    client.print(static_cast<unsigned>(switchHistory.size()));
    if (switchHistory.dropped() > 0) {
        client.print(F(", самые старые вытеснены: "));
        client.print(switchHistory.dropped());
    }
    client.print(F(". Новые сверху.</p><div class='table-wrap'><table>"
        "<tr><th>Время (Рига)</th><th>Реле</th><th>Состояние</th><th>Причина</th></tr>"));

    char timeText[32];
    for (size_t i = 0; i < switchHistory.size(); ++i) {
        const history::SwitchEvent& event = switchHistory.newest(i);
        history::formatEventTime(event, timeText, sizeof(timeText));
        if (event.channel == history::kDeviceEvent) {
            client.print(F("<tr class='device'><td>"));
            client.print(timeText);
            client.print(F("</td><td colspan='3'>"));
            client.print(history::causeName(event.cause));
            client.print(F("</td></tr>"));
            continue;
        }
        client.print(F("<tr><td>"));
        client.print(timeText);
        client.print(F("</td><td>Реле "));
        client.print(event.channel + 1);
        client.print(event.on ? F("</td><td class='on'>ВКЛ") : F("</td><td class='off'>ВЫКЛ"));
        client.print(F("</td><td class='cause'>"));
        client.print(history::causeName(event.cause));
        client.print(F("</td></tr>"));
    }
    client.print(F("</table></div></body></html>"));
    if (!client.finish()) {
        Serial.println("[WEB] Страница истории передана не полностью.");
    }
}
