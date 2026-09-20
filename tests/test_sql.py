#!/usr/bin/env python3
# Mirrors the v4 storage layout: two tables per domain (raw detail + 5-minute
# archive) with scaled-INTEGER metric columns (fixed-point, see database.cpp).
# Per-core CPU lives in its own domain (core count varies per machine).
import sqlite3, time
con = sqlite3.connect(':memory:')
system_cols = ('ts INTEGER PRIMARY KEY,cpu_usage INTEGER,cpu_temp INTEGER,load1 INTEGER,mem_used INTEGER,mem_total INTEGER,'
 'swap_used INTEGER,swap_total INTEGER,net_rx INTEGER,net_tx INTEGER,disk_read INTEGER,disk_write INTEGER,'
 'disk_used INTEGER,disk_total INTEGER,battery_percent INTEGER,battery_power INTEGER,battery_health INTEGER,battery_status TEXT')
gpu_cols = ('ts INTEGER NOT NULL,id TEXT NOT NULL,vendor TEXT,name TEXT,driver TEXT,state TEXT,util INTEGER,mem_used INTEGER,'
 'mem_total INTEGER,temp INTEGER,power INTEGER,freq INTEGER,PRIMARY KEY(ts,id)')
cpu_cols = 'ts INTEGER NOT NULL,core INTEGER NOT NULL,util INTEGER,PRIMARY KEY(ts,core)'
for t in ('system_raw','system_5m'): con.execute(f'CREATE TABLE {t} ({system_cols})')
for t in ('gpu_raw','gpu_5m'): con.execute(f'CREATE TABLE {t} ({gpu_cols})')
for t in ('cpu_cores_raw','cpu_cores_5m'): con.execute(f'CREATE TABLE {t} ({cpu_cols})')
now = int(time.time())
open5 = (now // 300) * 300
for i in range(70):
    ts = open5 - 3600 + i * 60  # all inside completed 5-minute buckets
    con.execute('INSERT OR REPLACE INTO system_raw VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
      (ts, 200 + i % 100, 520, 100, 4096, 32768, 0, 0, 1200, 200, 3000, 1000, 12000, 50000, 800, -1200, 950, 'Discharging'))
    if i % 2 == 0:
        con.execute('INSERT OR REPLACE INTO gpu_raw VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
          (ts, 'intel:card0', 'Intel', 'Intel GPU', 'i915', 'active', 400, None, None, 550, 220, 700))
    for core, util in ((0, 200 + i % 100), (1, 300 + i % 100)):
        con.execute('INSERT OR REPLACE INTO cpu_cores_raw VALUES(?,?,?)', (ts, core, util))
con.execute(f'''INSERT OR REPLACE INTO system_5m SELECT (ts/300)*300,CAST(ROUND(AVG(cpu_usage)) AS INTEGER),CAST(ROUND(AVG(cpu_temp)) AS INTEGER),CAST(ROUND(AVG(load1)) AS INTEGER),CAST(ROUND(AVG(mem_used)) AS INTEGER),CAST(ROUND(AVG(mem_total)) AS INTEGER),CAST(ROUND(AVG(swap_used)) AS INTEGER),CAST(ROUND(AVG(swap_total)) AS INTEGER),CAST(ROUND(AVG(net_rx)) AS INTEGER),CAST(ROUND(AVG(net_tx)) AS INTEGER),CAST(ROUND(AVG(disk_read)) AS INTEGER),CAST(ROUND(AVG(disk_write)) AS INTEGER),CAST(ROUND(AVG(disk_used)) AS INTEGER),CAST(ROUND(AVG(disk_total)) AS INTEGER),CAST(ROUND(AVG(battery_percent)) AS INTEGER),CAST(ROUND(AVG(battery_power)) AS INTEGER),CAST(ROUND(AVG(battery_health)) AS INTEGER),MAX(battery_status) FROM system_raw WHERE ts < {open5} GROUP BY (ts/300)''')
con.execute(f'''INSERT OR REPLACE INTO gpu_5m SELECT (ts/300)*300,id,MAX(vendor),MAX(name),MAX(driver),MAX(state),CAST(ROUND(AVG(util)) AS INTEGER),CAST(ROUND(AVG(mem_used)) AS INTEGER),CAST(ROUND(AVG(mem_total)) AS INTEGER),CAST(ROUND(AVG(temp)) AS INTEGER),CAST(ROUND(AVG(power)) AS INTEGER),CAST(ROUND(AVG(freq)) AS INTEGER) FROM gpu_raw WHERE ts < {open5} GROUP BY (ts/300),id''')
con.execute(f'''INSERT OR REPLACE INTO cpu_cores_5m SELECT (ts/300)*300,core,CAST(ROUND(AVG(util)) AS INTEGER) FROM cpu_cores_raw WHERE ts < {open5} GROUP BY (ts/300),core''')
assert con.execute('SELECT COUNT(*) FROM system_5m').fetchone()[0] > 0
assert con.execute('SELECT COUNT(*) FROM gpu_5m').fetchone()[0] > 0
assert con.execute('SELECT COUNT(*) FROM cpu_cores_5m').fetchone()[0] > 0
# Scaled integers on disk: typeof() must be integer, not real.
assert con.execute("SELECT DISTINCT typeof(cpu_usage) FROM system_5m").fetchall() == [('integer',)]
assert con.execute("SELECT DISTINCT typeof(util) FROM gpu_5m").fetchall() == [('integer',)]
assert con.execute("SELECT DISTINCT typeof(util) FROM cpu_cores_5m").fetchall() == [('integer',)]
# Spot-check one bucket average: cpu 20.0..29.x% range stored x10.
row = con.execute('SELECT (ts/300)*300 b, AVG(cpu_usage)/10.0 FROM system_raw GROUP BY b ORDER BY b LIMIT 1').fetchone()
arch = con.execute('SELECT AVG(cpu_usage)/10.0 FROM system_5m WHERE ts=?', (row[0],)).fetchone()
assert abs(row[1] - arch[0]) < 0.06, (row, arch)
print('SQLite schema/downsampling/history: PASS')
