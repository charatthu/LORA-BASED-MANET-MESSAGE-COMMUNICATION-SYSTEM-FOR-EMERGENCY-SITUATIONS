from flask import Flask, request, redirect, session, jsonify, Response
from functools import wraps
import sqlite3
import bcrypt
from datetime import datetime
import redis
import time
import csv
import os
import socket
import threading

app = Flask(__name__)
app.secret_key = 'your-secret-key-change-this-in-production'

NODE_TIMEOUT = 120
LOG_FOLDER = r'C:\Users\Asus\Documents\PlatformIO\Projects\Project3\server\Chat_Logs'

try:
    os.makedirs(LOG_FOLDER, exist_ok=True)
except Exception as _e:
    print(f"⚠️  Cannot create LOG_FOLDER: {_e}. Using script directory instead.")
    LOG_FOLDER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'Chat_Logs')
    os.makedirs(LOG_FOLDER, exist_ok=True)
nodes_db = {}

_dedup_cache = {}
_DEDUP_WINDOW = 2.0

@app.before_request
def log_request():
    if request.method == 'POST':
        print(f"📡 POST → {request.path}")
    if request.path == '/update':
        data = request.get_json(force=True, silent=True)
        if data:
            print(f"📦 dbUsers: {data.get('dbUsers')}")

def get_db():
    db = sqlite3.connect('mesh_users.db')
    db.row_factory = sqlite3.Row
    return db

def init_db():
    db = get_db()
    db.execute('''
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            role TEXT DEFAULT 'user',
            is_banned INTEGER DEFAULT 0,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            last_login TIMESTAMP
        )
    ''')
    
    db.execute('''
        CREATE TABLE IF NOT EXISTS login_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER,
            node_id TEXT,
            ip_address TEXT,
            login_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            logout_time TIMESTAMP,
            FOREIGN KEY (user_id) REFERENCES users(id)
        )
    ''')
    
    db.execute('''
        CREATE TABLE IF NOT EXISTS activity_logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            admin_user TEXT,
            action TEXT,
            target_user TEXT,
            timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            details TEXT
        )
    ''')

    db.execute('''
        CREATE TABLE IF NOT EXISTS chat_messages (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            date_str      TEXT,           -- "YYYY-MM-DD"
            time_str      TEXT,           -- "HH:MM:SS"
            room          TEXT,           -- ห้องหรือ "DM:<nodeId>"
            sender        TEXT,           -- username
            msg           TEXT,           -- เนื้อหาข้อความ
            msg_type      TEXT DEFAULT 'chat', -- chat | dm | system

            -- ── Routing (AODV) ──────────────────────────────────────────────
            origin_node   INTEGER,        -- node ต้นทาง (1-15)
            gateway_node  INTEGER,        -- node ที่ส่งขึ้น PC (gateway)
            next_hop      INTEGER,        -- next-hop ที่ gateway ใช้ถึง origin
            hop_count     INTEGER,        -- จำนวน hop ทั้งหมด
            route_type    TEXT,           -- "direct" | "relay" | "multi_hop"
            ttl_remaining INTEGER,        -- TTL ที่เหลือเมื่อถึง gateway
            pkt_id        TEXT,           -- packet ID (msg_id จาก ESP)
            dest_seq      INTEGER,        -- destSeqNum ของ route ที่ใช้
            route_path    TEXT,           -- JSON array เส้นทาง [1,3,2] ถ้า ESP ส่งมา

            -- ── RF / Physical Layer ─────────────────────────────────────────
            rssi          INTEGER,        -- RSSI ที่ gateway รับ (dBm)
            tx_power      INTEGER,        -- TX power ที่ origin ใช้ (dBm)
            freq_error    REAL,           -- Frequency error (Hz)
            bandwidth     INTEGER,        -- LoRa BW (kHz) เช่น 125
            spread_factor INTEGER,        -- LoRa SF เช่น 7-12
            coding_rate   TEXT,           -- LoRa CR เช่น "4/5"

            -- ── Timing / Latency ────────────────────────────────────────────
            delivery_ms       INTEGER,    -- รวม relay hop delay (ms) จาก ESP path tracking
            local_delivery_ms INTEGER,    -- ms วัดที่ local sender (origin==gateway เท่านั้น)
            origin_ts         TEXT,       -- timestamp ที่ origin ส่ง (ถ้า ESP ส่งมา)

            -- ── Network State Snapshot ──────────────────────────────────────
            active_nodes  INTEGER,        -- จำนวน node ออนไลน์ขณะนั้น
            neighbors_json TEXT,          -- JSON neighbors ของ gateway ขณะนั้น

            -- ── Path Tracking ────────────────────────────────────────────────
            path              TEXT,       -- เส้นทางแบบย่อ เช่น "1->2->4"
            path_detail       TEXT        -- เส้นทางพร้อม delay เช่น "1-(85ms)->2-(112ms)->4"
        )
    ''')

    db.execute('''
        CREATE TABLE IF NOT EXISTS mesh_events (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            event_type    TEXT,           -- "RREQ"|"RREP"|"RERR"|"HB"|"JOIN"|"LEAVE"
            origin_node   INTEGER,
            gateway_node  INTEGER,
            src_node      INTEGER,        -- node ที่ generate event
            dest_node     INTEGER,        -- เป้าหมายของ RREQ/RREP/RERR
            via_node      INTEGER,        -- next-hop ที่เกี่ยวข้อง (RERR: broken hop)
            hop_count     INTEGER,
            seq_num       INTEGER,
            rssi          INTEGER,
            ttl_remaining INTEGER,
            details       TEXT            -- JSON ข้อมูลเพิ่มเติม
        )
    ''')

    db.execute('''
        CREATE TABLE IF NOT EXISTS node_snapshots (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            gateway_node  INTEGER,
            node_id       INTEGER,
            rssi          INTEGER,        -- RSSI ของ node นี้ที่ gateway เห็น
            tx_power      INTEGER,
            is_gateway    INTEGER,
            is_online     INTEGER,
            neighbors     TEXT,           -- JSON array
            route_table   TEXT,           -- JSON routing table ถ้า ESP ส่งมา
            heap_free     INTEGER,        -- free heap ESP (bytes)
            uptime_sec    INTEGER         -- uptime ESP (วินาที)
        )
    ''')

    db.execute('''
        CREATE TABLE IF NOT EXISTS link_quality (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            node_a        INTEGER,
            node_b        INTEGER,
            rssi_a2b      INTEGER,        -- RSSI ที่ B เห็น A
            rssi_b2a      INTEGER,        -- RSSI ที่ A เห็น B (ถ้าทราบ)
            pdr           REAL,           -- Packet Delivery Ratio 0.0-1.0 (คำนวณ)
            link_state    TEXT            -- "good"|"marginal"|"poor"|"broken"
        )
    ''')
    db.execute('CREATE INDEX IF NOT EXISTS idx_chat_ts     ON chat_messages(timestamp)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_chat_room   ON chat_messages(room)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_chat_sender ON chat_messages(sender)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_chat_origin ON chat_messages(origin_node)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_events_ts   ON mesh_events(timestamp)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_events_type ON mesh_events(event_type)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_snap_ts     ON node_snapshots(timestamp)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_snap_node   ON node_snapshots(node_id)')
    db.execute('CREATE INDEX IF NOT EXISTS idx_lq_nodes    ON link_quality(node_a, node_b)')

    for _col, _def in [
        ('local_delivery_ms', 'INTEGER'),
        ('path',              'TEXT'),
        ('path_detail',       'TEXT'),
    ]:
        try:
            db.execute(f'ALTER TABLE chat_messages ADD COLUMN {_col} {_def}')
        except Exception:
            pass
    db.commit()

    db.execute('CREATE INDEX IF NOT EXISTS idx_chat_path   ON chat_messages(path)')

    admin_hash = bcrypt.hashpw('admin123'.encode('utf-8'), bcrypt.gensalt())
    try:
        db.execute('INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)',
                   ('admin', admin_hash, 'admin'))
        db.commit()
    except sqlite3.IntegrityError:
        pass
    
    db.close()

try:
    r = redis.Redis(host='localhost', port=6379, decode_responses=True)
    r.ping()
except:
    print("⚠️  Warning: Redis not connected. Using in-memory session storage.")
    r = None

def admin_required(f):
    @wraps(f)
    def decorated_function(*args, **kwargs):
        if 'admin_user' not in session:
            return redirect('/admin/login')
        return f(*args, **kwargs)
    return decorated_function

def get_online_users():
    """ดึงรายชื่อผู้ใช้ที่ออนไลน์จาก Redis"""
    online_users = []
    if not r:
        return online_users
    
    db = get_db()
    for key in r.scan_iter("session:*"):
        session_data = r.hgetall(key)
        if session_data:
            user_id = session_data.get('user_id')
            user = db.execute('SELECT username FROM users WHERE id = ?', (user_id,)).fetchone()
            if user:
                online_users.append({
                    'username': user['username'],
                    'node_id': session_data.get('node_id', 'N/A'),
                    'ip': session_data.get('ip', 'N/A'),
                    'login_time': session_data.get('login_time', 'N/A'),
                    'session_id': key.replace('session:', '')
                })
    db.close()
    return online_users

@app.route('/')
def public_index():
    return Response(HTML_PUBLIC, mimetype='text/html')

@app.route('/admin/login', methods=['GET', 'POST'])
def admin_login():
    if request.method == 'POST':
        username = request.form.get('username')
        password = request.form.get('password')
        
        db = get_db()
        user = db.execute('SELECT * FROM users WHERE username = ? AND role = ?', 
                          (username, 'admin')).fetchone()
        db.close()
        
        if user and bcrypt.checkpw(password.encode('utf-8'), user['password_hash']):
            session['admin_user'] = username
            return redirect('/admin/dashboard')
        else:
            return Response(HTML_LOGIN.replace('{{ error }}', 'Invalid credentials'), mimetype='text/html')
    
    return Response(HTML_LOGIN.replace('{{ error }}', ''), mimetype='text/html')

@app.route('/admin/logout')
def admin_logout():
    session.pop('admin_user', None)
    return redirect('/admin/login')

@app.route('/admin/dashboard')
@admin_required
def admin_dashboard():
    db = get_db()
    

    total_users = db.execute('SELECT COUNT(*) as count FROM users WHERE role != "admin"').fetchone()['count']
    today_logins = db.execute('''
        SELECT COUNT(*) as count FROM login_history 
        WHERE DATE(login_time) = DATE('now')
    ''').fetchone()['count']
    
    db.close()
    

    online_users = get_online_users()
    
    online_rows = ''
    for user in online_users:
        display_time = user['login_time'][:19] if len(user['login_time']) > 19 else user['login_time']
        
        online_rows += f'''
        <tr>
            <td>
                <div class="d-flex align-items-center">
                    <div class="avatar avatar-xs me-2">
                        <span class="avatar-initial rounded-circle bg-label-success" style="padding: 5px;">
                            <i class="bx bx-user" style="font-size: 14px;"></i>
                        </span>
                    </div>
                    <span class="fw-medium">{user['username']}</span>
                </div>
            </td>
            <td><span class="badge bg-label-primary">{user['node_id']}</span></td>
            <td><code>{user['ip']}</code></td>
            <td><small class="text-muted">{display_time}</small></td>
            <td>
                <button class="btn btn-sm btn-outline-danger" onclick="kickUser('{user['session_id']}')">
                    <i class="bx bx-log-out-circle me-1"></i> Kick
                </button>
            </td>
        </tr>
        '''
    
    if not online_rows:
        online_rows = '''
        <tr>
            <td colspan="5" class="text-center py-5">
                <div class="text-muted">
                    <i class="bx bx-ghost mb-2" style="font-size: 2rem; display: block;"></i>
                    <span>No users currently online</span>
                </div>
            </td>
        </tr>
        '''

    html = HTML_DASHBOARD
    html = html.replace('{{ admin_user }}', session.get('admin_user', 'Admin'))
    html = html.replace('{{ total_users }}', str(total_users))
    html = html.replace('{{ online_count }}', str(len(online_users)))
    html = html.replace('{{ today_logins }}', str(today_logins))
    html = html.replace('{{ online_rows }}', online_rows)

    return Response(html, mimetype='text/html')

@app.route('/api/nodes')
def api_nodes():
    try:
        now = time.time()
        output = []

        for nid, data in nodes_db.items():
            if nid == 0:
                continue
            if now - data["last_seen"] > 3600:
                continue

            online = (now - data["last_seen"] < NODE_TIMEOUT)
            output.append({
                "id": nid,
                "online": online,
                "rssi": data.get("rssi", 0) if online else 0,
                "neighbors": data.get("neighbors", []),
                "is_gateway": data.get("is_gateway", False),
                "last_seen_sec": int(now - data["last_seen"])
            })

        output.sort(key=lambda x: x["id"])
        return jsonify(output)

    except Exception as e:
        print(f"❌ /api/nodes ERROR: {e}")
        return jsonify([]), 500

