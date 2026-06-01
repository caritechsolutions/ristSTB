#!/usr/bin/env python3
"""
RIST Sender API
FastAPI backend for managing ristsender channels with multiple RIST outputs
"""

import os
import re
import sys
import yaml
import json
import time
import asyncio
import secrets
import hashlib
import logging
import subprocess
import psutil
import threading
from collections import deque
from pathlib import Path
from datetime import datetime, timedelta
from typing import Optional, List, Dict, Any
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException, Request, Response, Depends, status
from fastapi.responses import HTMLResponse, JSONResponse, FileResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# Configuration paths
CONFIG_FILE = os.environ.get('SENDER_CONFIG', '/etc/ristsender/sender_config.yaml')
SYSTEMD_DIR = '/etc/systemd/system'
PID_DIR = '/var/run/ristsender'
LOG_DIR = '/var/log/ristsender'
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')
STATS_DIR = '/dev/shm/ristsender-stats'

# Ensure directories exist
os.makedirs(os.path.dirname(CONFIG_FILE), exist_ok=True)
os.makedirs(PID_DIR, exist_ok=True)
os.makedirs(LOG_DIR, exist_ok=True)

# Setup logging (console only, no file logging)
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger('sender_api')

# =============================================================================
# In-Memory Stats Storage
# =============================================================================

STATS_HISTORY_SIZE = 60
stats_history: Dict[str, deque] = {}
stats_history_lock = threading.Lock()

# Interface bandwidth history (for server stats page)
INTERFACE_HISTORY_SIZE = 3600  # 1 hour at 1 sample per second
INTERFACE_HISTORY_FILE = '/var/lib/ristsender/interface_history.json'
interface_history: Dict[str, deque] = {}
interface_prev_stats: Dict[str, Dict] = {}
interface_history_lock = threading.Lock()
interface_collector_thread = None
interface_collector_running = False

def init_interface_history():
    """Load interface history from file if exists"""
    global interface_history
    os.makedirs(os.path.dirname(INTERFACE_HISTORY_FILE), exist_ok=True)
    if os.path.exists(INTERFACE_HISTORY_FILE):
        try:
            with open(INTERFACE_HISTORY_FILE, 'r') as f:
                data = json.load(f)
                for iface, points in data.items():
                    interface_history[iface] = deque(points, maxlen=INTERFACE_HISTORY_SIZE)
            logger.info(f"Loaded interface history for {len(interface_history)} interfaces")
        except Exception as e:
            logger.warning(f"Failed to load interface history: {e}")

def save_interface_history():
    """Save interface history to file"""
    try:
        with interface_history_lock:
            data = {iface: list(points) for iface, points in interface_history.items()}
        with open(INTERFACE_HISTORY_FILE, 'w') as f:
            json.dump(data, f)
    except Exception as e:
        logger.warning(f"Failed to save interface history: {e}")

def collect_interface_stats():
    """Background thread to collect interface statistics"""
    global interface_collector_running, interface_prev_stats
    last_save = time.time()

    while interface_collector_running:
        try:
            net_io = psutil.net_io_counters(pernic=True)
            net_if_stats = psutil.net_if_stats()
            now = time.time()

            with interface_history_lock:
                for iface, io in net_io.items():
                    # Skip virtual interfaces
                    if iface.startswith('lo') or iface.startswith('docker') or \
                       iface.startswith('br-') or iface.startswith('veth'):
                        continue

                    stats = net_if_stats.get(iface)
                    if not stats or not stats.isup:
                        continue

                    # Calculate rates
                    rx_rate = 0
                    tx_rate = 0
                    if iface in interface_prev_stats:
                        prev = interface_prev_stats[iface]
                        time_delta = now - prev['timestamp']
                        if time_delta > 0:
                            rx_rate = (io.bytes_recv - prev['bytes_recv']) / time_delta
                            tx_rate = (io.bytes_sent - prev['bytes_sent']) / time_delta

                    # Store current stats for next calculation
                    interface_prev_stats[iface] = {
                        'timestamp': now,
                        'bytes_recv': io.bytes_recv,
                        'bytes_sent': io.bytes_sent
                    }

                    # Initialize history deque if needed
                    if iface not in interface_history:
                        interface_history[iface] = deque(maxlen=INTERFACE_HISTORY_SIZE)

                    # Add data point
                    interface_history[iface].append({
                        'timestamp': now,
                        'rx_rate': rx_rate,
                        'tx_rate': tx_rate,
                        'rx_bytes': io.bytes_recv,
                        'tx_bytes': io.bytes_sent
                    })

            # Save to file every 60 seconds
            if now - last_save > 60:
                save_interface_history()
                last_save = now

        except Exception as e:
            logger.error(f"Interface stats collection error: {e}")

        time.sleep(1)

    # Save on shutdown
    save_interface_history()
    logger.info("Interface stats collector stopped")

def start_interface_collector():
    """Start the background interface stats collector"""
    global interface_collector_thread, interface_collector_running
    interface_collector_running = True
    interface_collector_thread = threading.Thread(target=collect_interface_stats, daemon=True)
    interface_collector_thread.start()
    logger.info("Interface stats collector started")

def stop_interface_collector():
    """Stop the background interface stats collector"""
    global interface_collector_running
    interface_collector_running = False

# Channel stats history (persisted to disk)
CHANNEL_HISTORY_SIZE = 1800  # 1 hour at 2 seconds per sample
CHANNEL_HISTORY_FILE = '/var/lib/ristsender/channel_history.json'
channel_history: Dict[str, deque] = {}
channel_history_lock = threading.Lock()
channel_collector_thread = None
channel_collector_running = False

