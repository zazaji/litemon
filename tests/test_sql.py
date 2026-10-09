#!/usr/bin/env python3
# Mirrors the v9 storage layout: two tables per domain (raw detail + 5-minute
# archive) with scaled-INTEGER metric columns (fixed-point, see database.cpp).
# Per-core CPU lives in its own domain (core count varies per machine).
# v8: memory/swap/disk capacities are no longer persisted (they never change);
# only usage percentages are stored, scaled x10.
# v9: GPU VRAM follows the same rule — gpu_raw/gpu_5m keep mem_pct (x10)
# instead of absolute mem_used/mem_total MiB.
import sqlite3, time
con = sqlite3.connect(':memory:')
system_cols = ('ts INTEGER PRIMARY KEY,cpu_usage INTEGER,cpu_temp INTEGER,load1 INTEGER,mem_pct INTEGER,'
 'swap_pct INTEGER,net_rx INTEGER,net_tx INTEGER,disk_read INTEGER,disk_write INTEGER,'
 'battery_percent INTEGER,battery_power INTEGER,battery_health INTEGER,battery_status TEXT,'
 'psi_cpu INTEGER,psi_mem INTEGER,psi_io INTEGER,nvme_temp INTEGER,bat_temp INTEGER')
gpu_cols = ('ts INTEGER NOT NULL,id TEXT NOT NULL,vendor TEXT,name TEXT,driver TEXT,state TEXT,util INTEGER,mem_pct INTEGER,'
 'temp INTEGER,power INTEGER,freq INTEGER,PRIMARY KEY(ts,id)')
cpu_cols = 'ts INTEGER NOT NULL,core INTEGER NOT NULL,util INTEGER,PRIMARY KEY(ts,core)'
disk_cols = 'ts INTEGER NOT NULL,mount TEXT NOT NULL,used_pct INTEGER,PRIMARY KEY(ts,mount)'
for t in ('system_raw','system_5m'): con.execute(f'CREATE TABLE {t} ({system_cols})')
for t in ('gpu_raw','gpu_5m'): con.execute(f'CREATE TABLE {t} ({gpu_cols})')
for t in ('cpu_cores_raw','cpu_cores_5m'): con.execute(f'CREATE TABLE {t} ({cpu_cols})')
for t in ('disks_raw','disks_5m'): con.execute(f'CREATE TABLE {t} ({disk_cols})')
now = int(time.time())
open5 = (now // 300) * 300
for i in range(70):
    ts = open5 - 3600 + i * 60  # all inside completed 5-minute buckets
    con.execute('INSERT OR REPLACE INTO system_raw VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)',
      (ts, 200 + i % 100, 520, 100, 4096, 0, 0, 1200, 200, 3000, 800, -1200, 950, 'Discharging', None, None, None, None, None))
    if i % 2 == 0:
        con.execute('INSERT OR REPLACE INTO gpu_raw VALUES(?,?,?,?,?,?,?,?,?,?,?)',
          (ts, 'intel:card0', 'Intel', 'Intel GPU', 'i915', 'active', 400, None, 550, 220, 700))
    con.execute('INSERT OR REPLACE INTO disks_raw VALUES(?,?,?)', (ts, '/', 12000 + i))
    for core, util in ((0, 200 + i % 100), (1, 300 + i % 100)):
        con.execute('INSERT OR REPLACE INTO cpu_cores_raw VALUES(?,?,?)', (ts, core, util))
con.execute(f'''INSERT OR REPLACE INTO system_5m SELECT (ts/300)*300,CAST(ROUND(AVG(cpu_usage)) AS INTEGER),CAST(ROUND(AVG(cpu_temp)) AS INTEGER),CAST(ROUND(AVG(load1)) AS INTEGER),CAST(ROUND(AVG(mem_pct)) AS INTEGER),CAST(ROUND(AVG(swap_pct)) AS INTEGER),CAST(ROUND(AVG(net_rx)) AS INTEGER),CAST(ROUND(AVG(net_tx)) AS INTEGER),CAST(ROUND(AVG(disk_read)) AS INTEGER),CAST(ROUND(AVG(disk_write)) AS INTEGER),CAST(ROUND(AVG(battery_percent)) AS INTEGER),CAST(ROUND(AVG(battery_power)) AS INTEGER),CAST(ROUND(AVG(battery_health)) AS INTEGER),MAX(battery_status),CAST(ROUND(AVG(psi_cpu)) AS INTEGER),CAST(ROUND(AVG(psi_mem)) AS INTEGER),CAST(ROUND(AVG(psi_io)) AS INTEGER),CAST(ROUND(AVG(nvme_temp)) AS INTEGER),CAST(ROUND(AVG(bat_temp)) AS INTEGER) FROM system_raw WHERE ts < {open5} GROUP BY (ts/300)''')
con.execute(f'''INSERT OR REPLACE INTO gpu_5m SELECT (ts/300)*300,id,MAX(vendor),MAX(name),MAX(driver),MAX(state),CAST(ROUND(AVG(util)) AS INTEGER),CAST(ROUND(AVG(mem_pct)) AS INTEGER),CAST(ROUND(AVG(temp)) AS INTEGER),CAST(ROUND(AVG(power)) AS INTEGER),CAST(ROUND(AVG(freq)) AS INTEGER) FROM gpu_raw WHERE ts < {open5} GROUP BY (ts/300),id''')
con.execute(f'''INSERT OR REPLACE INTO cpu_cores_5m SELECT (ts/300)*300,core,CAST(ROUND(AVG(util)) AS INTEGER) FROM cpu_cores_raw WHERE ts < {open5} GROUP BY (ts/300),core''')
con.execute(f'''INSERT OR REPLACE INTO disks_5m SELECT (ts/300)*300,mount,CAST(ROUND(AVG(used_pct)) AS INTEGER) FROM disks_raw WHERE ts < {open5} GROUP BY (ts/300),mount''')
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