@app.route('/update', methods=['POST'])
def update():
    try:
        data = request.get_json(force=True, silent=True)
        if not data: 
            return jsonify({"error": "invalid payload"}), 400

        now = time.time()
        ts_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        current_gateway = int(data.get('gatewayId', 0))

        if "nodes" in data:
            db_snap = get_db()
            for n in data["nodes"]:
                nid      = int(n.get("id", 0))
                if nid == 0:
                    continue
                rssi     = int(n.get("rssi", 0))
                tx_power = n.get("txPower")
                is_gw    = bool(n.get("isGw", False))
                nbrs     = n.get("nbs", [])
                heap_free   = n.get("heapFree")
                uptime_sec  = n.get("uptime")
                route_table_json = None
                if "routeTable" in n:
                    import json as _json
                    route_table_json = _json.dumps(n["routeTable"])

                prev = nodes_db.get(nid, {})
                prev_online   = prev.get("last_seen", 0) > 0
                prev_nbrs     = set(prev.get("neighbors", []))
                curr_nbrs     = set(nbrs)
                rssi_changed  = abs(rssi - prev.get("rssi", 0)) >= 10
                nbrs_changed  = prev_nbrs != curr_nbrs
                newly_online  = not prev_online

                nodes_db[nid] = {
                    "id": nid, "rssi": rssi, "tx_power": tx_power,
                    "neighbors": nbrs, "is_gateway": is_gw,
                    "last_seen": now, "heap_free": heap_free, "uptime_sec": uptime_sec,
                }

                if newly_online or nbrs_changed or rssi_changed:
                    db_snap.execute('''
                        INSERT INTO node_snapshots
                            (gateway_node, node_id, rssi, tx_power, is_gateway,
                             is_online, neighbors, route_table, heap_free, uptime_sec)
                        VALUES (?,?,?,?,?,?,?,?,?,?)
                    ''', (
                        current_gateway, nid, rssi, tx_power, 1 if is_gw else 0,
                        1, str(nbrs), route_table_json, heap_free, uptime_sec
                    ))

                for nb_id in nbrs:
                    node_a, node_b = min(nid, nb_id), max(nid, nb_id)
                    db_snap.execute('''
                        INSERT INTO link_quality (node_a, node_b, rssi_a2b, link_state)
                        VALUES (?,?,?,?)
                    ''', (node_a, node_b, rssi, _link_state_from_rssi(rssi)))

            db_snap.commit()
            db_snap.close()

        if "events" in data:
            import json as _json
            db_ev = get_db()
            for ev in data["events"]:
                db_ev.execute('''
                    INSERT INTO mesh_events
                        (event_type, origin_node, gateway_node, src_node, dest_node,
                         via_node, hop_count, seq_num, rssi, ttl_remaining, details)
                    VALUES (?,?,?,?,?,?,?,?,?,?,?)
                ''', (
                    ev.get("type"),
                    ev.get("origin"),
                    current_gateway,
                    ev.get("src"),
                    ev.get("dest"),
                    ev.get("via"),
                    ev.get("hops"),
                    ev.get("seq"),
                    ev.get("rssi"),
                    ev.get("ttl"),
                    _json.dumps(ev.get("extra", {}))
                ))
            db_ev.commit()
            db_ev.close()

        if "dbUsers" in data:
            db = get_db()
            if r:
                for key in r.scan_iter("session:*"):
                    session_data = r.hgetall(key)
                    if session_data.get('node_id') == str(current_gateway):
                        r.delete(key)

            for u_data in data["dbUsers"]:
                username = u_data.get('u')
                if not username:
                    continue
                user = db.execute('SELECT id FROM users WHERE username = ?', (username,)).fetchone()
                if not user:
                    db.execute('INSERT INTO users (username, password_hash, role) VALUES (?, ?, ?)',
                               (username, 'lora_mesh_default', 'user'))
                    db.commit()
                    user = db.execute('SELECT id FROM users WHERE username = ?', (username,)).fetchone()

                if u_data.get('isLoggedIn', False) and r and user:
                    session_id = f"sess_{user['id']}"
                    r.hset(f"session:{session_id}", mapping={
                        'user_id': user['id'],
                        'username': username,
                        'node_id': str(current_gateway),
                        'ip': data.get('ip', 'N/A'),
                        'login_time': datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    })
                    r.expire(f"session:{session_id}", 300)
                    thai_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    db.execute('UPDATE users SET last_login = ? WHERE username = ?', (thai_time, username))
                    db.commit()
                    print(f"✅ Active user: {username}")
            db.close()

        return jsonify({"status": "updated"})

    except Exception as e:
        print(f"❌ /update Error: {e}")
        return jsonify({"error": str(e)}), 500

def _link_state_from_rssi(rssi):
    """แปลง RSSI เป็น link state label"""
    if rssi is None:
        return "unknown"
    if rssi >= -70:
        return "good"
    elif rssi >= -85:
        return "marginal"
    elif rssi >= -100:
        return "poor"
    return "broken"

def _count_online_nodes():
    """นับ node ที่ online ขณะนั้น"""
    now = time.time()
    return sum(1 for nid, d in nodes_db.items()
               if nid != 0 and now - d.get("last_seen", 0) < NODE_TIMEOUT)

def write_chat_log(room, sender, msg, mesh_meta=None):
    """
    บันทึกข้อความลง CSV (legacy) และ SQLite chat_messages (พร้อม mesh metrics)

    mesh_meta (dict, optional) – ข้อมูล MANET ที่แนบมากับ request:
        origin_node, gateway_node, next_hop, hop_count, route_type,
        ttl_remaining, pkt_id, dest_seq, route_path,
        rssi, tx_power, freq_error, bandwidth, spread_factor, coding_rate,
        delivery_ms, origin_ts, msg_type
    """
    if mesh_meta is None:
        mesh_meta = {}
    try:
        now = datetime.now()
        date_str = now.strftime("%Y-%m-%d")
        time_str = now.strftime("%H:%M:%S")

        try:
            os.makedirs(LOG_FOLDER, exist_ok=True)
            filename = os.path.join(LOG_FOLDER, "Chat_Log.csv")
            file_exists = os.path.isfile(filename)
            with open(filename, mode='a', newline='', encoding='utf-8-sig') as file:
                writer = csv.writer(file)
                if not file_exists:
                    writer.writerow([
                        'Date', 'Time', 'Room', 'Sender', 'Message',
                        'OriginNode', 'GatewayNode', 'NextHop', 'HopCount',
                        'RouteType', 'TTL', 'PktID', 'RSSI', 'TxPower',
                        'DeliveryMs', 'LocalDeliveryMs', 'Path', 'PathDetail'
                    ])
                writer.writerow([
                    date_str, time_str, room, sender, msg,
                    mesh_meta.get('origin_node', ''),
                    mesh_meta.get('gateway_node', ''),
                    mesh_meta.get('next_hop', ''),
                    mesh_meta.get('hop_count', ''),
                    mesh_meta.get('route_type', ''),
                    mesh_meta.get('ttl_remaining', ''),
                    mesh_meta.get('pkt_id', ''),
                    mesh_meta.get('rssi', ''),
                    mesh_meta.get('tx_power', ''),
                    mesh_meta.get('delivery_ms', ''),
                    mesh_meta.get('local_delivery_ms', ''),
                    mesh_meta.get('path', ''),
                    mesh_meta.get('path_detail', ''),
                ])
            print(f"📄 CSV saved: {filename}")
        except Exception as csv_err:
            print(f"⚠️  CSV write failed (SQLite will still save): {csv_err}")

        active_nodes = _count_online_nodes()
        gw_node = mesh_meta.get('gateway_node')
        neighbors_json = None
        if gw_node and gw_node in nodes_db:
            import json
            neighbors_json = json.dumps(nodes_db[gw_node].get('neighbors', []))

        db = get_db()
        db.execute('''
            INSERT INTO chat_messages (
                date_str, time_str, room, sender, msg, msg_type,
                origin_node, gateway_node, next_hop, hop_count,
                route_type, ttl_remaining, pkt_id, dest_seq, route_path,
                rssi, tx_power, freq_error, bandwidth, spread_factor, coding_rate,
                delivery_ms, local_delivery_ms, origin_ts,
                path, path_detail,
                active_nodes, neighbors_json
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ''', (
            date_str, time_str, room, sender, msg,
            mesh_meta.get('msg_type', 'chat'),
            mesh_meta.get('origin_node'),
            mesh_meta.get('gateway_node'),
            mesh_meta.get('next_hop'),
            mesh_meta.get('hop_count'),
            mesh_meta.get('route_type'),
            mesh_meta.get('ttl_remaining'),
            mesh_meta.get('pkt_id'),
            mesh_meta.get('dest_seq'),
            mesh_meta.get('route_path'),
            mesh_meta.get('rssi'),
            mesh_meta.get('tx_power'),
            mesh_meta.get('freq_error'),
            mesh_meta.get('bandwidth'),
            mesh_meta.get('spread_factor'),
            mesh_meta.get('coding_rate'),
            mesh_meta.get('delivery_ms'),
            mesh_meta.get('local_delivery_ms'),
            mesh_meta.get('origin_ts'),
            mesh_meta.get('path') or '',
            mesh_meta.get('path_detail') or '',
            active_nodes,
            neighbors_json
        ))
        db.commit()

        origin = mesh_meta.get('origin_node')
        gw     = mesh_meta.get('gateway_node')
        rssi   = mesh_meta.get('rssi')
        if origin and gw and origin != gw:
            node_a, node_b = min(origin, gw), max(origin, gw)
            db.execute('''
                INSERT INTO link_quality (node_a, node_b, rssi_a2b, link_state)
                VALUES (?,?,?,?)
            ''', (node_a, node_b, rssi, _link_state_from_rssi(rssi)))
            db.commit()

        db.close()
        path_info = mesh_meta.get('path', '')
        print(f"📝 [Chat Saved] {room} | {sender}: {msg[:50]} | "
              f"path={path_info or '-'} hops={mesh_meta.get('hop_count','?')} "
              f"delay={mesh_meta.get('delivery_ms','?')}ms rssi={rssi}")
    except Exception as e:
        print(f"❌ write_chat_log Error: {e}")

def _extract_mesh_meta(source):
    """
    ดึง mesh metrics จาก request.values หรือ dict
    รองรับทั้ง GET/POST form-data และ JSON payload
    """
    g = source.get
    return {

        'origin_node':   _int_or_none(g('originNode') or g('origin_node')),
        'gateway_node':  _int_or_none(g('gatewayNode') or g('gateway_node')),
        'next_hop':      _int_or_none(g('nextHop') or g('next_hop')),
        'hop_count':     _int_or_none(g('hopCount') or g('hop_count')),
        'route_type':    g('routeType') or g('route_type'),
        'ttl_remaining': _int_or_none(g('ttl') or g('ttl_remaining')),
        'pkt_id':        g('pktId') or g('pkt_id'),
        'dest_seq':      _int_or_none(g('destSeq') or g('dest_seq')),
        'route_path':    g('routePath') or g('route_path'),

        'rssi':          _int_or_none(g('rssi')),
        'tx_power':      _int_or_none(g('txPower') or g('tx_power')),
        'freq_error':    _float_or_none(g('freqError') or g('freq_error')),
        'bandwidth':     _int_or_none(g('bw') or g('bandwidth')),
        'spread_factor': _int_or_none(g('sf') or g('spread_factor')),
        'coding_rate':   g('cr') or g('coding_rate'),

        'delivery_ms':        _int_or_none(g('deliveryMs') or g('delivery_ms')),
        'local_delivery_ms':  _int_or_none(g('localDeliveryMs') or g('local_delivery_ms')),
        'origin_ts':          g('originTs') or g('origin_ts'),

        'path':               g('path'),
        'path_detail':        g('pathDetail') or g('path_detail'),
        'msg_type':           g('msgType') or g('msg_type') or 'chat',
    }

def _int_or_none(v):
    try: return int(v)
    except: return None

def _float_or_none(v):
    try: return float(v)
    except: return None

@app.route('/log_chat', methods=['GET', 'POST'])
def log_chat():
    global _dedup_cache
    try:
        room   = request.values.get('room', 'General')
        sender = request.values.get('sender', 'Unknown')
        msg    = request.values.get('msg', '')

        if not msg:
            return "No message content", 200

        current_time = time.time()
        dedup_key = f"{sender}|{room}|{msg}"
        if _dedup_cache.get(dedup_key, 0) > current_time - _DEDUP_WINDOW:
            print(f"[DEDUP] ignored: {sender} in {room}: {msg[:30]}")
            return "Duplicate ignored", 200
        _dedup_cache[dedup_key] = current_time

        if len(_dedup_cache) > 500:
            cutoff = current_time - 10.0
            _dedup_cache = {k: v for k, v in _dedup_cache.items() if v > cutoff}

        mesh_meta = _extract_mesh_meta(request.values)

        write_chat_log(room, sender, msg, mesh_meta)

        if sender and sender != 'Unknown' and r:
            db = get_db()
            user = db.execute('SELECT id FROM users WHERE username = ?', (sender,)).fetchone()
            if user:
                node_hint = mesh_meta.get('gateway_node') or mesh_meta.get('origin_node') or room
                session_id = f"sess_{user['id']}"
                r.hset(f"session:{session_id}", mapping={
                    'user_id':    user['id'],
                    'username':   sender,
                    'node_id':    str(node_hint),
                    'ip':         request.remote_addr,
                    'login_time': datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                })
                r.expire(f"session:{session_id}", 300)
                print(f"✅ Session updated for: {sender}")
            db.close()

        return "OK", 200

    except Exception as e:
        print(f"❌ [log_chat ERROR] {e}")
        return str(e), 500

