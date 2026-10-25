"""Erzeugt include/fahrplan.h aus dem GTFS Fahrplan der Wiener Linien.

Das Display zeigt diese Zeiten, wenn für eine Linie keine Live Daten kommen.
Nach jedem Fahrplanwechsel (meist im Dezember) neu ausführen:

    python3 tools/fahrplan.py

Lädt dafür https://www.wienerlinien.at/ogd_realtime/doku/ogd/gtfs/gtfs.zip (ca. 100 MB).
Mit einem Pfad als Argument wird stattdessen eine bereits geladene gtfs.zip verwendet.
"""

import csv
import datetime
import io
import os
import sys
import tempfile
import urllib.request
import zipfile

GTFS_URL = "https://www.wienerlinien.at/ogd_realtime/doku/ogd/gtfs/gtfs.zip"
OUT = os.path.join(os.path.dirname(__file__), "..", "include", "fahrplan.h")

# Ein Eintrag pro Steig und Linie, passend zu den Blöcken in src/main.cpp.
# (Name im Code, Linie, GTFS stop_id des Steigs, erlaubte Ziele laut trip_headsign)
SOURCES = [
    ("FP_U2_MUSEUMSQUARTIER", "U2", "at:49:835:0:3", ["Seestadt", "Aspernstraße"]),
    ("FP_57A_KOESTLERGASSE", "57A", "at:49:1985:0:1", ["Rudolfsheim, Anschützgasse"]),
    ("FP_U4_KETTENBRUECKENGASSE_HUETTELDORF", "U4", "at:49:670:0:1", ["Hütteldorf"]),
    ("FP_U4_KETTENBRUECKENGASSE_HEILIGENSTADT", "U4", "at:49:670:0:4",
     ["Heiligenstadt", "Mitte-Landstraße"]),
    ("FP_U3_NEUBAUGASSE_OTTAKRING", "U3", "at:49:56:0:5", ["Ottakring", "Westbahnhof"]),
    ("FP_U3_NEUBAUGASSE_SIMMERING", "U3", "at:49:56:0:7", ["Simmering"]),
    ("FP_13A_HAUS_DES_MEERES", "13A", "at:49:662:0:2", ["Hauptbahnhof"]),
    ("FP_14A_HAUS_DES_MEERES", "14A", "at:49:662:0:2", ["Reumannplatz"]),
]

WEEKDAYS = ["monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"]


def read_csv(zf, name):
    with zf.open(name) as f:
        yield from csv.DictReader(io.TextIOWrapper(f, encoding="utf-8-sig"))


def headsign(text):
    return text[5:] if text.startswith("Wien ") else text