def init_channel_history():
    """Load channel history from file if exists"""
    global channel_history
    os.makedirs(os.path.dirname(CHANNEL_HISTORY_FILE), exist_ok=True)
    if os.path.exists(CHANNEL_HISTORY_FILE):
        try:
            with open(CHANNEL_HISTORY_FILE, 'r') as f:
                data = json.load(f)
                for ch_id, points in data.items():
                    channel_history[ch_id] = deque(points, maxlen=CHANNEL_HISTORY_SIZE)
            logger.info(f"Loaded channel history for {len(channel_history)} channels")
        except Exception as e:
            logger.warning(f"Failed to load channel history: {e}")

def save_channel_history():
    """Save channel history to file"""
    try:
        with channel_history_lock:
            data = {ch_id: list(points) for ch_id, points in channel_history.items()}
        with open(CHANNEL_HISTORY_FILE, 'w') as f:
            json.dump(data, f)
    except Exception as e:
        logger.warning(f"Failed to save channel history: {e}")

def collect_channel_stats():
    """Background thread to collect channel statistics"""
    global channel_collector_running
    last_save = time.time()

    while channel_collector_running:
        try:
            config = load_config()
            channels = config.get('channels', {})
            now = time.time()

            with channel_history_lock:
                for ch_id in channels:
                    stats = get_channel_stats(ch_id)

                    # Initialize history deque if needed
                    if ch_id not in channel_history:
                        channel_history[ch_id] = deque(maxlen=CHANNEL_HISTORY_SIZE)

                    # Add data point with essential stats
                    channel_history[ch_id].append({
                        'timestamp': now,
                        'bandwidth_bps': stats.get('bandwidth_bps', 0),
                        'quality': stats.get('quality', 100),
                        'rtt': stats.get('timing', {}).get('rtt', 0),
                        'sent': stats.get('packets', {}).get('sent', 0),
                        'retransmitted': stats.get('packets', {}).get('retransmitted', 0)
                    })

                # Clean up history for deleted channels
                active_ids = set(channels.keys())
                for ch_id in list(channel_history.keys()):
                    if ch_id not in active_ids:
                        del channel_history[ch_id]

            # Save to file every 60 seconds
            if now - last_save > 60:
                save_channel_history()
                last_save = now

        except Exception as e:
            logger.error(f"Channel stats collection error: {e}")

        time.sleep(2)  # Collect every 2 seconds

    # Save on shutdown
    save_channel_history()
    logger.info("Channel stats collector stopped")

def start_channel_collector():
    """Start the background channel stats collector"""
    global channel_collector_thread, channel_collector_running
    channel_collector_running = True
    channel_collector_thread = threading.Thread(target=collect_channel_stats, daemon=True)
    channel_collector_thread.start()
    logger.info("Channel stats collector started")

def stop_channel_collector():
    """Stop the background channel stats collector"""
    global channel_collector_running
    channel_collector_running = False
    if interface_collector_thread:
        interface_collector_thread.join(timeout=2)

# =============================================================================
# Pydantic Models
# =============================================================================

class OutputPeer(BaseModel):
    url: str
    name: Optional[str] = ""
    enabled: bool = True

class ChannelSettings(BaseModel):
    profile: int = Field(1, ge=0, le=2, description="RIST profile (0=simple, 1=main, 2=advanced)")
    buffer: int = Field(1000, ge=100, le=10000, description="Buffer size in milliseconds")
    virt_dst_port: Optional[int] = Field(None, description="Virtual destination port")
    encryption_type: Optional[int] = Field(None, description="Encryption type (128 or 256)")
    secret: Optional[str] = Field(None, description="Encryption secret")
    bandwidth: Optional[int] = Field(None, description="Bandwidth limit in Kbps")
    congestion_control: Optional[int] = Field(None, description="Congestion control (0=disabled, 1=normal, 2=aggressive)")

class Channel(BaseModel):
    name: str
    enabled: bool = True
    input: str  # UDP input URL
    outputs: List[OutputPeer]  # Multiple RIST outputs
    settings: ChannelSettings = ChannelSettings()
    metrics_port: Optional[int] = None
    group: Optional[str] = None

class LoginRequest(BaseModel):
    password: str

class PasswordChange(BaseModel):
    current_password: str
    new_password: str

# =============================================================================
# Configuration Management
# =============================================================================

def load_config() -> dict:
    """Load configuration from YAML file"""
    if not os.path.exists(CONFIG_FILE):
        default_config = {
            'channels': {},
            'system': {
                'log_level': 6,
                'data_dir': '/var/lib/ristsender',
                'pid_dir': PID_DIR
            }
        }
        save_config(default_config)
        return default_config

    with open(CONFIG_FILE, 'r') as f:
        return yaml.safe_load(f) or {'channels': {}, 'system': {}}