@app.route('/admin/users')
@admin_required
def admin_users():
    db = get_db()
    search = request.args.get('search', '')
    filter_status = request.args.get('status', 'all')
    page = int(request.args.get('page', 1))
    per_page = 20
    

    query = 'SELECT * FROM users WHERE role != "admin"'
    params = []
    
    if search:
        query += ' AND username LIKE ?'
        params.append(f'%{search}%')
    
    if filter_status == 'banned':
        query += ' AND is_banned = 1'
    
    offset = (page - 1) * per_page
    query += f' ORDER BY created_at DESC LIMIT {per_page} OFFSET {offset}'
    
    users = db.execute(query, params).fetchall()

    if filter_status == 'active':
        users = [u for u in users if r and r.exists(f"session:sess_{u['id']}")]
    elif filter_status == 'inactive':
        users = [u for u in users if not (r and r.exists(f"session:sess_{u['id']}"))]
    
    user_rows = ''
    for user in users:
        is_online = r.exists(f"session:sess_{user['id']}") if r else False
        
        if user['is_banned']:
            account_status = '<span class="badge bg-label-danger">Banned</span>'
        elif is_online:
            account_status = '<span class="badge bg-label-success">Active</span>'
        else:
            account_status = '<span class="badge bg-label-secondary">Inactive</span>'
        
        user_rows += f'''
        <tr>
            <td>
                <div class="d-flex align-items-center">
                    <div class="avatar avatar-xs me-2">
                        <span class="avatar-initial rounded-circle bg-label-primary" style="padding: 5px;">
                            <i class="bx bx-user" style="font-size: 14px;"></i>
                        </span>
                    </div>
                    <strong>{user['username']}</strong>
                </div>
            </td>
            <td>{account_status}</td>
            <td><small class="text-muted">{user['last_login'] or 'Never'}</small></td>
            <td>
                <div class="d-flex">
                    <button class="btn btn-sm btn-danger" onclick="deleteUser({user['id']}, '{user['username']}')">
                        <i class="bx bx-trash me-1"></i>Delete
                    </button>
                </div>
            </td>
        </tr>
        '''
    
    if not user_rows:
        user_rows = '''
        <tr>
            <td colspan="5" class="text-center py-5">
                <div class="text-muted">
                    <i class="bx bx-search-alt-2 mb-2" style="font-size: 2.5rem; display: block; opacity: 0.5;"></i>
                    <span class="fw-semibold">No users found</span>
                    <p class="small mb-0">Try adjusting your filters or search terms</p>
                </div>
            </td>
        </tr>
        '''

    db.close()

    html = HTML_USERS
    html = html.replace('{{ admin_user }}', session.get('admin_user', 'Admin'))
    html = html.replace('{{ user_rows }}', user_rows)
    html = html.replace('{{ search }}', search)
    html = html.replace('{{ page }}', str(page))

    return Response(html, mimetype='text/html')

@app.route('/admin/settings', methods=['GET', 'POST'])
@admin_required
def admin_settings():
    message = ''
    
    if request.method == 'POST':
        action = request.form.get('action')
        
        if action == 'change_password':
            new_password = request.form.get('new_password')
            password_hash = bcrypt.hashpw(new_password.encode('utf-8'), bcrypt.gensalt())
            
            db = get_db()
            db.execute('UPDATE users SET password_hash = ? WHERE username = ?',
                       (password_hash, session['admin_user']))
            db.commit()
            db.close()
            
            message = '<div class="alert success">✅ Password changed successfully</div>'
        
        elif action == 'reset_db':
            confirm = request.form.get('confirm')
            if confirm == 'DELETE ALL DATA':
                db = get_db()
                db.execute('DELETE FROM login_history')
                db.execute('DELETE FROM activity_logs')
                db.execute('DELETE FROM users WHERE role != "admin"')
                db.commit()
                db.close()
                
                if r:
                    for key in r.scan_iter("session:*"):
                        r.delete(key)
                    for key in r.scan_iter("user:*"):
                        r.delete(key)
                
                message = '<div class="alert success">✅ Database reset successfully</div>'
            else:
                message = '<div class="alert error">❌ Confirmation text incorrect</div>'
    
    html = HTML_SETTINGS
    html = html.replace('{{ admin_user }}', session['admin_user'])
    html = html.replace('{{ message }}', message)
    
    return Response(html, mimetype='text/html')

@app.route('/admin/api/kick_user', methods=['POST'])
@admin_required
def api_kick_user():
    session_id = request.json.get('session_id')
    
    if r:
        r.delete(f"session:{session_id}")
    
    db = get_db()
    db.execute('INSERT INTO activity_logs (admin_user, action, details) VALUES (?, ?, ?)',
               (session['admin_user'], 'KICK_USER', f'Session: {session_id}'))
    db.commit()
    db.close()
    
    return jsonify({'success': True})

@app.route('/admin/api/delete_user', methods=['POST'])
@admin_required
def api_delete_user():
    user_id = request.json.get('user_id')
    
    db = get_db()
    user = db.execute('SELECT username FROM users WHERE id = ?', (user_id,)).fetchone()
    
    if user:
        if r:
            session_key = r.get(f"user:{user_id}:session")
            if session_key:
                r.delete(f"session:{session_key}")
                r.delete(f"user:{user_id}:session")
        
        db.execute('DELETE FROM login_history WHERE user_id = ?', (user_id,))
        db.execute('DELETE FROM users WHERE id = ?', (user_id,))
        db.execute('INSERT INTO activity_logs (admin_user, action, target_user) VALUES (?, ?, ?)',
                   (session['admin_user'], 'DELETE_USER', user['username']))
        db.commit()
    
    db.close()
    return jsonify({'success': True})

@app.route('/api/active_users_list')
def api_active_users_list():
    if 'admin_user' not in session:
        return jsonify([])
    users = []

    if not r:
        return jsonify(users)

    db = get_db()

    for key in r.scan_iter("session:*"):
        session_data = r.hgetall(key)
        if not session_data:
            continue

        user_id = session_data.get("user_id")
        if not user_id:
            continue

        user = db.execute(
            "SELECT username FROM users WHERE id = ?",
            (user_id,)
        ).fetchone()

        if user:
            users.append({
                "username": user["username"],
                "node_id": session_data.get("node_id", "N/A"),
                "login_time": session_data.get("login_time", "")[:19]
            })

    db.close()
    return jsonify(users)

@app.route('/api/record_login', methods=['POST'])
def record_login():
    data = request.json
    username = data.get('username')
    ip = data.get('ip')
    node_id = data.get('node_id')
    
    log_file = os.path.join(LOG_FOLDER, "login_logs.txt")
    with open(log_file, "a", encoding="utf-8") as f:
        f.write(f"{datetime.now()} - User: {username}, Node: {node_id}\n")
    
    db = get_db()
    user = db.execute('SELECT id FROM users WHERE username = ?', (username,)).fetchone()
    
    if user:
        session_id = f"{user['id']}_{int(datetime.now().timestamp())}"
        
        if r:
            r.hset(f"session:{session_id}", mapping={
                'user_id': user['id'],
                'ip': ip,
                'node_id': node_id,
                'login_time': datetime.now().isoformat()
            })
            r.set(f"user:{user['id']}:session", session_id)
        
        db.execute('INSERT INTO login_history (user_id, node_id, ip_address) VALUES (?, ?, ?)',
                   (user['id'], node_id, ip))
        db.execute('UPDATE users SET last_login = CURRENT_TIMESTAMP WHERE id = ?', (user['id'],))
        db.commit()
    
    db.close()
    return jsonify({'success': True})

@app.route('/api/send_message', methods=['POST'])
def send_message():
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"error": "no data"}), 400
        print("📨 RAW payload:", data)

        username = data.get('u', 'Unknown')
        room     = data.get('room', 'General')
        msg      = data.get('msg', '')
        node_id  = data.get('node_id', 'N/A')
        dest_ip  = data.get('dest_ip')
        if dest_ip:
            room = f"DM:{dest_ip}"

        mesh_meta = _extract_mesh_meta(data)

        if not mesh_meta.get('gateway_node') and node_id not in ('N/A', None):
            try:
                mesh_meta['gateway_node'] = int(node_id)
            except:
                pass

        write_chat_log(room, username, msg, mesh_meta)

        if username and username != 'Unknown' and r:
            db = get_db()
            user = db.execute('SELECT id FROM users WHERE username = ?', (username,)).fetchone()
            if user:
                session_id = f"sess_{user['id']}"
                r.hset(f"session:{session_id}", mapping={
                    'user_id':    user['id'],
                    'username':   username,
                    'node_id':    str(node_id),
                    'ip':         data.get('ip', 'N/A'),
                    'login_time': datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                })
                r.expire(f"session:{session_id}", 300)
                print(f"✅ Session updated for: {username}")
            db.close()

        return jsonify({"status": "log_saved"})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/analytics')
def api_analytics():
    """
    วิเคราะห์ภาพรวม mesh network ย้อนหลัง
    params:
      from_ts  – ISO timestamp เริ่มต้น (default: 24h ที่แล้ว)
      to_ts    – ISO timestamp สิ้นสุด  (default: now)
      node     – กรองตาม origin_node (optional)
      room     – กรองตาม room (optional)
    """
    try:
        from_ts = request.args.get('from_ts', '')
        to_ts   = request.args.get('to_ts', '')
        node    = request.args.get('node')
        room    = request.args.get('room')

        where = []
        params = []

        if from_ts:
            where.append("timestamp >= ?"); params.append(from_ts)
        else:
            where.append("timestamp >= datetime('now','-24 hours')")
        if to_ts:
            where.append("timestamp <= ?"); params.append(to_ts)
        if node:
            where.append("origin_node = ?"); params.append(int(node))
        if room:
            where.append("room = ?"); params.append(room)

        w = ("WHERE " + " AND ".join(where)) if where else ""
        db = get_db()

        total_msgs = db.execute(f"SELECT COUNT(*) FROM chat_messages {w}", params).fetchone()[0]

        w_hop = (w + " AND" if w else "WHERE") + " hop_count IS NOT NULL"
        hop_rows = db.execute(f'''
            SELECT hop_count, COUNT(*) as cnt
            FROM chat_messages {w_hop}
            GROUP BY hop_count ORDER BY hop_count
        ''', params).fetchall()

        w_rf = (w + " AND" if w else "WHERE") + " origin_node IS NOT NULL"
        rf_rows = db.execute(f'''
            SELECT origin_node,
                   ROUND(AVG(rssi),1) as avg_rssi,
                   MIN(rssi)          as min_rssi,
                   MAX(rssi)          as max_rssi,
                   COUNT(*)           as msg_count
            FROM chat_messages {w_rf}
            GROUP BY origin_node ORDER BY origin_node
        ''', params).fetchall()

        w_rt = (w + " AND" if w else "WHERE") + " route_type IS NOT NULL"
        rt_rows = db.execute(f'''
            SELECT route_type, COUNT(*) as cnt
            FROM chat_messages {w_rt}
            GROUP BY route_type
        ''', params).fetchall()

        w_lat = (w + " AND" if w else "WHERE") + " delivery_ms IS NOT NULL AND delivery_ms >= 0"
        lat_row = db.execute(f'''
            SELECT ROUND(AVG(delivery_ms),1) as avg_ms,
                   MIN(delivery_ms) as min_ms,
                   MAX(delivery_ms) as max_ms
            FROM chat_messages {w_lat}
        ''', params).fetchone()

        hourly = db.execute('''
            SELECT strftime('%Y-%m-%d %H:00', timestamp) as hour, COUNT(*) as cnt
            FROM chat_messages
            WHERE timestamp >= datetime('now','-24 hours')
            GROUP BY hour ORDER BY hour
        ''').fetchall()

        db.close()
        import json as _json
        return jsonify({
            "total_messages": total_msgs,
            "hop_distribution": [{"hops": r[0], "count": r[1]} for r in hop_rows],
            "rf_per_node": [dict(r) for r in rf_rows],
            "route_type_breakdown": [{"type": r[0], "count": r[1]} for r in rt_rows],
            "latency_ms": dict(lat_row) if lat_row else {},
            "hourly_volume": [{"hour": r[0], "count": r[1]} for r in hourly],
        })
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/route_history')
def api_route_history():
    """
    ประวัติเส้นทางของแต่ละ destination node ย้อนหลัง
    params:
      dest  – origin_node ที่ต้องการดูเส้นทาง (required)
      limit – จำนวน record ล่าสุด (default 100)
    """
    try:
        dest  = request.args.get('dest')
        limit = int(request.args.get('limit', 100))
        if not dest:
            return jsonify({"error": "dest required"}), 400

        db = get_db()
        rows = db.execute('''
            SELECT timestamp, gateway_node, next_hop, hop_count,
                   route_type, ttl_remaining, dest_seq, route_path, rssi, delivery_ms
            FROM chat_messages
            WHERE origin_node = ?
            ORDER BY timestamp DESC LIMIT ?
        ''', (int(dest), limit)).fetchall()
        db.close()
        return jsonify([dict(r) for r in rows])
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/link_quality')
def api_link_quality():
    """
    คุณภาพ link ระหว่าง node คู่ต่างๆ
    params:
      node_a, node_b – กรองเฉพาะคู่นี้ (optional)
      hours          – ช่วงเวลาย้อนหลัง (default 24)
    """
    try:
        node_a = request.args.get('node_a')
        node_b = request.args.get('node_b')
        hours  = int(request.args.get('hours', 24))

        where  = [f"timestamp >= datetime('now','-{hours} hours')"]
        params = []
        if node_a:
            where.append("(node_a=? OR node_b=?)"); params += [int(node_a), int(node_a)]
        if node_b:
            where.append("(node_a=? OR node_b=?)"); params += [int(node_b), int(node_b)]

        w = "WHERE " + " AND ".join(where)
        db = get_db()

        agg = db.execute(f'''
            SELECT node_a, node_b,
                   ROUND(AVG(rssi_a2b),1) as avg_rssi,
                   MIN(rssi_a2b)          as min_rssi,
                   MAX(rssi_a2b)          as max_rssi,
                   COUNT(*)               as sample_count,
                   MAX(timestamp)         as last_seen
            FROM link_quality {w}
            GROUP BY node_a, node_b
            ORDER BY node_a, node_b
        ''', params).fetchall()

        db.close()
        result = []
        for r in agg:
            d = dict(r)
            d['link_state'] = _link_state_from_rssi(d['avg_rssi'])
            result.append(d)
        return jsonify(result)
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/mesh_events')
def api_mesh_events():
    """
    ดู RREQ/RREP/RERR/HB events ย้อนหลัง
    params:
      type   – event_type filter (RREQ|RREP|RERR|HB)
      node   – src_node หรือ dest_node filter
      hours  – ช่วงเวลา (default 1)
      limit  – จำนวน record (default 200)
    """
    try:
        ev_type = request.args.get('type')
        node    = request.args.get('node')
        hours   = int(request.args.get('hours', 1))
        limit   = int(request.args.get('limit', 200))

        where  = [f"timestamp >= datetime('now','-{hours} hours')"]
        params = []
        if ev_type:
            where.append("event_type = ?"); params.append(ev_type.upper())
        if node:
            where.append("(src_node=? OR dest_node=? OR origin_node=?)")
            params += [int(node), int(node), int(node)]

        w = "WHERE " + " AND ".join(where)
        db = get_db()
        rows = db.execute(f'''
            SELECT * FROM mesh_events {w}
            ORDER BY timestamp DESC LIMIT ?
        ''', params + [limit]).fetchall()
        db.close()
        return jsonify([dict(r) for r in rows])
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/topology_history')
def api_topology_history():
    """
    Replay topology snapshot ย้อนหลัง
    params:
      node   – node_id ที่ต้องการ (optional)
      hours  – ช่วงเวลา (default 1)
      limit  – จำนวน record (default 500)
    """
    try:
        node  = request.args.get('node')
        hours = int(request.args.get('hours', 1))
        limit = int(request.args.get('limit', 500))

        where  = [f"timestamp >= datetime('now','-{hours} hours')"]
        params = []
        if node:
            where.append("node_id = ?"); params.append(int(node))

        w = "WHERE " + " AND ".join(where)
        db = get_db()
        rows = db.execute(f'''
            SELECT * FROM node_snapshots {w}
            ORDER BY timestamp DESC LIMIT ?
        ''', params + [limit]).fetchall()
        db.close()
        return jsonify([dict(r) for r in rows])
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/chat_log')
def api_chat_log():
    """
    ดึง chat log ย้อนหลังพร้อม mesh metrics ครบ
    params:
      room, sender, origin_node – filter (optional)
      from_ts, to_ts            – ช่วงเวลา
      limit                     – default 100
    """
    try:
        room   = request.args.get('room')
        sender = request.args.get('sender')
        node   = request.args.get('origin_node')
        from_ts= request.args.get('from_ts')
        to_ts  = request.args.get('to_ts')
        limit  = int(request.args.get('limit', 100))

        where = []
        params = []
        if room:    where.append("room=?");        params.append(room)
        if sender:  where.append("sender=?");      params.append(sender)
        if node:    where.append("origin_node=?"); params.append(int(node))
        if from_ts: where.append("timestamp>=?");  params.append(from_ts)
        if to_ts:   where.append("timestamp<=?");  params.append(to_ts)

        w = ("WHERE " + " AND ".join(where)) if where else ""
        db = get_db()
        rows = db.execute(f'''
            SELECT * FROM chat_messages {w}
            ORDER BY timestamp DESC LIMIT ?
        ''', params + [limit]).fetchall()
        db.close()
        return jsonify([dict(r) for r in rows])
    except Exception as e:
        return jsonify({"error": str(e)}), 500