def main():
    if len(sys.argv) > 1:
        zip_path = sys.argv[1]
    else:
        zip_path = os.path.join(tempfile.gettempdir(), "wienerlinien-gtfs.zip")
        print("Lade GTFS Fahrplan ...")
        urllib.request.urlretrieve(GTFS_URL, zip_path)

    zf = zipfile.ZipFile(zip_path)
    lines = {line for _, line, _, _ in SOURCES}
    stops = {stop for _, _, stop, _ in SOURCES}

    routes = {r["route_id"] for r in read_csv(zf, "routes.txt") if r["route_short_name"] in lines}
    route_line = {r["route_id"]: r["route_short_name"]
                  for r in read_csv(zf, "routes.txt") if r["route_id"] in routes}
    trips = {}
    for r in read_csv(zf, "trips.txt"):
        if r["route_id"] in routes:
            trips[r["trip_id"]] = (route_line[r["route_id"]], r["service_id"],
                                   headsign(r["trip_headsign"]))

    calendar = {r["service_id"]: r for r in read_csv(zf, "calendar.txt")}
    exceptions = {}
    for r in read_csv(zf, "calendar_dates.txt"):
        exceptions.setdefault(r["service_id"], {})[r["date"]] = r["exception_type"]

    def active(service, day):
        ds = day.strftime("%Y%m%d")
        e = exceptions.get(service, {}).get(ds)
        if e:
            return e == "1"
        c = calendar.get(service)
        return bool(c) and c["start_date"] <= ds <= c["end_date"] and c[WEEKDAYS[day.weekday()]] == "1"

    # Abfahrten pro Steig: (service_id, Minuten seit Mitternacht des Betriebstags, Ziel)
    print("Lese Abfahrtszeiten ...")
    deps = {name: [] for name, _, _, _ in SOURCES}
    for r in read_csv(zf, "stop_times.txt"):
        if r["stop_id"] not in stops or r["trip_id"] not in trips:
            continue
        line, service, head = trips[r["trip_id"]]
        for name, src_line, src_stop, heads in SOURCES:
            if src_line == line and src_stop == r["stop_id"] and head in heads:
                h, m, _ = map(int, r["departure_time"].split(":"))
                deps[name].append((service, h * 60 + m, head))

    first = min(datetime.datetime.strptime(c["start_date"], "%Y%m%d").date() for c in calendar.values())
    last = max(datetime.datetime.strptime(c["end_date"], "%Y%m%d").date() for c in calendar.values())
    first = max(first, datetime.date.today() - datetime.timedelta(days=1))
    days = [first + datetime.timedelta(days=i) for i in range((last - first).days + 1)]

    out = []
    out.append("// Automatisch erzeugt von tools/fahrplan.py, nicht von Hand ändern")
    out.append(f"// Fahrplan gültig von {first} bis {last}")
    out.append("#pragma once")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("// Tage seit 1.1.1970 des ersten Tages in der Tabelle")
    out.append(f"const int32_t FP_FIRST_DAY = {(first - datetime.date(1970, 1, 1)).days};")
    out.append(f"const int FP_DAY_COUNT = {len(days)};")
    out.append("")
    out.append("// Pro Steig: Tagesfahrpläne (Minuten seit Mitternacht, über 1440 = nach Mitternacht,")
    out.append("// obere 4 Bit = Index in heads) und welcher Tagesfahrplan an welchem Tag gilt")
    out.append("struct FpSource {")
    out.append("  const char *const *heads;")
    out.append("  const uint16_t *const *patterns;")
    out.append("  const uint16_t *patternSizes;")
    out.append("  const uint8_t *dayPattern;")
    out.append("};")
    out.append("")

    enum = []
    for name, _, _, heads in SOURCES:
        patterns = []
        day_pattern = []
        for day in days:
            times = sorted({(t, heads.index(h)) for s, t, h in deps[name] if active(s, day)})
            packed = tuple(t | (hi << 12) for t, hi in times)
            if packed not in patterns:
                patterns.append(packed)
            day_pattern.append(patterns.index(packed))
        total = sum(len(p) for p in patterns)
        print(f"{name}: {len(patterns)} Tagesfahrpläne, {total} Zeiten")

        low = name.lower()
        quoted = ", ".join('"' + h + '"' for h in heads)
        out.append(f"const char *const {low}_heads[] = {{{quoted}}};")
        for i, p in enumerate(patterns):
            values = ", ".join(str(v) for v in p) or "0"
            out.append(f"const uint16_t {low}_p{i}[] = {{{values}}};")
        out.append(f"const uint16_t *const {low}_patterns[] = "
                   f"{{{', '.join(f'{low}_p{i}' for i in range(len(patterns)))}}};")
        out.append(f"const uint16_t {low}_sizes[] = {{{', '.join(str(len(p)) for p in patterns)}}};")
        out.append(f"const uint8_t {low}_days[] = {{{', '.join(str(d) for d in day_pattern)}}};")
        out.append("")
        enum.append((name, low))

    out.append("const FpSource FP_SOURCES[] = {")
    for name, low in enum:
        out.append(f"    {{{low}_heads, {low}_patterns, {low}_sizes, {low}_days}},")
    out.append("};")
    out.append("")
    out.append("enum FpSourceId {")
    for name, _ in enum:
        out.append(f"  {name},")
    out.append("};")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print(f"Geschrieben: {os.path.normpath(OUT)} (gültig bis {last})")


if __name__ == "__main__":
    main()
