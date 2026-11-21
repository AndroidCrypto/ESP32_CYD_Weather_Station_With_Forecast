// =====================================================================================
//  HA_Template.h  -  Jinja template rendered by Home Assistant (POST /api/template)
//
//  Only used when HA_ENABLED is true. One request returns everything as compact JSON.
//  Replace the example entity IDs with your own. Sensors that do not exist or are
//  unavailable are returned as null and simply shown as "--" on the display.
//
//  Expected inputs:
//    tide extremes : a sensor with attribute "predictions": [{time, height, type: high|low}, ...]
//                    (e.g. built from a tide API with a REST / template sensor)
//    others        : plain numeric sensors
//  Test the template in HA: Developer tools -> Template.
// =====================================================================================
#pragma once

static const char HA_TEMPLATE[] = R"HAT({%- macro n(e, d=1) -%}{%- set v = states(e) -%}{{ (v | float) | round(d) if is_number(v) else 'null' }}{%- endmacro -%}
{%- set nowts = as_timestamp(now()) -%}
{%- set ns = namespace(prev=none, out=[]) -%}
{%- for e in (state_attr('sensor.tide_extremes','predictions') or []) -%}
{%- set ts = as_timestamp(e.time, 0) -%}
{%- if ts <= nowts -%}{%- set ns.prev = e -%}{%- elif ns.out | length < 7 -%}{%- set ns.out = ns.out + [e] -%}{%- endif -%}
{%- endfor -%}
{%- set tl = ([ns.prev] if ns.prev else []) + ns.out -%}
{%- set lc = states.sensor.blitzortung_lightning_counter -%}
{"t":{{ nowts | int }},"tide":[{% for e in tl %}[{{ as_timestamp(e.time) | int }},{{ e.height }},{{ 1 if e.type == 'high' else 0 }}]{{ ',' if not loop.last }}{% endfor %}],"wave":{{ n('sensor.wave_height', 2) }},"sst":{{ n('sensor.sea_surface_temperature') }},"out_t":{{ n('sensor.outdoor_temperature') }},"out_h":{{ n('sensor.outdoor_humidity', 0) }},"out_f":{{ n('sensor.outdoor_feels_like') }},"aqi":{{ n('sensor.outdoor_aqi', 0) }},"pm25":{{ n('sensor.outdoor_pm2_5', 0) }},"co2":{{ n('sensor.indoor_co2', 0) }},"in_t":{{ n('sensor.indoor_temperature') }},"in_h":{{ n('sensor.indoor_humidity', 0) }},"in_pm":{{ n('sensor.indoor_pm2_5', 1) }},"p3h":{{ n('sensor.pressure_change_3h') }},"l_cnt":{{ n('sensor.blitzortung_lightning_counter', 0) }},"l_dist":{{ n('sensor.blitzortung_lightning_distance', 0) }},"l_az":{{ n('sensor.blitzortung_lightning_azimuth', 0) }},"l_age":{{ ((nowts - as_timestamp(lc.last_changed)) / 60) | int if lc else 'null' }},"moon":{{ n('sensor.moon_illumination', 0) }},"cap":{{ n('sensor.weather_alert_count', 0) }},"tmd":{{ states('sensor.weather_alert_text') | tojson }},"nowcast":{{ states('sensor.pirateweather_minutely_summary') | tojson }}})HAT";