MESH_SIDEBAR = '''
    <a href="/admin/dashboard">&#128225; Network Monitor</a>
    <a href="/admin/users">&#128100; User Management</a>
    <a href="/admin/mesh/chatlog" id="nav_chatlog">&#128172; Chat Log</a>
    <a href="/admin/mesh/events" id="nav_events">&#9889; Mesh Events</a>
    <a href="/admin/mesh/links" id="nav_links">&#128268; Link Quality</a>
    <a href="/admin/settings">&#9881; Settings</a>
'''

MESH_BASE_CSS = '''
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:'Roboto',sans-serif; background:
.header { background:
.header h1 { font-size:18px; font-weight:700; }
.container { display:flex; min-height:calc(100vh - 50px); }
.sidebar { width:210px; background:white; border-right:1px solid
.sidebar a { display:block; padding:11px 20px; color:
.sidebar a:hover, .sidebar a.active { background:
.content { flex:1; padding:24px; overflow:auto; }
.section { background:white; border-radius:8px; padding:20px; margin-bottom:20px; box-shadow:0 1px 4px rgba(0,0,0,.06); }
.section-title { font-size:15px; font-weight:700; color:
table { width:100%; border-collapse:collapse; font-size:13px; }
th { background:
td { padding:9px 12px; border-bottom:1px solid
tr:hover td { background:
.badge { display:inline-block; padding:2px 9px; border-radius:12px; font-size:11px; font-weight:700; }
.badge-green  { background:
.badge-blue   { background:
.badge-yellow { background:
.badge-red    { background:
.badge-gray   { background:
.filters { display:flex; gap:10px; margin-bottom:16px; flex-wrap:wrap; align-items:center; }
.filters input, .filters select { padding:7px 12px; border:1px solid
.filters button { padding:7px 18px; background:
.filters button:hover { background:
.stat-row { display:flex; gap:16px; margin-bottom:20px; flex-wrap:wrap; }
.stat-card { background:white; border-radius:8px; padding:16px 20px; flex:1; min-width:140px; box-shadow:0 1px 4px rgba(0,0,0,.06); text-align:center; }
.stat-card h3 { font-size:11px; color:
.stat-card .num { font-size:28px; font-weight:700; color:
.stat-card .sub { font-size:11px; color:
'''

@app.route('/admin/mesh/chatlog')
@admin_required
def mesh_chatlog():
    return Response(HTML_MESH_CHATLOG, mimetype='text/html')

HTML_MESH_CHATLOG = '''<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Chat Log — Mesh Admin</title>
<link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap" rel="stylesheet">
<style>''' + MESH_BASE_CSS + '''
.rssi-bar { display:inline-block; width:60px; height:8px; background:
.rssi-fill { height:100%; border-radius:4px; }
.path-tag { display:inline-block; background:
.path-tag:hover { background:
</style></head>
<body>
<div class="header">
  <h1>&
  <a href="/admin/logout" style="color:white;text-decoration:none;font-size:13px;">Logout</a>
</div>
<div class="container">
  <div class="sidebar">''' + MESH_SIDEBAR + '''</div>
  <div class="content">

    <div class="filters">
      <input id="fRoom"   placeholder="Room..." style="width:130px">
      <input id="fSender" placeholder="Sender..." style="width:120px">
      <input id="fNode"   placeholder="Origin Node..." style="width:120px" type="number" min="1" max="15">
      <select id="fRoute">
        <option value="">All Routes</option>
        <option value="direct">Direct</option>
        <option value="relay">Relay</option>
        <option value="multi_hop">Multi-hop</option>
      </select>
      <input id="fFrom" type="datetime-local" style="width:175px">
      <input id="fTo"   type="datetime-local" style="width:175px">
      <button onclick="loadChat()">&
      <span id="chatCount" style="margin-left:auto;color:#888;font-size:12px;"></span>
    </div>

    <div class="section">
      <table id="chatTable">
        <thead><tr>
          <th>Time</th><th>Room</th><th>Sender</th><th>Message</th>
          <th>Origin</th><th>Hops</th><th>Path</th><th>Route</th>
          <th>RSSI</th><th>Relay Delay</th>
        </tr></thead>
        <tbody id="chatBody"><tr><td colspan="10" style="text-align:center;padding:30px;color:#aaa;">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
</div>
<script>
function rssiColor(v) {
  if (!v) return '#ccc';
  if (v >= -70) return '#2ecc71';
  if (v >= -85) return '#f39c12';
  if (v >= -100) return '#e74c3c';
  return '#999';
}
function rssiBar(v) {
  if (!v && v !== 0) return '-';
  return `<span style="color:${rssiColor(v)};font-weight:700">${v} dBm</span>`;
}
function routeBadge(r) {
  if (!r) return '<span class="badge badge-gray">-</span>';
  const map = {direct:'badge-green', relay:'badge-blue', multi_hop:'badge-yellow'};
  return `<span class="badge ${map[r]||'badge-gray'}">${r}</span>`;
}
async function loadChat() {
  const params = new URLSearchParams();
  const room   = document.getElementById('fRoom').value.trim();
  const sender = document.getElementById('fSender').value.trim();
  const node   = document.getElementById('fNode').value.trim();
  const route  = document.getElementById('fRoute').value;
  const from   = document.getElementById('fFrom').value;
  const to     = document.getElementById('fTo').value;
  if (room)   params.set('room', room);
  if (sender) params.set('sender', sender);
  if (node)   params.set('origin_node', node);
  if (from)   params.set('from_ts', from.replace('T',' '));
  if (to)     params.set('to_ts',   to.replace('T',' '));
  params.set('limit', 200);

  const res  = await fetch('/api/chat_log?' + params);
  let rows   = await res.json();
  if (route) rows = rows.filter(r => r.route_type === route);

  document.getElementById('chatCount').textContent = rows.length + ' records';
  const tbody = document.getElementById('chatBody');
  if (!rows.length) { tbody.innerHTML = '<tr><td colspan="10" style="text-align:center;padding:30px;color:#aaa;">No data</td></tr>'; return; }

  tbody.innerHTML = rows.map(r => {
    const pathTip = r.path_detail || r.path || '';
    const pathHtml = r.path ? `<span class="path-tag" title="${pathTip.replace(/"/g,'&quot;')}">&#128225; ${r.path}</span>` : '<span style="color:
    const delayHtml = r.delivery_ms != null
      ? `<span style="color:${r.delivery_ms<300?'#2ecc71':r.delivery_ms<700?'#f39c12':'#e74c3c'};font-weight:700">${r.delivery_ms}ms</span>`
      : (r.local_delivery_ms != null ? `<span style="color:#888">${r.local_delivery_ms}ms<sup style='font-size:9px'>local</sup></span>` : '-');
    return `<tr>
    <td style="white-space:nowrap;color:#888;font-size:12px">${(r.timestamp||'').slice(0,19)}</td>
    <td><span class="badge badge-blue">${r.room||'-'}</span></td>
    <td><b>${r.sender||'-'}</b></td>
    <td style="max-width:180px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${(r.msg||'').replace(/"/g,'&quot;')}">${r.msg||'-'}</td>
    <td style="text-align:center">${r.origin_node != null ? '<b>Node-'+r.origin_node+'</b>' : '-'}</td>
    <td style="text-align:center">${r.hop_count != null ? '<b>'+r.hop_count+'</b>' : '-'}</td>
    <td style="white-space:nowrap">${pathHtml}</td>
    <td>${routeBadge(r.route_type)}</td>
    <td style="white-space:nowrap">${rssiBar(r.rssi)}</td>
    <td style="text-align:center">${delayHtml}</td>
  </tr>`;
  }).join('');
}
loadChat();
setInterval(loadChat, 15000);
</script>
</body></html>
'''

@app.route('/admin/mesh/events')
@admin_required
def mesh_events_page():
    return Response(HTML_MESH_EVENTS, mimetype='text/html')

HTML_MESH_EVENTS = '''<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Mesh Events — Admin</title>
<link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap" rel="stylesheet">
<style>''' + MESH_BASE_CSS + '''</style></head>
<body>
<div class="header">
  <h1>&
  <a href="/admin/logout" style="color:white;text-decoration:none;font-size:13px;">Logout</a>
</div>
<div class="container">
  <div class="sidebar">''' + MESH_SIDEBAR + '''</div>
  <div class="content">

    <div class="stat-row" id="evStats"></div>

    <div class="filters">
      <select id="fType">
        <option value="">All Types</option>
        <option value="RREQ">RREQ</option>
        <option value="RREP">RREP</option>
        <option value="RERR">RERR</option>
        <option value="HB">HB</option>
      </select>
      <input id="fNode" placeholder="Node ID..." type="number" min="1" max="15" style="width:110px">
      <select id="fHours">
        <option value="1">Last 1h</option>
        <option value="6">Last 6h</option>
        <option value="24" selected>Last 24h</option>
        <option value="168">Last 7d</option>
      </select>
      <button onclick="loadEvents()">&
      <span id="evCount" style="margin-left:auto;color:#888;font-size:12px;"></span>
    </div>

    <div class="section">
      <table>
        <thead><tr>
          <th>Time</th><th>Type</th><th>Src</th><th>Dest</th>
          <th>Via</th><th>Hops</th><th>Seq</th><th>RSSI</th><th>TTL</th>
        </tr></thead>
        <tbody id="evBody"><tr><td colspan="9" style="text-align:center;padding:30px;color:#aaa;">Loading...</td></tr></tbody>
      </table>
    </div>
  </div>
</div>
<script>
const TYPE_STYLE = {
  RREQ: 'badge-yellow', RREP: 'badge-green',
  RERR: 'badge-red',    HB:   'badge-gray'
};
function rssiColor(v) {
  if (!v) return '#999';
  if (v >= -70) return '#2ecc71'; if (v >= -85) return '#f39c12'; return '#e74c3c';
}
async function loadEvents() {
  const params = new URLSearchParams({
    hours: document.getElementById('fHours').value,
    limit: 300
  });
  const t = document.getElementById('fType').value;
  const n = document.getElementById('fNode').value;
  if (t) params.set('type', t);
  if (n) params.set('node', n);

  const rows = await (await fetch('/api/mesh_events?' + params)).json();
  document.getElementById('evCount').textContent = rows.length + ' events';

  // stats
  const counts = {RREQ:0, RREP:0, RERR:0, HB:0};
  rows.forEach(r => { if (counts[r.event_type] !== undefined) counts[r.event_type]++; });
  document.getElementById('evStats').innerHTML = Object.entries(counts).map(([k,v]) =>
    `<div class="stat-card"><h3>${k}</h3><div class="num">${v}</div><div class="sub">events</div></div>`
  ).join('');

  const tbody = document.getElementById('evBody');
  if (!rows.length) { tbody.innerHTML = '<tr><td colspan="9" style="text-align:center;padding:30px;color:#aaa;">No events</td></tr>'; return; }
  tbody.innerHTML = rows.map(r => `<tr>
    <td style="white-space:nowrap;color:#888;font-size:12px">${(r.timestamp||'').slice(0,19)}</td>
    <td><span class="badge ${TYPE_STYLE[r.event_type]||'badge-gray'}">${r.event_type||'-'}</span></td>
    <td><b>${r.src_node != null ? 'Node-'+r.src_node : '-'}</b></td>
    <td>${r.dest_node != null ? 'Node-'+r.dest_node : '-'}</td>
    <td>${r.via_node  != null ? 'Node-'+r.via_node  : '-'}</td>
    <td style="text-align:center">${r.hop_count != null ? r.hop_count : '-'}</td>
    <td style="text-align:center;color:#888">${r.seq_num != null ? r.seq_num : '-'}</td>
    <td style="color:${rssiColor(r.rssi)};font-weight:700">${r.rssi != null ? r.rssi+' dBm' : '-'}</td>
    <td style="text-align:center">${r.ttl_remaining != null ? r.ttl_remaining : '-'}</td>
  </tr>`).join('');
}
loadEvents();
setInterval(loadEvents, 10000);
</script>
</body></html>
'''

