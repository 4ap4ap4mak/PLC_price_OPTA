#!/usr/bin/env bash
# Панель Home Assistant для Opta (docs/HOME_ASSISTANT.md §9): вкладка «Обзор» и по
# вкладке на реле с состоянием, графиком и настройками сценариев.
#
#   scripts/ha-dashboard.sh [id-платы] > панель.yaml
#
# Без аргумента — id-заглушка opta_xxxxxx; так собран файл в репозитории:
#   scripts/ha-dashboard.sh > homeassistant/opta-dashboard.yaml
# Id платы — строка «[MQTT] … устройство opta_…» в логе или карточка устройства в HA.
# Шаблоны ниже — YAML с подстановками @ID@ (id платы) и @N@ (номер реле).
set -euo pipefail

id=${1:-opta_xxxxxx}
[[ $id =~ ^opta_[0-9a-z]+$ ]] || { echo "ha-dashboard: id должен быть opta_…: $id" >&2; exit 1; }
relays=4  # kChannelCount

fill() { sed -e "s/@ID@/$id/g" -e "s/@N@/${1:-}/g"; }

fill <<'EOF'
# Панель Home Assistant для Arduino Opta (PLC_price_OPTA), id платы: @ID@.
# Собрана scripts/ha-dashboard.sh — правьте скрипт, а не этот файл.
# Установка — docs/HOME_ASSISTANT.md §9: новая панель → «Редактор кода» → вставить
# весь файл. Графики — карточка apexcharts-card из HACS (§8); без нее на месте
# графиков ошибка, остальная панель работает.
title: Opta
views:
  - title: Обзор
    path: overview
    icon: mdi:view-dashboard
    type: sections
    max_columns: 3
    sections:
      - type: grid
        column_span: 2
        cards:
          - type: heading
            heading: Цена
            icon: mdi:cash
          - type: tile
            entity: sensor.@ID@_price
            name: Цена сейчас
            grid_options:
              columns: full
          # apexcharts-card строго проверяет свои ключи: grid_options ей не передавать
          - type: custom:apexcharts-card
            graph_span: 2d
            span:
              start: day
            now:
              show: true
              label: сейчас
            yaxis:
              - decimals: 3
            series:
              - entity: sensor.@ID@_price
                name: Цена, EUR/kWh
                type: line
                curve: stepline
                extend_to: false
                data_generator: |
                  return (entity.attributes.d || []).map(p => [p[0] * 1000, p[1]]);
EOF

for n in $(seq 1 "$relays"); do
    fill "$n" <<'EOF'
      - type: grid
        cards:
          - type: heading
            heading: Реле @N@
            icon: mdi:electric-switch
            tap_action:
              action: navigate
              navigation_path: relay@N@
          - type: tile
            entity: binary_sensor.@ID@_relay@N@_output
            name: Выход
          - type: tile
            entity: select.@ID@_relay@N@_mode
            name: Режим
            features_position: bottom
            features:
              - type: select-options
          - type: markdown
            content: "{{ states('sensor.@ID@_relay@N@_reason') }}"
EOF
done

fill <<'EOF'
      - type: grid
        cards:
          - type: heading
            heading: Прошивка
            icon: mdi:chip
          - type: tile
            entity: update.@ID@_firmware
            name: Прошивка
          - type: tile
            entity: button.@ID@_firmware_check
            name: Проверить обновление
            tap_action:
              action: perform-action
              perform_action: button.press
              target:
                entity_id: button.@ID@_firmware_check
EOF