def save_config(config: dict):
    """Save configuration to YAML file"""
    with open(CONFIG_FILE, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

def get_next_channel_id(config: dict) -> str:
    """Generate next channel ID"""
    existing = [int(k.replace('channel', '')) for k in config.get('channels', {}).keys()
                if k.startswith('channel') and k.replace('channel', '').isdigit()]
    next_num = max(existing, default=0) + 1
    return f'channel{next_num}'

def get_next_metrics_port(config: dict) -> int:
    """Find next available metrics port starting from 6001"""
    used_ports = set()
    for ch in config.get('channels', {}).values():
        if ch.get('metrics_port'):
            used_ports.add(ch['metrics_port'])
    port = 6001
    while port in used_ports:
        port += 1
    return port

# =============================================================================
# Service Management
# =============================================================================

def get_channel_status(channel_id: str) -> str:
    """Get the status of a channel service"""
    try:
        result = subprocess.run(
            ['systemctl', 'is-active', f'ristsender-{channel_id}.service'],
            capture_output=True, text=True
        )
        return 'running' if result.stdout.strip() == 'active' else 'stopped'
    except Exception:
        return 'error'

def generate_service_file(channel_id: str, channel: dict):
    """Generate systemd service file for a ristsender channel"""

    # Build output URLs with settings (comma-separated in single -o argument)
    output_urls = []
    for output in channel.get('outputs', []):
        if output.get('enabled', True):
            url = output['url']
            # Add virt_dst_port to URL if specified
            if channel['settings'].get('virt_dst_port'):
                sep = '&' if '?' in url else '?'
                url = f"{url}{sep}virt-dst-port={channel['settings']['virt_dst_port']}"
            if channel['settings'].get('congestion_control') is not None:
                sep = '&' if '?' in url else '?'
                url = f"{url}{sep}congestion-control={channel['settings']['congestion_control']}"
            if channel['settings'].get('bandwidth'):
                sep = '&' if '?' in url else '?'
                url = f"{url}{sep}bandwidth={channel['settings']['bandwidth']}"
            output_urls.append(url)

    if not output_urls:
        logger.warning(f"No enabled outputs for channel {channel_id}")
        return

    # Build command
    cmd_parts = [
        '/usr/local/bin/ristsender',
        f"-p {channel['settings'].get('profile', 1)}",
        f"-i '{channel['input']}'",
        '-n',  # Null packets
        f"-o '{','.join(output_urls)}'",  # Comma-separated outputs in single -o
        '-v 6',
        '-M',  # Metrics HTTP
        f"--metrics-port={channel['metrics_port']}",
        '-S 1000',  # Stats interval
    ]

    # Buffer
    if channel['settings'].get('buffer'):
        cmd_parts.append(f"-b {channel['settings']['buffer']}")

    # Encryption
    if channel['settings'].get('secret'):
        cmd_parts.append(f"-s '{channel['settings']['secret']}'")
    if channel['settings'].get('encryption_type'):
        cmd_parts.append(f"-e {channel['settings']['encryption_type']}")

    service_content = f"""[Unit]
Description=RIST Sender Channel {channel.get('name', channel_id)}
After=network.target

[Service]
Type=simple
ExecStart=/bin/bash -c 'exec {' '.join(cmd_parts)}'
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal
SyslogIdentifier=ristsender-{channel_id}

[Install]
WantedBy=multi-user.target
"""

    service_file = f"{SYSTEMD_DIR}/ristsender-{channel_id}.service"
    with open(service_file, 'w') as f:
        f.write(service_content)

    # Reload systemd
    subprocess.run(['systemctl', 'daemon-reload'], check=True, capture_output=True)

    # Enable service if channel is enabled
    if channel.get('enabled', True):
        subprocess.run(['systemctl', 'enable', f'ristsender-{channel_id}.service'],
                      check=True, capture_output=True)

def delete_service_file(channel_id: str):
    """Delete systemd service file"""
    service_name = f'ristsender-{channel_id}.service'
    try:
        subprocess.run(['systemctl', 'stop', service_name], capture_output=True)
        subprocess.run(['systemctl', 'disable', service_name], capture_output=True)
    except Exception:
        pass

    service_file = f"{SYSTEMD_DIR}/{service_name}"
    if os.path.exists(service_file):
        os.remove(service_file)

    subprocess.run(['systemctl', 'daemon-reload'], capture_output=True)

# =============================================================================
# Stats Parsing
# =============================================================================

def parse_sender_stats(stats_text: str) -> dict:
    """Parse ristsender stats from journal output (JSON format).

    Groups peers by cname (client) and shows individual peer stats for each.
    Structure: clients[] -> each has cname + peers[] with individual stats.
    """
    stats = {
        'quality': 100.0,
        'bandwidth_bps': 0,
        'retry_bandwidth_bps': 0,
        'packets': {
            'sent': 0,
            'received': 0,
            'retransmitted': 0,
            'lost': 0
        },
        'timing': {
            'rtt': 0,
            'avg_rtt': 0,
        },
        'clients': [],  # Grouped by cname, each with individual peer stats
        'peers': []     # Flat list for backward compatibility
    }

    if not stats_text:
        return stats

    # Collect all peer entries, keyed by (cname, id)
    peer_entries = {}

    for line in stats_text.strip().split('\n'):
        if 'sender-stats' not in line:
            continue

        try:
            # Extract JSON from the line - format: timestamp|level|[INFO] {"sender-stats":...}
            json_start = line.find('{')
            if json_start == -1:
                continue

            json_str = line[json_start:]
            data = json.loads(json_str)

            if 'sender-stats' not in data:
                continue

            peer_data = data['sender-stats'].get('peer', {})
            if not peer_data:
                continue

            cname = peer_data.get('cname', 'unknown')
            peer_id = peer_data.get('id', 0)
            peer_stats = peer_data.get('stats', {})

            # Store/update this peer entry (keeps most recent)
            peer_entries[(cname, peer_id)] = {
                'id': peer_id,
                'cname': cname,
                'flow_id': peer_data.get('flow_id', 0),
                'quality': peer_stats.get('quality', 100),
                'sent': peer_stats.get('sent', 0),
                'received': peer_stats.get('received', 0),
                'retransmitted': peer_stats.get('retransmitted', 0),
                'bandwidth_bps': peer_stats.get('bandwidth', 0),
                'retry_bandwidth_bps': peer_stats.get('retry_bandwidth', 0),
                'rtt': peer_stats.get('rtt', 0),
                'avg_rtt': peer_stats.get('avg_rtt', 0),
            }

        except json.JSONDecodeError:
            continue
        except Exception as e:
            logger.debug(f"Failed to parse stats line: {e}")
            continue

    # Group by cname
    cname_groups = {}
    for (cname, peer_id), entry in peer_entries.items():
        if cname not in cname_groups:
            cname_groups[cname] = []
        cname_groups[cname].append(entry)

    # Build client list with individual peer stats
    for cname in sorted(cname_groups.keys()):
        peers = cname_groups[cname]
        if not peers:
            continue

        # Sort peers by ID for consistent display
        peers_sorted = sorted(peers, key=lambda p: p['id'])

        # Calculate totals for this client
        total_bandwidth = sum(p['bandwidth_bps'] for p in peers_sorted)
        total_retry_bw = sum(p['retry_bandwidth_bps'] for p in peers_sorted)
        total_sent = sum(p['sent'] for p in peers_sorted)
        total_received = sum(p['received'] for p in peers_sorted)
        total_retransmitted = sum(p['retransmitted'] for p in peers_sorted)
        min_quality = min(p['quality'] for p in peers_sorted)
        avg_rtt = sum(p['avg_rtt'] for p in peers_sorted) / len(peers_sorted)

        client = {
            'cname': cname,
            'peers': peers_sorted,
            'totals': {
                'quality': min_quality,
                'bandwidth_bps': total_bandwidth,
                'retry_bandwidth_bps': total_retry_bw,
                'sent': total_sent,
                'received': total_received,
                'retransmitted': total_retransmitted,
                'rtt': avg_rtt
            }
        }
        stats['clients'].append(client)

        # Update global totals
        stats['bandwidth_bps'] += total_bandwidth
        stats['retry_bandwidth_bps'] += total_retry_bw
        stats['packets']['sent'] += total_sent
        stats['packets']['received'] += total_received
        stats['packets']['retransmitted'] += total_retransmitted
        if min_quality < stats['quality']:
            stats['quality'] = min_quality

        # Also add to flat peers list for backward compat
        stats['peers'].append({
            'cname': cname,
            'id': cname,
            'quality': min_quality,
            'bandwidth_bps': total_bandwidth,
            'retry_bandwidth_bps': total_retry_bw,
            'packets': {'sent': total_sent, 'received': total_received, 'retransmitted': total_retransmitted},
            'rtt': avg_rtt,
            'connection_count': len(peers_sorted)
        })

    # Calculate overall average RTT
    if stats['clients']:
        all_rtts = [c['totals']['rtt'] for c in stats['clients']]
        stats['timing']['rtt'] = max(all_rtts)
        stats['timing']['avg_rtt'] = sum(all_rtts) / len(all_rtts)

    return stats

def get_channel_stats(channel_id: str) -> dict:
    """Get stats for a channel from shared memory"""
    stats_file = f"{STATS_DIR}/ristsender-{channel_id}.txt"

    try:
        if os.path.exists(stats_file):
            with open(stats_file, 'r') as f:
                stats_text = f.read()
            return parse_sender_stats(stats_text)
    except Exception as e:
        logger.error(f"Failed to read stats for {channel_id}: {e}")

    return parse_sender_stats('')

# =============================================================================
# Authentication
# =============================================================================

SESSIONS: Dict[str, datetime] = {}
SESSION_TIMEOUT = timedelta(hours=24)
DEFAULT_PASSWORD = 'admin'

def hash_password(password: str) -> str:
    """Hash password with SHA-256"""
    return hashlib.sha256(password.encode()).hexdigest()

def get_password_hash() -> str:
    """Get password hash from config or use default"""
    config = load_config()
    return config.get('system', {}).get('password_hash',
           hashlib.sha256(DEFAULT_PASSWORD.encode()).hexdigest())

def verify_session(session_id: str) -> bool:
    """Verify session is valid"""
    if not session_id or session_id not in SESSIONS:
        return False
    if datetime.now() - SESSIONS[session_id] > SESSION_TIMEOUT:
        del SESSIONS[session_id]
        return False
    return True

async def require_auth(request: Request):
    """Dependency to require authentication"""
    session_id = request.cookies.get('session_id')
    if not verify_session(session_id):
        raise HTTPException(status_code=401, detail="Not authenticated")
    return session_id

# =============================================================================
# System Health
# =============================================================================

def get_system_health() -> dict:
    """Get system health metrics"""
    try:
        cpu_percent = psutil.cpu_percent(interval=0.1)
        memory = psutil.virtual_memory()
        disk = psutil.disk_usage('/')

        # Network stats
        net_io = psutil.net_io_counters()

        return {
            'cpu': {
                'percent': cpu_percent,
                'cores': psutil.cpu_count()
            },
            'memory': {
                'total': memory.total,
                'used': memory.used,
                'percent': memory.percent
            },
            'disk': {
                'total': disk.total,
                'used': disk.used,
                'percent': disk.percent
            },
            'network': {
                'bytes_sent': net_io.bytes_sent,
                'bytes_recv': net_io.bytes_recv
            }
        }
    except Exception as e:
        logger.error(f"Failed to get system health: {e}")
        return {}

# =============================================================================
# FastAPI App
# =============================================================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    """Startup and shutdown events"""
    logger.info("RIST Sender API starting...")
    init_interface_history()
    init_channel_history()
    start_interface_collector()
    start_channel_collector()
    yield
    logger.info("RIST Sender API shutting down...")
    stop_interface_collector()
    stop_channel_collector()

app = FastAPI(title="RIST Sender API", lifespan=lifespan)

# CORS middleware
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Cache-control middleware - prevent browser caching of API responses
@app.middleware("http")
async def add_cache_control(request: Request, call_next):
    """Prevent browser caching of API responses to avoid stale auth state"""
    response = await call_next(request)
    if request.url.path.startswith("/api/"):
        response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
        response.headers["Pragma"] = "no-cache"
    return response

# =============================================================================
# Auth Routes
# =============================================================================

@app.post("/api/login")
async def login(request: LoginRequest, response: Response):
    """Login endpoint"""
    password_hash = hashlib.sha256(request.password.encode()).hexdigest()
    if password_hash != get_password_hash():
        raise HTTPException(status_code=401, detail="Invalid password")

    session_id = secrets.token_urlsafe(32)
    SESSIONS[session_id] = datetime.now()
    response.set_cookie(key="session_id", value=session_id, httponly=True, samesite="lax")
    return {"success": True}

@app.post("/api/logout")
async def logout(response: Response, session_id: str = Depends(require_auth)):
    """Logout endpoint"""
    if session_id in SESSIONS:
        del SESSIONS[session_id]
    response.delete_cookie("session_id")
    return {"success": True}

@app.get("/api/auth/status")
async def auth_status(request: Request):
    """Check authentication status"""
    session_id = request.cookies.get('session_id')
    return {"authenticated": verify_session(session_id)}

# =============================================================================
# Channel Routes
# =============================================================================

@app.get("/api/channels")
async def list_channels(session_id: str = Depends(require_auth)):
    """List all channels"""
    config = load_config()
    channels = config.get('channels', {})

    # Add runtime status
    for ch_id, ch in channels.items():
        ch['status'] = get_channel_status(ch_id)

    return {"channels": channels}

@app.get("/api/channels/{channel_id}")
async def get_channel(channel_id: str, session_id: str = Depends(require_auth)):
    """Get a specific channel"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    channel = config['channels'][channel_id]
    channel['status'] = get_channel_status(channel_id)
    return channel

@app.post("/api/channels")
async def create_channel(channel: Channel, session_id: str = Depends(require_auth)):
    """Create a new channel"""
    config = load_config()
    if 'channels' not in config:
        config['channels'] = {}

    channel_id = get_next_channel_id(config)

    channel_dict = channel.dict()
    if not channel_dict.get('metrics_port'):
        channel_dict['metrics_port'] = get_next_metrics_port(config)

    channel_dict['status'] = 'stopped'
    channel_dict['process_id'] = None
    channel_dict['last_error'] = None

    config['channels'][channel_id] = channel_dict
    save_config(config)

    # Generate service file
    generate_service_file(channel_id, channel_dict)

    return {"channel_id": channel_id, **channel_dict}

@app.put("/api/channels/{channel_id}")
async def update_channel(channel_id: str, channel: Channel, session_id: str = Depends(require_auth)):
    """Update a channel"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    # Preserve some fields
    existing = config['channels'][channel_id]
    channel_dict = channel.dict()
    channel_dict['metrics_port'] = existing.get('metrics_port', channel_dict.get('metrics_port'))
    channel_dict['status'] = existing.get('status', 'stopped')
    channel_dict['process_id'] = existing.get('process_id')
    channel_dict['last_error'] = existing.get('last_error')

    config['channels'][channel_id] = channel_dict
    save_config(config)

    # Regenerate service file
    generate_service_file(channel_id, channel_dict)

    # Restart if running
    if get_channel_status(channel_id) == 'running':
        subprocess.run(['systemctl', 'restart', f'ristsender-{channel_id}.service'],
                      capture_output=True)

    return channel_dict

@app.delete("/api/channels/{channel_id}")
async def delete_channel(channel_id: str, session_id: str = Depends(require_auth)):
    """Delete a channel"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    # Stop and remove service
    delete_service_file(channel_id)

    del config['channels'][channel_id]
    save_config(config)

    return {"success": True}

@app.post("/api/channels/{channel_id}/start")
async def start_channel(channel_id: str, session_id: str = Depends(require_auth)):
    """Start a channel"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    # Ensure service file exists
    service_file = f"{SYSTEMD_DIR}/ristsender-{channel_id}.service"
    if not os.path.exists(service_file):
        generate_service_file(channel_id, config['channels'][channel_id])

    try:
        subprocess.run(['systemctl', 'start', f'ristsender-{channel_id}.service'],
                      check=True, capture_output=True)
        return {"status": "started"}
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to start: {e.stderr.decode()}")

@app.post("/api/channels/{channel_id}/stop")
async def stop_channel(channel_id: str, session_id: str = Depends(require_auth)):
    """Stop a channel"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    try:
        subprocess.run(['systemctl', 'stop', f'ristsender-{channel_id}.service'],
                      check=True, capture_output=True)
        return {"status": "stopped"}
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to stop: {e.stderr.decode()}")

@app.post("/api/channels/{channel_id}/restart")
async def restart_channel(channel_id: str, session_id: str = Depends(require_auth)):
    """Restart a channel"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    try:
        subprocess.run(['systemctl', 'restart', f'ristsender-{channel_id}.service'],
                      check=True, capture_output=True)
        return {"status": "restarted"}
    except subprocess.CalledProcessError as e:
        raise HTTPException(status_code=500, detail=f"Failed to restart: {e.stderr.decode()}")

@app.get("/api/channels/{channel_id}/stats")
async def channel_stats(channel_id: str, session_id: str = Depends(require_auth)):
    """Get channel stats"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    stats = get_channel_stats(channel_id)
    return stats

@app.get("/api/channels/{channel_id}/stats/history")
async def channel_stats_history(channel_id: str, minutes: int = 60, session_id: str = Depends(require_auth)):
    """Get channel stats history for graphing"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    minutes = min(minutes, 60)  # Max 1 hour
    cutoff = time.time() - (minutes * 60)

    with channel_history_lock:
        if channel_id in channel_history:
            history = [p for p in channel_history[channel_id] if p['timestamp'] >= cutoff]
        else:
            history = []

    return {"channel_id": channel_id, "history": history}

@app.get("/api/channels/{channel_id}/logs")
async def channel_logs(channel_id: str, lines: int = 100, session_id: str = Depends(require_auth)):
    """Get channel logs"""
    config = load_config()
    if channel_id not in config.get('channels', {}):
        raise HTTPException(status_code=404, detail="Channel not found")

    try:
        result = subprocess.run(
            ['journalctl', '-u', f'ristsender-{channel_id}.service', '-n', str(lines), '--no-pager'],
            capture_output=True, text=True
        )
        return {"logs": result.stdout.split('\n')}
    except Exception as e:
        return {"logs": [], "error": str(e)}

# =============================================================================
# Bulk Operations
# =============================================================================

@app.post("/api/channels/start-all")
async def start_all_channels(session_id: str = Depends(require_auth)):
    """Start all enabled channels"""
    config = load_config()
    started = []
    for ch_id, ch in config.get('channels', {}).items():
        if ch.get('enabled', True):
            try:
                subprocess.run(['systemctl', 'start', f'ristsender-{ch_id}.service'],
                              check=True, capture_output=True)
                started.append(ch_id)
            except Exception:
                pass
    return {"started": started}

@app.post("/api/channels/stop-all")
async def stop_all_channels(session_id: str = Depends(require_auth)):
    """Stop all channels"""
    config = load_config()
    stopped = []
    for ch_id in config.get('channels', {}).keys():
        try:
            subprocess.run(['systemctl', 'stop', f'ristsender-{ch_id}.service'],
                          check=True, capture_output=True)
            stopped.append(ch_id)
        except Exception:
            pass
    return {"stopped": stopped}

# =============================================================================
# System Routes
# =============================================================================

@app.get("/api/system/health")
async def system_health(session_id: str = Depends(require_auth)):
    """Get system health metrics"""
    return get_system_health()

@app.get("/api/system/info")
async def system_info(session_id: str = Depends(require_auth)):
    """Get detailed system information"""
    import platform

    # CPU info
    cpu_count = psutil.cpu_count(logical=False) or 1
    cpu_count_logical = psutil.cpu_count(logical=True) or 1
    cpu_freq = psutil.cpu_freq()
    cpu_percent_per_core = psutil.cpu_percent(interval=0.1, percpu=True)

    # Memory
    memory = psutil.virtual_memory()
    swap = psutil.swap_memory()

    # Disk
    disk = psutil.disk_usage('/')

    # Network interfaces
    net_if_addrs = psutil.net_if_addrs()
    net_if_stats = psutil.net_if_stats()
    net_io_counters = psutil.net_io_counters(pernic=True)

    # Get latest rates from the background collector
    latest_rates = {}
    with interface_history_lock:
        for iface, history in interface_history.items():
            if history:
                latest = history[-1]
                latest_rates[iface] = {
                    'rx_rate': latest.get('rx_rate', 0),
                    'tx_rate': latest.get('tx_rate', 0)
                }

    interfaces = {}
    for iface, addrs in net_if_addrs.items():
        if iface.startswith('lo') or iface.startswith('docker') or iface.startswith('br-') or iface.startswith('veth'):
            continue

        stats = net_if_stats.get(iface, None)
        io = net_io_counters.get(iface, None)

        ipv4 = None
        ipv6 = None
        mac = None
        for addr in addrs:
            if addr.family.name == 'AF_INET':
                ipv4 = addr.address
            elif addr.family.name == 'AF_INET6' and not addr.address.startswith('fe80'):
                ipv6 = addr.address
            elif addr.family.name == 'AF_PACKET':
                mac = addr.address

        rates = latest_rates.get(iface, {'rx_rate': 0, 'tx_rate': 0})

        interfaces[iface] = {
            'ipv4': ipv4,
            'ipv6': ipv6,
            'mac': mac,
            'is_up': stats.isup if stats else False,
            'speed': stats.speed if stats else 0,
            'mtu': stats.mtu if stats else 0,
            'bytes_sent': io.bytes_sent if io else 0,
            'bytes_recv': io.bytes_recv if io else 0,
            'packets_sent': io.packets_sent if io else 0,
            'packets_recv': io.packets_recv if io else 0,
            'errors_in': io.errin if io else 0,
            'errors_out': io.errout if io else 0,
            'drops_in': io.dropin if io else 0,
            'drops_out': io.dropout if io else 0,
            'rx_rate': rates['rx_rate'],
            'tx_rate': rates['tx_rate']
        }

    # System uptime
    boot_time = psutil.boot_time()
    uptime_seconds = time.time() - boot_time

    # Load average
    try:
        load_avg = os.getloadavg()
    except:
        load_avg = (0, 0, 0)

    return {
        "hostname": os.uname().nodename,
        "platform": platform.system(),
        "platform_release": platform.release(),
        "platform_version": platform.version(),
        "architecture": platform.machine(),
        "uptime_seconds": uptime_seconds,
        "load_average": {
            "1min": load_avg[0],
            "5min": load_avg[1],
            "15min": load_avg[2]
        },
        "cpu": {
            "cores_physical": cpu_count,
            "cores_logical": cpu_count_logical,
            "frequency_mhz": cpu_freq.current if cpu_freq else 0,
            "frequency_max_mhz": cpu_freq.max if cpu_freq else 0,
            "percent": psutil.cpu_percent(interval=0),
            "percent_per_core": cpu_percent_per_core
        },
        "memory": {
            "total": memory.total,
            "available": memory.available,
            "used": memory.used,
            "percent": memory.percent
        },
        "swap": {
            "total": swap.total,
            "used": swap.used,
            "free": swap.free,
            "percent": swap.percent
        },
        "disk": {
            "total": disk.total,
            "used": disk.used,
            "free": disk.free,
            "percent": disk.percent
        },
        "interfaces": interfaces,
        "timestamp": datetime.now().isoformat()
    }

@app.get("/api/system/interface-history")
async def get_interface_history_endpoint(minutes: int = 60, session_id: str = Depends(require_auth)):
    """Get interface bandwidth history for graphing"""
    minutes = min(minutes, 60)
    cutoff = time.time() - (minutes * 60)

    result = {}
    with interface_history_lock:
        for iface, points in interface_history.items():
            filtered = [p for p in points if p['timestamp'] >= cutoff]
            if filtered:
                result[iface] = filtered

    return result

# =============================================================================
# Config Backup/Restore Routes
# =============================================================================

@app.get("/api/config/backup")
async def download_config_backup(session_id: str = Depends(require_auth)):
    """Download current configuration as JSON"""
    config = load_config()
    channels = config.get('channels', {})

    # Capture current running state for each channel
    for ch_id, ch in channels.items():
        status = get_channel_status(ch_id)
        if 'autostart' not in ch:
            ch['autostart'] = status == 'running'

    backup_data = {
        'version': '1.0',
        'timestamp': datetime.now().isoformat(),
        'hostname': os.uname().nodename,
        'channels': channels,
        'system': config.get('system', {})
    }

    content = json.dumps(backup_data, indent=2)
    filename = f"ristsender_backup_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"

    return Response(
        content=content,
        media_type="application/json",
        headers={"Content-Disposition": f"attachment; filename={filename}"}
    )

@app.post("/api/config/restore")
async def restore_config_backup(request: Request, session_id: str = Depends(require_auth)):
    """Restore configuration from uploaded JSON backup"""
    try:
        backup_data = await request.json()

        if 'channels' not in backup_data:
            raise HTTPException(status_code=400, detail="Invalid backup: missing channels")

        # Stop all running channels and remove service files
        config = load_config()
        for ch_id in config.get('channels', {}):
            try:
                subprocess.run(['systemctl', 'stop', f'ristsender-{ch_id}.service'],
                              capture_output=True)
                delete_service_file(ch_id)
            except:
                pass

        # Preserve password hash from current config
        password_hash = config.get('system', {}).get('password_hash')

        # Build new config
        new_config = {
            'channels': backup_data.get('channels', {}),
            'system': backup_data.get('system', config.get('system', {}))
        }

        if password_hash:
            new_config['system']['password_hash'] = password_hash

        save_config(new_config)

        # Recreate service files for all channels
        for ch_id, ch in new_config.get('channels', {}).items():
            if not ch.get('metrics_port'):
                ch['metrics_port'] = get_next_metrics_port(new_config)
            generate_service_file(ch_id, ch)

            # Start channels that were running
            if ch.get('autostart', False):
                subprocess.run(['systemctl', 'start', f'ristsender-{ch_id}.service'],
                              capture_output=True)

        return {"success": True, "message": "Configuration restored successfully"}

    except HTTPException:
        raise
    except Exception as e:
        logger.error(f"Restore failed: {e}")
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/api/auth/password")
async def change_password(request: PasswordChange, session_id: str = Depends(require_auth)):
    """Change admin password"""
    config = load_config()
    stored_hash = config.get('system', {}).get('password_hash', hash_password('admin'))
    current_hash = hash_password(request.current_password)

    if current_hash != stored_hash:
        raise HTTPException(status_code=401, detail="Current password is incorrect")

    if 'system' not in config:
        config['system'] = {}

    config['system']['password_hash'] = hash_password(request.new_password)
    save_config(config)

    return {"success": True, "message": "Password changed successfully"}

# =============================================================================
# Preview (HLS Transcoding)
# =============================================================================

PREVIEW_DIR = '/dev/shm/ristsender-preview'

def send_to_preview_socket(channel_id: str, command: str) -> Optional[str]:
    """Send command to ristpreview Unix socket, return response or None"""
    import socket
    socket_path = os.path.join(PREVIEW_DIR, channel_id, 'keepalive.sock')
    if not os.path.exists(socket_path):
        return None
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2)
        sock.connect(socket_path)
        sock.send(command.encode())
        response = sock.recv(1024).decode().strip()
        sock.close()
        return response
    except Exception:
        return None