@app.route('/admin/mesh/links')
@admin_required
def mesh_links():
    return Response(HTML_MESH_LINKS, mimetype='text/html')

HTML_MESH_LINKS = '''<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Link Quality — Admin</title>
<link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap" rel="stylesheet">
<style>''' + MESH_BASE_CSS + '''
.lq-matrix { display:grid; gap:12px; margin-bottom:20px; }
.lq-card { background:white; border-radius:8px; padding:16px; box-shadow:0 1px 4px rgba(0,0,0,.06); display:flex; align-items:center; gap:16px; }
.lq-nodes { font-size:18px; font-weight:700; color:
.lq-bar-wrap { flex:1; }
.lq-bar-bg { height:12px; background:
.lq-bar-fill { height:100%; border-radius:6px; transition:width .5s; }
.lq-val { min-width:70px; text-align:right; font-weight:700; font-size:15px; }
.lq-state { min-width:80px; text-align:center; }
</style></head>
<body>
<div class="header">
  <h1>&
  <a href="/admin/logout" style="color:white;text-decoration:none;font-size:13px;">Logout</a>
</div>
<div class="container">
  <div class="sidebar">''' + MESH_SIDEBAR + '''</div>
  <div class="content">

    <div class="filters">
      <select id="fHours">
        <option value="1">Last 1h</option>
        <option value="6">Last 6h</option>
        <option value="24" selected>Last 24h</option>
      </select>
      <button onclick="loadLinks()">&
    </div>

    <div id="lqCards" class="lq-matrix"></div>

    <div class="section">
      <div class="section-title">History Table</div>
      <table>
        <thead><tr>
          <th>Node A</th><th>Node B</th><th>Avg RSSI</th>
          <th>Min RSSI</th><th>Max RSSI</th><th>Samples</th>
          <th>State</th><th>Last Seen</th>
        </tr></thead>
        <tbody id="lqBody"></tbody>
      </table>
    </div>
  </div>
</div>
<script>
function rssiColor(v) {
  if (!v) return '#ccc';
  if (v >= -70) return '#2ecc71'; if (v >= -85) return '#f39c12'; return '#e74c3c';
}
function stateBadge(s) {
  const map = {good:'badge-green', marginal:'badge-yellow', poor:'badge-red', broken:'badge-gray', unknown:'badge-gray'};
  return `<span class="badge ${map[s]||'badge-gray'}">${s||'-'}</span>`;
}
async function loadLinks() {
  const hours = document.getElementById('fHours').value;
  const rows  = await (await fetch('/api/link_quality?hours='+hours)).json();

  // cards
  const cards = document.getElementById('lqCards');
  if (!rows.length) { cards.innerHTML = '<p style="color:#aaa;padding:20px">No link data yet</p>'; }
  else cards.innerHTML = rows.map(r => {
    const pct = Math.max(0, Math.min(100, ((r.avg_rssi + 120) / 80) * 100));
    const col = rssiColor(r.avg_rssi);
    return `<div class="lq-card">
      <div class="lq-nodes">Node-${r.node_a} ↔ Node-${r.node_b}</div>
      <div class="lq-bar-wrap">
        <div style="display:flex;justify-content:space-between;font-size:11px;color:#888;margin-bottom:3px">
          <span>-120 dBm</span><span>-40 dBm</span>
        </div>
        <div class="lq-bar-bg"><div class="lq-bar-fill" style="width:${pct}%;background:${col}"></div></div>
      </div>
      <div class="lq-val" style="color:${col}">${r.avg_rssi} dBm</div>
      <div class="lq-state">${stateBadge(r.link_state)}</div>
      <div style="font-size:11px;color:#aaa;min-width:60px;text-align:right">${r.sample_count} samples</div>
    </div>`;
  }).join('');

  // table
  const tbody = document.getElementById('lqBody');
  if (!rows.length) { tbody.innerHTML = '<tr><td colspan="8" style="text-align:center;padding:20px;color:#aaa">No data</td></tr>'; return; }
  tbody.innerHTML = rows.map(r => `<tr>
    <td><b>Node-${r.node_a}</b></td>
    <td><b>Node-${r.node_b}</b></td>
    <td style="color:${rssiColor(r.avg_rssi)};font-weight:700">${r.avg_rssi} dBm</td>
    <td style="color:#e74c3c">${r.min_rssi} dBm</td>
    <td style="color:#2ecc71">${r.max_rssi} dBm</td>
    <td style="text-align:center">${r.sample_count}</td>
    <td>${stateBadge(r.link_state)}</td>
    <td style="color:#888;font-size:12px">${(r.last_seen||'').slice(0,19)}</td>
  </tr>`).join('');
}
loadLinks();
setInterval(loadLinks, 20000);
</script>
</body></html>
'''

@app.route('/admin/mesh/analytics')
@admin_required
def mesh_analytics():
    return redirect('/admin/dashboard')

HTML_MESH_ANALYTICS = '''<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Analytics — Mesh Admin</title>
<link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap" rel="stylesheet">
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.0/chart.umd.min.js"></script>
<style>''' + MESH_BASE_CSS + '''
.chart-wrap { position:relative; height:220px; }
.grid2 { display:grid; grid-template-columns:1fr 1fr; gap:16px; }
@media(max-width:900px){ .grid2{grid-template-columns:1fr;} }
</style></head>
<body>
<div class="header">
  <h1>&
  <a href="/admin/logout" style="color:white;text-decoration:none;font-size:13px;">Logout</a>
</div>
<div class="container">
  <div class="sidebar">''' + MESH_SIDEBAR + '''</div>
  <div class="content">

    <div class="filters">
      <select id="fHours" onchange="loadAnalytics()">
        <option value="1">Last 1h</option>
        <option value="6">Last 6h</option>
        <option value="24" selected>Last 24h</option>
        <option value="168">Last 7d</option>
      </select>
      <span id="lastUpdate" style="margin-left:auto;color:#aaa;font-size:12px;"></span>
    </div>

    <!-- summary cards -->
    <div class="stat-row" id="summaryCards"></div>

    <div class="grid2">
      <!-- hop distribution -->
      <div class="section">
        <div class="section-title">Hop Distribution</div>
        <div class="chart-wrap"><canvas id="hopChart"></canvas></div>
      </div>

      <!-- route type -->
      <div class="section">
        <div class="section-title">Route Type</div>
        <div class="chart-wrap"><canvas id="rtChart"></canvas></div>
      </div>

      <!-- hourly volume -->
      <div class="section" style="grid-column:1/-1">
        <div class="section-title">Message Volume (Hourly)</div>
        <div class="chart-wrap" style="height:180px"><canvas id="volChart"></canvas></div>
      </div>
    </div>

    <!-- RSSI per node table -->
    <div class="section">
      <div class="section-title">RSSI per Origin Node</div>
      <table>
        <thead><tr>
          <th>Node</th><th>Avg RSSI</th><th>Min</th><th>Max</th><th>Messages</th><th>Signal Bar</th>
        </tr></thead>
        <tbody id="rssiBody"></tbody>
      </table>
    </div>

  </div>
</div>
<script>
let hopChart, rtChart, volChart;
function rssiColor(v) {
  if (!v) return '#ccc';
  if (v >= -70) return '#2ecc71'; if (v >= -85) return '#f39c12'; return '#e74c3c';
}
function makeBar(v) {
  const pct = Math.max(0, Math.min(100, ((v + 120) / 80) * 100));
  return `<div style="background:#e0e6ed;border-radius:4px;height:10px;width:100px;display:inline-block;overflow:hidden;vertical-align:middle">
    <div style="height:100%;width:${pct}%;background:${rssiColor(v)};border-radius:4px"></div></div>`;
}

async function loadAnalytics() {
  const h = document.getElementById('fHours').value;
  const now = new Date(); now.setHours(now.getHours() - h);
  const from_ts = now.toISOString().slice(0,19).replace('T',' ');
  const data = await (await fetch(`/api/analytics?from_ts=${encodeURIComponent(from_ts)}`)).json();

  document.getElementById('lastUpdate').textContent = 'Updated: ' + new Date().toLocaleTimeString();

  // summary cards
  const lat = data.latency_ms || {};
  document.getElementById('summaryCards').innerHTML = [
    ['Total Messages', data.total_messages, ''],
    ['Avg Latency',    lat.avg_ms != null ? lat.avg_ms+'ms' : '-', 'origin→gateway'],
    ['Max Latency',    lat.max_ms != null ? lat.max_ms+'ms' : '-', ''],
    ['Active Nodes',   data.rf_per_node ? data.rf_per_node.length : '-', 'nodes seen'],
  ].map(([t,v,s]) => `<div class="stat-card"><h3>${t}</h3><div class="num">${v}</div><div class="sub">${s}</div></div>`).join('');

  // hop chart
  const hops = data.hop_distribution || [];
  if (hopChart) hopChart.destroy();
  hopChart = new Chart(document.getElementById('hopChart'), {
    type: 'bar',
    data: {
      labels: hops.map(h => h.hops + ' hop' + (h.hops>1?'s':'')),
      datasets: [{ label: 'Messages', data: hops.map(h => h.count),
        backgroundColor: ['#2ecc71','#3498db','#f39c12','#e74c3c','#9b59b6'] }]
    },
    options: { responsive:true, maintainAspectRatio:false,
      plugins:{ legend:{display:false} }, scales:{ y:{ beginAtZero:true, ticks:{stepSize:1} } } }
  });

  // route type chart
  const rt = data.route_type_breakdown || [];
  if (rtChart) rtChart.destroy();
  rtChart = new Chart(document.getElementById('rtChart'), {
    type: 'doughnut',
    data: {
      labels: rt.map(r => r.type),
      datasets: [{ data: rt.map(r => r.count),
        backgroundColor: ['#2ecc71','#3498db','#f39c12','#e74c3c'] }]
    },
    options: { responsive:true, maintainAspectRatio:false,
      plugins:{ legend:{ position:'right' } } }
  });

  // hourly volume
  const vol = data.hourly_volume || [];
  if (volChart) volChart.destroy();
  volChart = new Chart(document.getElementById('volChart'), {
    type: 'line',
    data: {
      labels: vol.map(v => v.hour.slice(11,16)),
      datasets: [{ label: 'Messages', data: vol.map(v => v.count),
        borderColor:'#1a2a44', backgroundColor:'rgba(26,42,68,.1)',
        fill:true, tension:.3, pointRadius:3 }]
    },
    options: { responsive:true, maintainAspectRatio:false,
      plugins:{ legend:{display:false} }, scales:{ y:{ beginAtZero:true } } }
  });

  // RSSI table
  const rf = data.rf_per_node || [];
  document.getElementById('rssiBody').innerHTML = rf.length
    ? rf.map(r => `<tr>
        <td><b>Node-${r.origin_node}</b></td>
        <td style="color:${rssiColor(r.avg_rssi)};font-weight:700">${r.avg_rssi} dBm</td>
        <td style="color:#e74c3c">${r.min_rssi} dBm</td>
        <td style="color:#2ecc71">${r.max_rssi} dBm</td>
        <td style="text-align:center">${r.msg_count}</td>
        <td>${makeBar(r.avg_rssi)} <span style="font-size:11px;color:#aaa;margin-left:6px">${r.avg_rssi} dBm</span></td>
      </tr>`).join('')
    : '<tr><td colspan="6" style="text-align:center;padding:20px;color:#aaa">No data</td></tr>';
}
loadAnalytics();
setInterval(loadAnalytics, 30000);
</script>
</body></html>
'''

HTML_LOGIN = '''<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Admin Login</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">

    <style>
        :root {
            --bg:
            --card:
            --primary:
            --primary-dark:
            --text:
            --muted:
            --border:
        }

        * {
            box-sizing: border-box;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI",
                         Roboto, "Helvetica Neue", Arial, sans-serif;
        }

        body {
            margin: 0;
            height: 100vh;
            background: var(--bg);
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .login-card {
            width: 100%;
            max-width: 380px;
            background: var(--card);
            border-radius: 16px;
            padding: 36px 32px;
            box-shadow:
                0 10px 25px rgba(0,0,0,0.08),
                0 2px 6px rgba(0,0,0,0.05);
        }

        .logo {
            text-align: center;
            font-size: 32px;
            margin-bottom: 10px;
        }

        h1 {
            text-align: center;
            font-size: 20px;
            color: var(--text);
            margin-bottom: 6px;
        }

        .subtitle {
            text-align: center;
            font-size: 13px;
            color: var(--muted);
            margin-bottom: 28px;
        }

        label {
            display: block;
            font-size: 13px;
            color: var(--muted);
            margin-bottom: 6px;
        }

        input {
            width: 100%;
            padding: 12px 14px;
            font-size: 14px;
            border-radius: 10px;
            border: 1px solid var(--border);
            outline: none;
            transition: border 0.2s, box-shadow 0.2s;
            margin-bottom: 18px;
        }

        input:focus {
            border-color: var(--primary);
            box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.15);
        }

        button {
            width: 100%;
            padding: 12px;
            font-size: 14px;
            font-weight: 600;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            color: white;
            background: linear-gradient(
                135deg,
                var(--primary),
                var(--primary-dark)
            );
            transition: transform 0.1s, box-shadow 0.1s, opacity 0.1s;
        }

        button:hover {
            opacity: 0.95;
            box-shadow: 0 6px 15px rgba(37, 99, 235, 0.35);
        }

        button:active {
            transform: translateY(1px);
        }

        .error {
            margin-top: 16px;
            font-size: 13px;
            color:
            text-align: center;
        }

        .footer {
            text-align: center;
            margin-top: 28px;
            font-size: 12px;
            color: var(--muted);
        }
    </style>
</head>

<body>
    <div class="login-card">
        <div class="logo">🔐</div>

        <h1>Admin Panel</h1>
        <div class="subtitle">LoRa Mesh Control Center</div>

        <form method="POST">
            <label>Username</label>
            <input type="text" name="username" required autofocus>

            <label>Password</label>
            <input type="password" name="password" required>

            <button type="submit">Sign in</button>
        </form>

        <div class="error">{{ error }}</div>

        <div class="footer">
            © 2026 LoRa Mesh System
        </div>
    </div>
</body>
</html>'''