for n in $(seq 1 "$relays"); do
    fill "$n" <<'EOF'
  - title: Реле @N@
    path: relay@N@
    icon: mdi:numeric-@N@-box-outline
    type: sections
    max_columns: 3
    sections:
      - type: grid
        cards:
          - type: heading
            heading: Состояние
            icon: mdi:electric-switch
          - type: tile
            entity: binary_sensor.@ID@_relay@N@_output
            name: Выход
          - type: markdown
            content: "{{ states('sensor.@ID@_relay@N@_reason') }}"
          - type: tile
            entity: select.@ID@_relay@N@_mode
            name: Ручной режим
            grid_options:
              columns: full
            features_position: bottom
            features:
              - type: select-options
          - type: heading
            heading: Принудительное включение
            heading_style: subtitle
          - type: tile
            entity: button.@ID@_relay@N@_force
            name: Включить
            icon: mdi:timer-play
            tap_action:
              action: perform-action
              perform_action: button.press
              target:
                entity_id: button.@ID@_relay@N@_force
              confirmation:
                text: Включить реле @N@ принудительно на заданное число часов?
          - type: tile
            entity: button.@ID@_relay@N@_stop
            name: Остановить
            icon: mdi:timer-stop
            tap_action:
              action: perform-action
              perform_action: button.press
              target:
                entity_id: button.@ID@_relay@N@_stop
          - type: entities
            entities:
              - entity: number.@ID@_relay@N@_force_h
                name: Длительность
      - type: grid
        column_span: 2
        cards:
          - type: heading
            heading: Цена и план
            icon: mdi:chart-timeline-variant
          # apexcharts-card строго проверяет свои ключи: grid_options ей не передавать
          - type: custom:apexcharts-card
            graph_span: 2d
            span:
              start: day
            now:
              show: true
              label: сейчас
            yaxis:
              - id: price
                decimals: 3
              - id: plan
                show: false
                min: 0
                max: 1
            series:
              - entity: sensor.@ID@_price
                name: Цена, EUR/kWh
                yaxis_id: price
                type: line
                curve: stepline
                extend_to: false
                data_generator: |
                  return (entity.attributes.d || []).map(p => [p[0] * 1000, p[1]]);
              - entity: binary_sensor.@ID@_relay@N@_output
                name: План реле @N@
                yaxis_id: plan
                type: area
                curve: straight
                color: '#9aa0a6'
                opacity: 0.4
                extend_to: false
                data_generator: |
                  const points = [];
                  for (const [start, end] of entity.attributes.on || []) {
                    points.push([start * 1000, 0], [start * 1000, 1], [end * 1000, 1], [end * 1000, 0]);
                  }
                  return points;
      - type: grid
        cards:
          - type: heading
            heading: Цена
            icon: mdi:cash
          - type: entities
            entities:
              - entity: number.@ID@_relay@N@_safety
                name: Аварийный предел (цена выше — выключено)
              - entity: number.@ID@_relay@N@_threshold
                name: Порог включения (цена ≤ порога)
          - type: heading
            heading: Умный порог
            icon: mdi:sort-numeric-ascending
          - type: entities
            entities:
              - entity: switch.@ID@_relay@N@_smart
                name: Самые дешевые часы суток
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_smart
                    state: "on"
                row:
                  entity: number.@ID@_relay@N@_smart_h
                  name: Часов
      - type: grid
        cards:
          - type: heading
            heading: Пробуждение
            icon: mdi:weather-sunset-up
          - type: entities
            entities:
              - entity: switch.@ID@_relay@N@_wake
                name: Включение перед пробуждением
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_wake
                    state: "on"
                row:
                  entity: text.@ID@_relay@N@_wake_t
                  name: Время (ЧЧ:ММ)
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_wake
                    state: "on"
                row:
                  entity: number.@ID@_relay@N@_wake_h
                  name: Часов до пробуждения
          - type: heading
            heading: Ночь
            icon: mdi:weather-night
          - type: entities
            entities:
              - entity: switch.@ID@_relay@N@_night
                name: Включение ночью
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_night
                    state: "on"
                row:
                  entity: text.@ID@_relay@N@_night_s
                  name: Начало (ЧЧ:ММ)
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_night
                    state: "on"
                row:
                  entity: text.@ID@_relay@N@_night_e
                  name: Конец (ЧЧ:ММ)
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_night
                    state: "on"
                row:
                  entity: number.@ID@_relay@N@_night_h
                  name: Самых дешевых часов
      - type: grid
        cards:
          - type: heading
            heading: Отпуск
            icon: mdi:bag-suitcase
          - type: entities
            entities:
              - entity: text.@ID@_relay@N@_hol_ret
                name: Возвращение (ГГГГ-ММ-ДД ЧЧ:ММ)
              - entity: switch.@ID@_relay@N@_hol
                name: Режим отпуска
              - type: conditional
                conditions:
                  - condition: state
                    entity: switch.@ID@_relay@N@_hol
                    state: "on"
                row:
                  entity: number.@ID@_relay@N@_hol_h
                  name: Часов в последние 10 ч перед возвращением
EOF
done