@app.post("/api/channels/{channel_id}/preview/start")
async def start_preview(channel_id: str, session_id: str = Depends(require_auth)):
    """Start preview transcoder for channel"""
    config = load_config()
    channels = config.get('channels', {})

    if channel_id not in channels:
        raise HTTPException(status_code=404, detail="Channel not found")

    ch = channels[channel_id]
    input_url = ch.get('input')
    if not input_url:
        raise HTTPException(status_code=400, detail="Channel has no input configured")

    preview_dir = os.path.join(PREVIEW_DIR, channel_id)
    socket_path = os.path.join(preview_dir, 'keepalive.sock')

    # Check if already running
    response = send_to_preview_socket(channel_id, "keepalive")
    if response == "ok":
        return {
            "status": "running",
            "hls_url": f"/api/channels/{channel_id}/preview/stream.m3u8"
        }

    # Create preview directory
    os.makedirs(preview_dir, exist_ok=True)

    # Get bitrate setting
    bitrate = config.get('system', {}).get('preview_bitrate', 2000)

    # Start ristpreview process (works with UDP URLs too via ffmpeg)
    try:
        proc = subprocess.Popen(
            [
                '/usr/local/bin/ristpreview',
                '--rist-url', input_url,
                '--output', preview_dir,
                '--bitrate', str(bitrate)
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
    except FileNotFoundError:
        raise HTTPException(status_code=500, detail="ristpreview not installed")
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Failed to start preview: {e}")

    # Wait for socket to appear (up to 5 seconds)
    for _ in range(50):
        if os.path.exists(socket_path):
            response = send_to_preview_socket(channel_id, "keepalive")
            if response == "ok":
                break
        time.sleep(0.1)
    else:
        if proc.poll() is not None:
            raise HTTPException(status_code=500, detail="Preview process exited unexpectedly")
        raise HTTPException(status_code=500, detail="Preview process not responding")

    return {
        "status": "started",
        "hls_url": f"/api/channels/{channel_id}/preview/stream.m3u8"
    }

@app.post("/api/channels/{channel_id}/preview/keepalive")
async def preview_keepalive(channel_id: str, session_id: str = Depends(require_auth)):
    """Send keepalive to preview process"""
    response = send_to_preview_socket(channel_id, "keepalive")
    if response is None:
        raise HTTPException(status_code=404, detail="Preview not running")
    if response != "ok":
        raise HTTPException(status_code=500, detail=f"Unexpected response: {response}")
    return {"status": "ok"}

@app.get("/api/channels/{channel_id}/preview/info")
async def preview_info(channel_id: str, session_id: str = Depends(require_auth)):
    """Get stream info from preview process"""
    response = send_to_preview_socket(channel_id, "info")
    if response is None:
        raise HTTPException(status_code=404, detail="Preview not running")
    try:
        return json.loads(response)
    except json.JSONDecodeError:
        raise HTTPException(status_code=500, detail="Invalid response from preview")

@app.get("/api/channels/{channel_id}/preview/{filename:path}")
async def serve_preview_file(channel_id: str, filename: str, session_id: str = Depends(require_auth)):
    """Serve HLS files from preview"""
    if '..' in filename or filename.startswith('/'):
        raise HTTPException(status_code=400, detail="Invalid filename")

    preview_dir = os.path.join(PREVIEW_DIR, channel_id)
    file_path = os.path.join(preview_dir, filename)

    if not os.path.exists(file_path):
        raise HTTPException(status_code=404, detail="File not found")

    media_type = 'application/vnd.apple.mpegurl' if filename.endswith('.m3u8') else 'video/mp2t'
    return FileResponse(file_path, media_type=media_type)

# =============================================================================
# Settings
# =============================================================================

@app.get("/api/settings")
async def get_settings(session_id: str = Depends(require_auth)):
    """Get system settings"""
    config = load_config()
    system = config.get('system', {})
    return {
        'preview_bitrate': system.get('preview_bitrate', 2000),  # Default 2000 Kbps
    }

@app.put("/api/settings")
async def update_settings(request: Request, session_id: str = Depends(require_auth)):
    """Update system settings"""
    data = await request.json()
    config = load_config()

    if 'system' not in config:
        config['system'] = {}

    if 'preview_bitrate' in data:
        config['system']['preview_bitrate'] = int(data['preview_bitrate'])

    save_config(config)
    return {"success": True}

# =============================================================================
# Static Files & HTML Routes
# =============================================================================

@app.get("/favicon.ico")
async def favicon():
    """Serve favicon"""
    # Orange "S" icon for Sender
    svg_icon = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">
        <rect width="32" height="32" rx="6" fill="#f97316"/>
        <text x="16" y="24" font-family="Arial,sans-serif" font-size="22" font-weight="bold" fill="white" text-anchor="middle">S</text>
    </svg>'''
    return Response(content=svg_icon, media_type="image/svg+xml")

@app.get("/", response_class=HTMLResponse)
async def index():
    """Serve index page"""
    return FileResponse(os.path.join(WEB_DIR, 'index.html'))

@app.get("/login", response_class=HTMLResponse)
async def login_page():
    """Serve login page"""
    return FileResponse(os.path.join(WEB_DIR, 'login.html'))

@app.get("/edit/{channel_id}", response_class=HTMLResponse)
async def edit_page(channel_id: str):
    """Serve edit page"""
    return FileResponse(os.path.join(WEB_DIR, 'edit.html'))

@app.get("/new", response_class=HTMLResponse)
async def new_page():
    """Serve new channel page"""
    return FileResponse(os.path.join(WEB_DIR, 'edit.html'))

@app.get("/stats/{channel_id}", response_class=HTMLResponse)
async def stats_page(channel_id: str):
    """Serve stats page"""
    return FileResponse(os.path.join(WEB_DIR, 'stats.html'))

@app.get("/server", response_class=HTMLResponse)
async def server_page():
    """Serve server stats page"""
    return FileResponse(os.path.join(WEB_DIR, 'server.html'))

@app.get("/settings", response_class=HTMLResponse)
async def settings_page():
    """Serve settings page"""
    return FileResponse(os.path.join(WEB_DIR, 'settings.html'))

# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    import uvicorn

    # Ensure config exists
    load_config()

    logger.info("Starting RIST Sender API on port 8080")
    uvicorn.run(app, host="0.0.0.0", port=8080, log_level="info")