HTML_DASHBOARD = '''
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>LoRa Mesh Admin</title>
    <link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;700&display=swap" rel="stylesheet">
    <style>
        *{margin:0;padding:0;box-sizing:border-box;}
        body{font-family:'Roboto',sans-serif;background:
        .header{background:
        .header h1{font-size:20px;font-weight:700;}
        .container{display:flex;min-height:calc(100vh - 56px);}
        .sidebar{width:220px;background:white;border-right:1px solid
        .sidebar a{display:block;padding:12px 24px;color:
        .sidebar a:hover,.sidebar a.active{background:
        .content{flex:1;padding:24px;overflow-y:auto;}

        /* stat cards */
        .stats-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin-bottom:20px;}
        .stat-card{background:white;padding:18px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.06);text-align:center;}
        .stat-card h3{font-size:12px;color:
        .stat-card .num{font-size:30px;font-weight:800;color:
        .stat-card .sub{font-size:11px;color:

        /* sections */
        .section{background:white;padding:20px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.06);margin-bottom:20px;}
        .section-title{font-size:16px;font-weight:700;margin-bottom:14px;color:
        .grid-2{display:grid;grid-template-columns:1.3fr 1fr;gap:20px;}
        .grid-3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:20px;}

        canvas{border:1px solid

        /* tables */
        table{width:100%;border-collapse:collapse;}
        th{background:
        td{padding:10px 12px;text-align:center;border-bottom:1px solid
        tr:last-child td{border-bottom:none;}
        tr:hover td{background:

        /* badges */
        .badge{padding:4px 10px;border-radius:20px;font-size:11px;font-weight:700;display:inline-block;}
        .bg-online{background:
        .bg-offline{background:
        .hop-1{background:
        .hop-2{background:
        .hop-3{background:
        .hop-x{background:
        .rt-direct{background:
        .rt-relay{background:
        .rt-multi{background:

        /* RSSI bar */
        .rssi-bar{width:70px;height:8px;background:
        .rssi-fill{height:100%;border-radius:4px;}

        /* routing table */
        .route-row td{font-size:12px;}
        .route-invalid{opacity:.4;}

        /* tabs */
        .tabs{display:flex;gap:4px;margin-bottom:16px;border-bottom:2px solid
        .tab{padding:8px 16px;cursor:pointer;font-size:13px;font-weight:600;color:
        .tab.active{color:
        .tab-panel{display:none;}
        .tab-panel.active{display:block;}

        /* event log */
        .event-log{max-height:200px;overflow-y:auto;font-size:12px;font-family:monospace;}
        .ev-RREQ{color:
        .ev-row{padding:3px 0;border-bottom:1px solid
        .ev-time{color:
        .status-dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px;}
        .dot-online{background:
    </style>
</head>
<body>
<div class="header">
    <h1>&
    <div>
        <span style="margin-right:12px;font-size:14px">Admin: <strong>{{ admin_user }}</strong></span>
        <a href="/admin/logout" style="color:white;text-decoration:none;font-size:13px;border:1px solid rgba(255,255,255,.5);padding:6px 14px;border-radius:6px;">Logout</a>
    </div>
</div>

<div class="container">
<div class="sidebar">
    <a href="/admin/dashboard" class="active">&
    <a href="/admin/users">&
    <a href="/admin/mesh/chatlog">&
    <a href="/admin/mesh/events">&
    <a href="/admin/mesh/links">&
    <a href="/admin/settings">&
</div>
<div class="content">

    <!-- stat cards -->
    <div class="stats-grid">
        <div class="stat-card"><h3>Online Nodes</h3><div class="num" id="statNodes">-</div><div class="sub">active</div></div>
        <div class="stat-card"><h3>Active Users</h3><div class="num" id="statUsers">-</div><div class="sub">/ {{ total_users }} total</div></div>
        <div class="stat-card"><h3>Avg RSSI</h3><div class="num" id="statRssi">-</div><div class="sub">dBm</div></div>
        <div class="stat-card"><h3>Latest RSSI</h3><div class="num" id="statRssiLatest">-</div><div class="sub" id="statRssiLatestNode">dBm</div></div>
        <div class="stat-card"><h3>Avg Hop Count</h3><div class="num" id="statHops">-</div><div class="sub">last 1h</div></div>
    </div>

    <!-- topology -->
    <div class="section">
        <div class="section-title">&
        <canvas id="netMap" width="860" height="360"></canvas>
    </div>

    <!-- nodes + routing tabs -->
    <div class="section">
        <div class="tabs">
            <div class="tab active" onclick="switchTab('nodes')">&
            <div class="tab" onclick="switchTab('routing')">&
            <div class="tab" onclick="switchTab('events')">&
        </div>

        <!-- Nodes tab -->
        <div id="tab-nodes" class="tab-panel active">
            <table>
                <thead><tr>
                    <th>Node</th><th>Status</th><th>Type</th>
                    <th>RSSI</th><th>Neighbors</th><th>Last Seen</th>
                </tr></thead>
                <tbody id="nodesTableBody"><tr><td colspan="6">Loading...</td></tr></tbody>
            </table>
        </div>

        <!-- Routing Table tab -->
        <div id="tab-routing" class="tab-panel">
            <div style="display:flex;gap:8px;margin-bottom:10px;align-items:center;">
                <label style="font-size:13px;font-weight:600;color:#555;">View from Node:</label>
                <select id="routeNodeSel" onchange="fetchRouteHistory()" style="padding:4px 10px;border:1px solid #ddd;border-radius:6px;font-size:13px;">
                    <option value="">-- Select --</option>
                </select>
                <button onclick="fetchRouteHistory()" style="padding:4px 12px;border:1px solid #ddd;border-radius:6px;font-size:12px;cursor:pointer;background:#1a2a44;color:white;">Refresh</button>
                <span style="font-size:12px;color:#aaa;margin-left:8px;">Latest 50 messages with routing info</span>
            </div>
            <table>
                <thead><tr>
                    <th>Time</th><th>Room</th><th>Origin</th><th>Gateway</th>
                    <th>Next Hop</th><th>Hops</th><th>Route Type</th>
                    <th>TTL Left</th><th>Seq
                </tr></thead>
                <tbody id="routeTableBody"><tr><td colspan="10" style="color:#aaa;">Select a node above</td></tr></tbody>
            </table>
        </div>

        <!-- Events tab -->
        <div id="tab-events" class="tab-panel">
            <div style="display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap;">
                <button onclick="fetchEvents('')"  style="padding:4px 12px;border:1px solid #ddd;border-radius:20px;font-size:12px;cursor:pointer;background:#1a2a44;color:white;">ALL</button>
                <button onclick="fetchEvents('RREQ')" style="padding:4px 12px;border:1px solid #ddd;border-radius:20px;font-size:12px;cursor:pointer;">RREQ</button>
                <button onclick="fetchEvents('RREP')" style="padding:4px 12px;border:1px solid #ddd;border-radius:20px;font-size:12px;cursor:pointer;">RREP</button>
                <button onclick="fetchEvents('RERR')" style="padding:4px 12px;border:1px solid #ddd;border-radius:20px;font-size:12px;cursor:pointer;color:#991b1b;">RERR</button>
                <button onclick="fetchEvents('HB')"   style="padding:4px 12px;border:1px solid #ddd;border-radius:20px;font-size:12px;cursor:pointer;">HB</button>
            </div>
            <div class="event-log" id="eventLog"><span style="color:#aaa;">Loading events...</span></div>
        </div>
    </div>

    <!-- active users + link quality -->
    <div class="grid-2">
        <div class="section">
            <div class="section-title">&
            <table>
                <thead><tr><th>User</th><th>Node</th><th>Login Time</th></tr></thead>
                <tbody id="usersTableBody"><tr><td colspan="4">Loading...</td></tr></tbody>
            </table>
        </div>
        <div class="section">
            <div class="section-title">&
            <table>
                <thead><tr><th>Link</th><th>Avg RSSI</th><th>Min</th><th>Max</th><th>State</th><th>Samples</th></tr></thead>
                <tbody id="linkTableBody"><tr><td colspan="6">Loading...</td></tr></tbody>
            </table>
        </div>
    </div>

    <p style="text-align:center;color:#aaa;font-size:12px;margin-top:12px;">
        Last sync: <span id="lastUpdateText">-</span>
        &nbsp;|&nbsp; Auto-refresh every 5s
    </p>
</div><!-- .content -->
</div><!-- .container -->

<script>
var dbTotalUsers = {{ total_users }};

// ── Topology canvas ──────────────────────────────────────────────────────────
const canvas = document.getElementById('netMap');
const ctx    = canvas.getContext('2d');
let nodesData = [];
const fixedPos = {1:{x:180,y:100},2:{x:680,y:100},3:{x:180,y:280},4:{x:680,y:280},5:{x:430,y:190}};
function getPos(id){ return fixedPos[id]||{x:430+(id*37)%200,y:190+(id*29)%120}; }

function rssiColor(rssi){
    if(rssi >= -70) return '#22c55e';
    if(rssi >= -85) return '#f59e0b';
    if(rssi >= -100) return '#ef4444';
    return '#9ca3af';
}
function rssiPct(rssi){ return Math.max(0,Math.min(100,(rssi+120)/80*100)); }

function drawTopology(){
    ctx.clearRect(0,0,canvas.width,canvas.height);
    if(!nodesData.length){
        ctx.fillStyle='#aaa'; ctx.font='15px Roboto'; ctx.textAlign='center';
        ctx.fillText('Waiting for node data...', canvas.width/2, canvas.height/2); return;
    }
    // edges
    const drawn = new Set();
    nodesData.forEach(node=>{
        if(!node.neighbors) return;
        node.neighbors.forEach(nbId=>{
            const key = [Math.min(node.id,nbId),Math.max(node.id,nbId)].join('-');
            if(drawn.has(key)) return; drawn.add(key);
            const t = nodesData.find(k=>k.id==nbId); if(!t) return;
            const p1=getPos(node.id), p2=getPos(t.id);
            const active = node.online && t.online;
            ctx.beginPath(); ctx.moveTo(p1.x,p1.y); ctx.lineTo(p2.x,p2.y);
            ctx.strokeStyle = active ? rssiColor(Math.min(node.rssi||0,t.rssi||0)) : '#e5e7eb';
            ctx.lineWidth = active ? 3 : 1.5;
            ctx.setLineDash(active ? [] : [5,4]);
            ctx.stroke(); ctx.setLineDash([]);
            // RSSI label on edge
            if(active && node.rssi){
                const mx=(p1.x+p2.x)/2, my=(p1.y+p2.y)/2;
                ctx.fillStyle='#64748b'; ctx.font='11px Roboto'; ctx.textAlign='center';
                ctx.fillText(node.rssi+'dBm', mx, my-5);
            }
        });
    });
    // nodes
    nodesData.forEach(n=>{
        const pos=getPos(n.id);
        ctx.beginPath(); ctx.arc(pos.x,pos.y,28,0,Math.PI*2);
        ctx.fillStyle='#fff'; ctx.fill();
        ctx.lineWidth=3; ctx.strokeStyle=n.online?rssiColor(n.rssi||0):'#d1d5db'; ctx.stroke();
        ctx.fillStyle='#1a2a44'; ctx.font='bold 15px Roboto'; ctx.textAlign='center';
        ctx.fillText(n.id, pos.x, pos.y+5);
        // gateway crown
        if(n.is_gateway){
            ctx.font='13px Roboto'; ctx.fillStyle='#f59e0b';
            ctx.fillText('GW', pos.x, pos.y+20);
        }
        // node label above
        ctx.font='11px Roboto'; ctx.fillStyle=n.online?rssiColor(n.rssi||0):'#9ca3af';
        ctx.fillText(n.online?(n.rssi+'dBm'):'offline', pos.x, pos.y-36);
    });
}

// ── Tab switching ────────────────────────────────────────────────────────────
function switchTab(name){
    document.querySelectorAll('.tab').forEach((t,i)=>{
        const names=['nodes','routing','events'];
        t.classList.toggle('active', names[i]===name);
    });
    document.querySelectorAll('.tab-panel').forEach(p=>{
        p.classList.toggle('active', p.id==='tab-'+name);
    });
    if(name==='events') fetchEvents('');
}

// ── Routing history ──────────────────────────────────────────────────────────
async function fetchRouteHistory(){
    // populate dropdown from live nodes
    const sel = document.getElementById('routeNodeSel');
    const prev = sel.value;
    const opts = ['<option value="">-- Select --</option>'];
    nodesData.forEach(n=>{ opts.push(`<option value="${n.id}" ${n.id==prev?'selected':''}>${n.is_gateway?'[GW] ':''}Node ${n.id}</option>`); });
    sel.innerHTML = opts.join('');
    const node = sel.value;
    if(!node){ document.getElementById('routeTableBody').innerHTML='<tr><td colspan="10" style="color:#aaa;padding:20px;text-align:center;">Select a node above</td></tr>'; return; }
    try{
        const r = await fetch('/api/route_history?dest='+node+'&limit=50');
        const rows = await r.json();
        if(!rows.length){ document.getElementById('routeTableBody').innerHTML='<tr><td colspan="10" style="color:#aaa;">No data yet</td></tr>'; return; }

        function hopBadge(h){
            if(!h&&h!==0) return '-';
            const cls = h<=1?'hop-1':h===2?'hop-2':h===3?'hop-3':'hop-x';
            return `<span class="badge ${cls}">${h}</span>`;
        }
        function rtBadge(rt){
            if(!rt) return '-';
            const cls = rt==='direct'?'rt-direct':rt==='relay'?'rt-relay':'rt-multi';
            return `<span class="badge ${cls}">${rt}</span>`;
        }
        function rssiBar(v){
            if(v==null) return '-';
            const pct=rssiPct(v);
            const col=rssiColor(v);
            return `${v}dBm <span class="rssi-bar"><span class="rssi-fill" style="width:${pct}%;background:${col};"></span></span>`;
        }

        document.getElementById('routeTableBody').innerHTML = rows.map(r=>`
            <tr class="route-row">
                <td style="white-space:nowrap;font-size:11px;">${(r.timestamp||'').slice(11,19)}</td>
                <td>${r.room||'-'}</td>
                <td><b>N${r.origin_node||'?'}</b></td>
                <td>N${r.gateway_node||'?'}</td>
                <td>${r.next_hop ? 'N'+r.next_hop : '-'}</td>
                <td>${hopBadge(r.hop_count)}</td>
                <td>${rtBadge(r.route_type)}</td>
                <td>${r.ttl_remaining??'-'}</td>
                <td style="font-size:11px;color:#888;">${r.dest_seq??'-'}</td>
                <td>${rssiBar(r.rssi)}</td>
            </tr>`).join('');
    }catch(e){ document.getElementById('routeTableBody').innerHTML='<tr><td colspan="10" style="color:red;">Error: '+e.message+'</td></tr>'; }
}

// ── Mesh Events ──────────────────────────────────────────────────────────────
async function fetchEvents(type){
    try{
        const url = '/api/mesh_events?hours=1&limit=100'+(type?'&type='+type:'');
        const r = await fetch(url);
        const evs = await r.json();
        if(!evs.length){ document.getElementById('eventLog').innerHTML='<span style="color:#aaa;">No events in last 1h</span>'; return; }
        document.getElementById('eventLog').innerHTML = evs.map(e=>`
            <div class="ev-row">
                <span class="ev-time">${(e.timestamp||'').slice(11,19)}</span>
                <span class="ev-${e.event_type}" style="font-weight:700;min-width:36px;">${e.event_type}</span>
                <span>N${e.src_node??'?'}→N${e.dest_node??'?'}</span>
                ${e.via_node?`<span style="color:#aaa;">via N${e.via_node}</span>`:''}
                ${e.hop_count?`<span style="color:#888;">${e.hop_count}hop</span>`:''}
                ${e.rssi?`<span style="color:${rssiColor(e.rssi)}">${e.rssi}dBm</span>`:''}
            </div>`).join('');
    }catch(e){ document.getElementById('eventLog').innerHTML='<span style="color:red;">Error loading events</span>'; }
}

// ── Link Quality ─────────────────────────────────────────────────────────────
async function fetchLinkQuality(){
    try{
        const r = await fetch('/api/link_quality?hours=24');
        const links = await r.json();
        if(!links.length){ document.getElementById('linkTableBody').innerHTML='<tr><td colspan="6" style="color:#aaa;">No link data yet</td></tr>'; return; }
        const stateColor = {good:'#166534',marginal:'#854d0e',poor:'#991b1b',broken:'#6b7280',unknown:'#aaa'};
        document.getElementById('linkTableBody').innerHTML = links.map(l=>{
            const col = stateColor[l.link_state]||'#aaa';
            const pct = rssiPct(l.avg_rssi||0);
            return `<tr>
                <td><b>N${l.node_a}↔N${l.node_b}</b></td>
                <td style="color:${rssiColor(l.avg_rssi||0)};font-weight:700">${l.avg_rssi??'-'} dBm</td>
                <td style="color:#888;">${l.min_rssi??'-'}</td>
                <td style="color:#888;">${l.max_rssi??'-'}</td>
                <td><span style="color:${col};font-weight:700;">${l.link_state||'-'}</span></td>
                <td style="color:#aaa;">${l.sample_count}</td>
            </tr>`;
        }).join('');
    }catch(e){}
}

// ── Main fetch ───────────────────────────────────────────────────────────────
async function fetchData(){
    try{
        // nodes
        const nRes = await fetch('/api/nodes');
        nodesData = await nRes.json();
        drawTopology();

        let totalRssi=0, onlineCount=0;
        let nHtml = nodesData.sort((a,b)=>a.id-b.id).map(n=>{
            if(n.online){totalRssi+=n.rssi;onlineCount++;}
            const ago = n.last_seen_sec<60?n.last_seen_sec+'s':'~'+Math.floor(n.last_seen_sec/60)+'m';
            const rssiBar = n.online
                ? `<span style="color:${rssiColor(n.rssi)};font-weight:700">${n.rssi} dBm</span>`
                : `<span style="color:#aaa;font-size:11px;">${ago} ago</span>`;
            return `<tr>
                <td><b>N${n.id}</b></td>
                <td><span class="badge ${n.online?'bg-online':'bg-offline'}">${n.online?'ONLINE':'OFFLINE'}</span></td>
                <td>${n.is_gateway?'&#127760; Gateway':'Node'}</td>
                <td>${rssiBar}</td>
                <td>${n.neighbors&&n.neighbors.length?n.neighbors.map(x=>'N'+x).join(', '):'-'}</td>
                <td style="font-size:11px;color:#aaa;">${ago} ago</td>
            </tr>`;
        }).join('');
        document.getElementById('nodesTableBody').innerHTML = nHtml||'<tr><td colspan="6">No nodes</td></tr>';
        document.getElementById('statNodes').innerText = onlineCount;
        document.getElementById('statRssi').innerText  = onlineCount?Math.round(totalRssi/onlineCount)+' dBm':'-';

        // Latest RSSI — node ที่ last_seen_sec น้อยที่สุด (update ล่าสุด)
        const onlineNodes = nodesData.filter(n => n.online);
        if(onlineNodes.length){
            const latest = onlineNodes.reduce((a,b) => a.last_seen_sec < b.last_seen_sec ? a : b);
            document.getElementById('statRssiLatest').innerText = latest.rssi + ' dBm';
            document.getElementById('statRssiLatest').style.color = rssiColor(latest.rssi);
            document.getElementById('statRssiLatestNode').innerText = 'N'+latest.id+' · '+latest.last_seen_sec+'s ago';
        } else {
            document.getElementById('statRssiLatest').innerText = '-';
            document.getElementById('statRssiLatestNode').innerText = 'dBm';
        }

        // analytics (avg hop)
        const aRes = await fetch('/api/analytics');
        const analytics = await aRes.json();
        if(analytics.hop_distribution){
            const hops = analytics.hop_distribution;
            if(hops.length){
                const total = hops.reduce((s,h)=>s+h.count,0);
                const avg   = hops.reduce((s,h)=>s+h.hops*h.count,0)/total;
                document.getElementById('statHops').innerText = avg.toFixed(1);
            }
        }

        // users
        const uRes = await fetch('/api/active_users_list');
        const users = await uRes.json();
        document.getElementById('usersTableBody').innerHTML = users.length
            ? users.map(u=>`<tr>
                <td><span class="status-dot dot-online"></span>${u.username}</td>
                <td>N${u.node_id}</td>
                <td style="font-size:12px;">${u.login_time}</td>
              </tr>`).join('')
            : '<tr><td colspan="3" style="color:#aaa;">No users online</td></tr>';
        document.getElementById('statUsers').innerText = users.length;

        fetchLinkQuality();
        // update routing table node dropdown
        fetchRouteHistory();
        document.getElementById('lastUpdateText').innerText = new Date().toLocaleTimeString();
    }catch(e){ console.error('Sync error:',e); }
}

function kickUser(sid){
    if(!sid) return;
    fetch('/admin/api/kick_user',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({session_id:sid})})
    .then(()=>fetchData());
}

setInterval(fetchData, 5000);
fetchData();
</script>
</body>
</html>'''
HTML_USERS = '''<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>All Users - Admin</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }

        body {
            font-family: 'Roboto', -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
            background:
            font-size: 16px;                     /* ✅ same base size */
            color:
        }
        
        /* ===== Header (same theme as dashboard) ===== */
        .header {
            background:
            color: white;
            padding: 18px 30px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
        }
        .header h1 {
            font-size: 21px;                     /* ✅ same scale */
            font-weight: 700;
        }
        .header a {
            color: white;
            text-decoration: none;
            margin-left: 15px;
            font-size: 15px;
            border: 1px solid white;
            padding: 6px 14px;
            border-radius: 6px;
        }
        
        /* ===== Layout ===== */
        .container {
            display: flex;
            min-height: calc(100vh - 60px);
        }
        
        /* ===== Sidebar (same theme as dashboard) ===== */
        .sidebar {
            width: 250px;
            background: white;
            border-right: 1px solid
            padding: 20px 0;
        }
        .sidebar a {
            display: block;
            padding: 14px 30px;
            color:
            text-decoration: none;
            font-size: 16px;
            font-weight: 600;
            transition: 0.2s;
        }
        .sidebar a:hover,
        .sidebar a.active {
            background:
            color:
            border-left: 4px solid
        }
        
        /* ===== Content ===== */
        .content {
            flex: 1;
            padding: 28px;
        }
        
        .section {
            background: white;
            padding: 28px;
            border-radius: 14px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.06);
        }
        .section h2 {
            margin-bottom: 22px;
            font-size: 20px;                     /* ✅ dashboard section title size */
            font-weight: 700;
            color:
        }
        
        /* ===== Filters ===== */
        .filters {
            margin-bottom: 22px;
            display: flex;
            gap: 16px;
            align-items: center;
        }
        .filters input,
        .filters select {
            padding: 12px 14px;
            border: 1px solid
            border-radius: 10px;
            font-size: 15px;
        }
        .filters input {
            flex: 1;
        }
        .filters button {
            padding: 12px 22px;
            background:
            color: white;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            font-size: 15px;
            font-weight: 600;
        }
        .filters button:hover {
            opacity: 0.9;
        }
        
        /* ===== Table ===== */
        table {
            width: 100%;
            border-collapse: collapse;
        }
        th {
            padding: 15px;
            background:
            font-weight: 700;
            color:
            border-bottom: 2px solid
            font-size: 15px;
            text-align: center;
        }
        td {
            padding: 15px;
            border-bottom: 1px solid
            font-size: 16px;                     /* ✅ same as dashboard table */
            text-align: center;
            vertical-align: middle;
        }

        /* Username column: icon + text center */
        td:first-child {
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .status-dot {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            margin-right: 8px;
            display: inline-block;
        }
        .online { background:
        .offline { background:
        
        /* ===== Buttons ===== */
        .btn {
            padding: 8px 14px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 600;
            margin-right: 6px;
        }
        .btn-danger {
            background:
            color: white;
        }
        .btn-warning {
            background:
            color: white;
        }
        .btn-danger:hover { background:
        .btn-warning:hover { background:
    </style>
</head>
<body>
    <div class="header">
        <h1>LoRa Mesh Admin</h1>
        <div>
            <span>Welcome, {{ admin_user }}</span>
            <a href="/admin/logout">Logout</a>
        </div>
    </div>
    
    <div class="container">
        <div class="sidebar">
            <a href="/admin/dashboard">&
            <a href="/admin/users" class="active">&
            <a href="/admin/mesh/chatlog">&
            <a href="/admin/mesh/events">&
            <a href="/admin/mesh/links">&
            <a href="/admin/settings">&
        </div>
        
        <div class="content">
            <div class="section">
                <h2>All Registered Users</h2>
                
                <div class="filters">
                    <input type="text" id="searchInput" placeholder="Search username..." value="{{ search }}">
                    <select id="statusFilter">
                        <option value="all">All Status</option>
                        <option value="active">Active Only</option>
                        <option value="inactive">Inactive Only</option>                    </select>
                    <button onclick="applyFilters()">Search</button>
                </div>
                
                <table>
                    <thead>
                        <tr>
                            <th>Username</th>
                            <th>Account Status</th>
                            <th>Last Login</th>
                            <th>Actions</th>
                        </tr>
                    </thead>
                    <tbody>
                        {{ user_rows }}
                    </tbody>
                </table>
            </div>
        </div>
    </div>
    
    <script>
        function applyFilters() {
            const search = document.getElementById('searchInput').value;
            const status = document.getElementById('statusFilter').value;
            window.location.href = `/admin/users?search=${search}&status=${status}`;
        }
        
        async function deleteUser(userId, username) {
            if (!confirm(`Delete user "${username}"? This cannot be undone!`)) return;
            const res = await fetch('/admin/api/delete_user', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ user_id: userId })
            });
            if (res.ok) {
                alert('User deleted');
                location.reload();
            }
        }
    </script>
</body>
</html>'''

HTML_SETTINGS = '''<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Settings - Admin</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Arial, sans-serif; background:
        
        .header { background:
        .header h1 { font-size: 20px; font-weight: 700; }
        
        .container { display: flex; min-height: calc(100vh - 56px); }
        
        .sidebar { width: 220px; background: white; border-right: 1px solid
        .sidebar a { display: block; padding: 12px 24px; color:
        .sidebar a:hover, .sidebar a.active { background:
        
        .content { flex: 1; padding: 30px; max-width: 800px; }
        
        .section { background: white; padding: 25px; border-radius: 12px; box-shadow: 0 2px 8px rgba(0,0,0,0.05); margin-bottom: 20px; }
        .section h2 { margin-bottom: 20px; font-size: 18px; color:
        
        .form-group { margin-bottom: 20px; }
        .form-group label { display: block; margin-bottom: 8px; font-weight: 600; color:
        .form-group input { width: 100%; padding: 12px; border: 1px solid
        
        .btn { padding: 12px 24px; border: none; border-radius: 8px; cursor: pointer; font-size: 14px; font-weight: 600; }
        .btn-primary { background:
        .btn-danger { background:
        .btn:hover { opacity: 0.9; }
        
        .alert { padding: 15px; border-radius: 8px; margin-bottom: 20px; }
        .alert.success { background:
        .alert.error { background:
        
        .danger-zone { border: 2px solid
        .danger-zone h3 { color:
    </style>
</head>
<body>
    <div class="header">
        <h1>&
        <div>
            <span style="margin-right:12px;font-size:14px">Admin: <strong>{{ admin_user }}</strong></span>
            <a href="/admin/logout" style="color:white;text-decoration:none;font-size:13px;border:1px solid rgba(255,255,255,.5);padding:6px 14px;border-radius:6px;">Logout</a>
        </div>
    </div>
    
    <div class="container">
        <div class="sidebar">
            <a href="/admin/dashboard">&
            <a href="/admin/users">&
            <a href="/admin/mesh/chatlog">&
            <a href="/admin/mesh/events">&
            <a href="/admin/mesh/links">&
            <a href="/admin/settings" class="active">&
        </div>
        
        <div class="content">
            {{ message }}
            
            <div class="section">
                <h2><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:8px"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/><circle cx="12" cy="16" r="1"/><line x1="12" y1="17" x2="12" y2="19"/></svg>Change Admin Password</h2>
                <form method="POST">
                    <input type="hidden" name="action" value="change_password">
                    <div class="form-group">
                        <label>New Password</label>
                        <input type="password" name="new_password" required minlength="6">
                    </div>
                    <button type="submit" class="btn btn-primary">Update Password</button>
                </form>
            </div>
            
            <div class="section danger-zone">
                <h3><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#e74c3c" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:8px"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><circle cx="12" cy="17" r="0.5" fill="#e74c3c"/></svg>Danger Zone</h3>
                <p style="margin-bottom: 15px; color: #666;">This will delete ALL user accounts and logs. Admin account will remain.</p>
                <form method="POST" onsubmit="return confirm('Are you absolutely sure? This cannot be undone!')">
                    <input type="hidden" name="action" value="reset_db">
                    <div class="form-group">
                        <label>Type "DELETE ALL DATA" to confirm</label>
                        <input type="text" name="confirm" required>
                    </div>
                    <button type="submit" class="btn btn-danger">Reset Database</button>
                </form>
            </div>
        </div>
    </div>
</body>
</html>'''

HTML_PUBLIC = '''
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>LoRa Mesh Public Status</title>
    <link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;500;700&display=swap" rel="stylesheet">
    <style>
        :root { --primary:
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { font-family: 'Roboto', sans-serif; background: var(--bg); color:
        
        /* Navbar */
        .navbar { background: var(--primary); color: white; padding: 15px 30px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 2px 5px rgba(0,0,0,0.1); }
        .logo { font-weight: 700; font-size: 1.2rem; display: flex; align-items: center; gap: 10px; }
        .btn-login { text-decoration: none; background: rgba(255,255,255,0.15); color: white; padding: 8px 20px; border-radius: 4px; font-size: 0.9rem; transition: 0.2s; border: 1px solid rgba(255,255,255,0.2); }
        .btn-login:hover { background: rgba(255,255,255,0.25); }

        .container { max-width: 1200px; margin: 30px auto; padding: 0 20px; }
        
        /* Section Styles */
        .section { background: var(--card); padding: 25px; border-radius: 12px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.05); margin-bottom: 25px; }
        .section-title { font-size: 18px; font-weight: bold; margin-bottom: 20px; color: var(--primary); display: flex; align-items: center; gap: 10px; border-bottom: 2px solid
        
        /* Topology Map */
        .canvas-box { text-align: center; background:
        canvas { background-color:

        /* Tables */
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th { background:
        td { padding: 12px; text-align: center; border-bottom: 1px solid
        tr:last-child td { border-bottom: none; }
        
        .badge { padding: 4px 12px; border-radius: 20px; font-size: 12px; font-weight: 600; }
        .bg-online { background:
        .bg-offline { background:
        
        /* Layout Grid */
        .grid-2 { display: grid; grid-template-columns: 2fr 1fr; gap: 25px; }
        @media (max-width: 768px) { .grid-2 { grid-template-columns: 1fr; } }
    </style>
</head>
<body>
    <div class="navbar">
        <div class="logo"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:8px"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="0.5" fill="currentColor"/></svg>LoRa Mesh Network Public View</div>
        <a href="/admin/login" class="btn-login">🔑 Admin Login</a>
    </div>

    <div class="container">
        <div class="section">
            <div class="section-title"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:8px"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>Live Network Topology</div>
            <div class="canvas-box">
                <canvas id="netMap" width="850" height="400"></canvas>
            </div>
            <p style="text-align: center; color: #64748b; font-size: 0.9rem; margin-top: 10px;">
                แผนผังแสดงการเชื่อมต่อระหว่างโหนดในเครือข่ายแบบ Real-time
            </p>
        </div>

        <div class="grid-2">
            <div class="section">
                <div class="section-title"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:8px"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="0.5" fill="currentColor"/></svg>Nodes Status</div>                <table>
                    <thead>
                        <tr><th>Node ID</th><th>Status</th><th>Type</th><th>RSSI</th><th>Neighbors</th></tr>
                    </thead>
                    <tbody id="nodesTableBody">
                        <tr><td colspan="5">Loading data...</td></tr>
                    </tbody>
                </table>
            </div>

            <div class="section">
                <div class="section-title"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:middle;margin-right:8px"><line x1="18" y1="20" x2="18" y2="10"/><line x1="12" y1="20" x2="12" y2="4"/><line x1="6" y1="20" x2="6" y2="14"/></svg>Network Summary</div>
                <div style="text-align: center; padding: 20px;">
                    <h3 style="color: #64748b; font-size: 14px;">Total Active Nodes</h3>
                    <div id="statNodes" style="font-size: 48px; font-weight: bold; color: var(--primary);">0</div>
                    <hr style="border: 0; border-top: 1px solid #f1f5f9; margin: 20px 0;">
                    <h3 style="color: #64748b; font-size: 14px;">Avg. Signal (RSSI)</h3>
                    <div id="statRssi" style="font-size: 32px; font-weight: bold; color: var(--accent);">-</div>
                    <hr style="border: 0; border-top: 1px solid #f1f5f9; margin: 20px 0;">
                    <h3 style="color: #64748b; font-size: 14px;">Latest RSSI</h3>
                    <div id="statRssiLatest" style="font-size: 32px; font-weight: bold; color: var(--accent);">-</div>
                    <div id="statRssiLatestNode" style="font-size: 12px; color: #94a3b8; margin-top: 4px;">-</div>
                </div>
            </div>
        </div>
        
        <p style="text-align:center; color:#94a3b8; font-size:12px; margin-top:30px;">
            Last synced: <span id="lastUpdateText">-</span>
        </p>
    </div>

    <script>
        const canvas = document.getElementById('netMap');
        const ctx = canvas.getContext('2d');
        let nodesData = [];

        // กำหนดตำแหน่งตายตัวของ Node ตาม ID (เหมือนหน้า Admin)
        const fixedPos = {
            1: {x: 200, y: 100},
            2: {x: 650, y: 100},
            3: {x: 200, y: 300},
            4: {x: 650, y: 300},
            5: {x: 425, y: 200}
        };
        function getPos(id) {
            return fixedPos[id] || {x: 425 + (id*37)%200, y: 200 + (id*29)%150};
        }

        // ฟังก์ชันวาด Topology (ดึงมาจาก Admin Dashboard)
        function drawTopology() {
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            
            if (nodesData.length === 0) {
                ctx.fillStyle = '#94a3b8';
                ctx.font = '16px Roboto';
                ctx.textAlign = 'center';
                ctx.fillText('⏳ Waiting for node data...', canvas.width/2, canvas.height/2);
                return;
            }

            // 1. วาดเส้นเชื่อมต่อ (Connections)
            nodesData.forEach(node => {
                const startPos = getPos(node.id) || {x: 50 + (node.id*50)%800, y: 50 + (node.id*30)%350}; // Fallback position
                
                if (node.neighbors && Array.isArray(node.neighbors)) {
                    node.neighbors.forEach(nbId => {
                        const targetNode = nodesData.find(k => k.id == nbId);
                        if (targetNode) {
                            const endPos = getPos(nbId) || {x: 50 + (nbId*50)%800, y: 50 + (nbId*30)%350};
                            
                            ctx.beginPath();
                            ctx.moveTo(startPos.x, startPos.y);
                            ctx.lineTo(endPos.x, endPos.y);
                            
                            if (node.online && targetNode.online) {
                                ctx.strokeStyle = '#2c5aa0';
                                ctx.setLineDash([]);
                            } else {
                                ctx.strokeStyle = '#cbd5e0';
                                ctx.setLineDash([5, 5]);
                            }
                            ctx.lineWidth = 2;
                            ctx.stroke();
                        }
                    });
                }
            });

            // 2. วาดจุดโหนด (Nodes)
            nodesData.forEach(n => {
                const pos = getPos(n.id) || {x: 50 + (n.id*50)%800, y: 50 + (n.id*30)%350};
                
                ctx.beginPath();
                ctx.arc(pos.x, pos.y, 25, 0, Math.PI * 2);
                ctx.fillStyle = '#fff';
                ctx.fill();
                
                ctx.lineWidth = 3;
                ctx.strokeStyle = n.online ? '#2c5aa0' : '#cbd5e0';
                ctx.stroke();
                
                ctx.fillStyle = '#1a2a44';
                ctx.font = 'bold 14px Roboto';
                ctx.textAlign = 'center';
                ctx.fillText(n.id, pos.x, pos.y + 5);
                
                if (n.online) {
                    ctx.font = '10px Roboto';
                    ctx.fillStyle = '#2c5aa0';
                    ctx.fillText(n.rssi + " dBm", pos.x, pos.y - 35);
                }
            });
        }

        async function fetchData() {
            try {
                // เรียก API ดึงข้อมูล Node (ไม่ต้อง Login ก็ดึงได้)
                const response = await fetch('/api/nodes');
                if (!response.ok) throw new Error('Network error');
                
                nodesData = await response.json();
                
                // วาดกราฟ
                drawTopology();
                
                // อัปเดตตารางและสถิติ
                let tableHtml = "";
                let totalRssi = 0;
                let onlineCount = 0;

                // เรียงตาม ID น้อย -> มาก
                nodesData.sort((a,b) => a.id - b.id);

                nodesData.forEach(n => {
                    if(n.online) {
                        totalRssi += n.rssi;
                        onlineCount++;
                    }
                    
                    const statusBadge = n.online ? '<span class="badge bg-online">ONLINE</span>' : '<span class="badge bg-offline">OFFLINE</span>';
                    const neighborsStr = (n.neighbors && n.neighbors.length > 0) ? n.neighbors.join(', ') : '-';
                    const rssiStr = n.online ? n.rssi + ' dBm' : '-';

                    tableHtml += `
                        <tr>
                            <td><b>
                            <td>${statusBadge}</td>
                            <td>${n.is_gateway ? \'<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:middle;margin-right:4px"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>Gateway\' : \'<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:middle;margin-right:4px"><rect x="5" y="2" width="14" height="20" rx="2"/><line x1="12" y1="18" x2="12.01" y2="18"/></svg>Node\'}</td>
                            <td>${rssiStr}</td>
                            <td>${neighborsStr}</td>
                        </tr>
                    `;
                });

                document.getElementById('nodesTableBody').innerHTML = tableHtml || '<tr><td colspan="5">No nodes found</td></tr>';
                document.getElementById('statNodes').innerText = onlineCount;
                document.getElementById('statRssi').innerText = onlineCount > 0 ? Math.round(totalRssi/onlineCount) + " dBm" : "-";

                // Latest RSSI — node ที่ last_seen_sec น้อยที่สุด (update ล่าสุด)
                const onlineNodes = nodesData.filter(n => n.online);
                if(onlineNodes.length){
                    const latest = onlineNodes.reduce((a,b) => (a.last_seen_sec !== undefined && b.last_seen_sec !== undefined)
                        ? (a.last_seen_sec < b.last_seen_sec ? a : b) : a);
                    const latestEl = document.getElementById('statRssiLatest');
                    latestEl.innerText = latest.rssi + " dBm";
                    latestEl.style.color = latest.rssi >= -70 ? '#16a34a' : latest.rssi >= -85 ? '#d97706' : '#dc2626';
                    document.getElementById('statRssiLatestNode').innerText =
                        'Node #' + latest.id + (latest.last_seen_sec !== undefined ? ' · ' + latest.last_seen_sec + 's ago' : '');
                } else {
                    document.getElementById('statRssiLatest').innerText = "-";
                    document.getElementById('statRssiLatestNode').innerText = "-";
                }
                document.getElementById('lastUpdateText').innerText = new Date().toLocaleTimeString();

            } catch (e) {
                console.error("Error:", e);
                document.getElementById('nodesTableBody').innerHTML = '<tr><td colspan="5" style="color:red">Error loading data</td></tr>';
            }
        }

        // อัปเดตข้อมูลทุก 3 วินาที
        setInterval(fetchData, 3000);
        fetchData();
    </script>
</body>
</html>
'''
def udp_discovery_server():
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    udp_sock.bind(('', 5001))
    print("📡 UDP Discovery Server listening on port 5001...")
    
    while True:
        try:
            data, addr = udp_sock.recvfrom(1024)
            if data.decode('utf-8', errors='ignore').strip() == 'DISCOVER_LOG_SERVER':
                print(f"📡 Discovery request from {addr[0]}")
                udp_sock.sendto(b'I_AM_LOG_SERVER', addr)
        except Exception as e:
            print(f"❌ UDP Error: {e}")

if __name__ == '__main__':
    print("=" * 60)
    print("🔥 ADMIN SERVER STARTED (DEBUG SAFE MODE)")
    print("=" * 60)
    
    init_db()
    print("✅ Database initialized")

    udp_thread = threading.Thread(target=udp_discovery_server, daemon=True)
    udp_thread.start()
    
    print("📡 Starting server on http://localhost:5001")
    print("🔑 Default login: admin / admin123")
    print("=" * 60)

    app.run(
        host='0.0.0.0',
        port=5001,
        debug=False,
        use_reloader=False
    )