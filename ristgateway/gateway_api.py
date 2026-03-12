#!/usr/bin/env python3
"""
RIST Gateway API
FastAPI backend for managing rist22rist gateways with multi-peer bonding
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
CONFIG_FILE = os.environ.get('GATEWAY_CONFIG', '/etc/ristgateway/gateway_config.yaml')
SYSTEMD_DIR = '/etc/systemd/system'
PID_DIR = '/var/run/ristgateway'
WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')

# Ensure directories exist
os.makedirs(os.path.dirname(CONFIG_FILE), exist_ok=True)
os.makedirs(PID_DIR, exist_ok=True)

# Setup logging (console only, no file logging)
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')
logger = logging.getLogger('gateway_api')

# =============================================================================
# In-Memory Stats Storage
# =============================================================================

# Store last 60 data points per gateway (for charts)
STATS_HISTORY_SIZE = 60
gateway_stats: Dict[str, Dict] = {}  # Current stats per gateway
gateway_stats_history: Dict[str, deque] = {}  # Historical stats per gateway
gateway_status_cache: Dict[str, Dict] = {}  # Cached running status per gateway
stats_lock = threading.Lock()
stats_poller_thread = None
stats_poller_running = False

# Status cache with TTL to avoid hammering systemctl on every request
STATUS_CACHE_TTL = 2.0  # seconds
_status_cache: Dict[str, Dict] = {}  # {gateway_id: {status, timestamp}}
_gateway_locks: Dict[str, asyncio.Lock] = {}  # per-gateway async locks (lazy-init)

def init_gateway_stats(gateway_id: str):
    """Initialize stats storage for a gateway"""
    with stats_lock:
        if gateway_id not in gateway_stats:
            gateway_stats[gateway_id] = {
                'quality': 0,
                'peers': 0,
                'bandwidth': 0,
                'retry_bandwidth': 0,
                'rtt': 0,
                'packets': {
                    'sent': 0,
                    'received': 0,
                    'missing': 0,
                    'reordered': 0,
                    'recovered': 0,
                    'recovered_one_retry': 0,
                    'lost': 0
                },
                'timing': {
                    'min_iat': 0,
                    'cur_iat': 0,
                    'max_iat': 0
                },
                'peer_details': [],
                'last_update': None
            }
        if gateway_id not in gateway_stats_history:
            gateway_stats_history[gateway_id] = deque(maxlen=STATS_HISTORY_SIZE)

def parse_rist_json_stats(text: str) -> Dict:
    """Parse JSON stats from rist22rist output"""
    metrics = {
        'quality': 0,
        'peers': 0,
        'bandwidth': 0,
        'retry_bandwidth': 0,
        'rtt': 0,
        'packets': {
            'sent': 0,
            'received': 0,
            'missing': 0,
            'reordered': 0,
            'recovered': 0,
            'recovered_one_retry': 0,
            'lost': 0
        },
        'timing': {
            'min_iat': 0,
            'cur_iat': 0,
            'max_iat': 0
        },
        'peer_details': [],
        'sender': {},
        'timestamp': datetime.now().isoformat()
    }

    try:
        # Find all JSON objects in the text
        for line in text.split('\n'):
            # Extract JSON from log line (format: timestamp|...|[INFO] {json})
            if '{"receiver-stats"' in line or '{"sender-stats"' in line:
                # Find the JSON part
                json_start = line.find('{')
                if json_start == -1:
                    continue
                json_str = line[json_start:]

                try:
                    data = json.loads(json_str)
                except json.JSONDecodeError:
                    continue

                # Parse receiver-stats
                if 'receiver-stats' in data:
                    rx = data['receiver-stats']
                    if 'flowinstant' in rx:
                        flow = rx['flowinstant']
                        stats = flow.get('stats', {})

                        metrics['quality'] = stats.get('quality', 0)
                        metrics['packets']['received'] = stats.get('received', 0)
                        metrics['packets']['missing'] = stats.get('missing', 0)
                        metrics['packets']['recovered'] = stats.get('recovered_total', 0)
                        metrics['packets']['reordered'] = stats.get('reordered', 0)
                        metrics['packets']['lost'] = stats.get('lost', 0)
                        metrics['packets']['recovered_one_retry'] = stats.get('recovered_one_nack', 0)

                        # Bandwidth in bps -> Mbps
                        metrics['bandwidth'] = stats.get('bitrate', 0) / 1_000_000

                        # Inter-packet timing (microseconds -> ms)
                        metrics['timing']['min_iat'] = stats.get('min_inter_packet_spacing', 0) / 1000
                        metrics['timing']['cur_iat'] = stats.get('cur_inter_packet_spacing', 0) / 1000
                        metrics['timing']['max_iat'] = stats.get('max_inter_packet_spacing', 0) / 1000

                        # Parse peers
                        peers = flow.get('peers', [])
                        metrics['peers'] = len(peers)

                        # Calculate average RTT from peers
                        if peers:
                            total_rtt = sum(p.get('stats', {}).get('rtt', 0) for p in peers)
                            metrics['rtt'] = total_rtt / len(peers)

                            # Store peer details
                            metrics['peer_details'] = [
                                {
                                    'id': p.get('id'),
                                    'dead': p.get('dead', 0),
                                    'rtt': p.get('stats', {}).get('rtt', 0),
                                    'bitrate': p.get('stats', {}).get('bitrate', 0) / 1_000_000
                                }
                                for p in peers
                            ]

                # Parse sender-stats
                elif 'sender-stats' in data:
                    tx = data['sender-stats']
                    if 'peer' in tx:
                        peer = tx['peer']
                        stats = peer.get('stats', {})

                        metrics['sender'] = {
                            'cname': peer.get('cname', ''),
                            'quality': stats.get('quality', 0),
                            'sent': stats.get('sent', 0),
                            'retransmitted': stats.get('retransmitted', 0),
                            'bandwidth': stats.get('bandwidth', 0) / 1_000_000,
                            'retry_bandwidth': stats.get('retry_bandwidth', 0) / 1_000_000,
                            'rtt': stats.get('rtt', 0)
                        }
                        metrics['retry_bandwidth'] = stats.get('retry_bandwidth', 0) / 1_000_000
                        metrics['packets']['sent'] = stats.get('sent', 0)

    except Exception as e:
        logger.error(f"Error parsing RIST JSON stats: {e}")

    return metrics

def update_gateway_stats(gateway_id: str, metrics: Dict, running: bool = True):
    """Update stats for a gateway"""
    with stats_lock:
        init_gateway_stats(gateway_id)
        metrics['last_update'] = datetime.now().isoformat()
        metrics['running'] = running
        gateway_stats[gateway_id] = metrics
        gateway_stats_history[gateway_id].append({
            'timestamp': metrics['timestamp'],
            'quality': metrics['quality'],
            'bandwidth': metrics['bandwidth'],
            'peers': metrics['peers'],
            'rtt': metrics['rtt']
        })

def get_gateway_stats(gateway_id: str) -> Dict:
    """Get current stats for a gateway"""
    with stats_lock:
        return gateway_stats.get(gateway_id, {})

def get_cached_status(gateway_id: str) -> Dict:
    """Get cached running status for a gateway"""
    with stats_lock:
        return gateway_status_cache.get(gateway_id, {'running': False, 'status': 'unknown'})

def update_cached_status(gateway_id: str, running: bool, pid: int = None, uptime: str = None):
    """Update cached running status for a gateway"""
    with stats_lock:
        gateway_status_cache[gateway_id] = {
            'running': running,
            'status': 'running' if running else 'stopped',
            'pid': pid,
            'uptime': uptime
        }

def get_gateway_stats_history(gateway_id: str) -> List[Dict]:
    """Get historical stats for a gateway"""
    with stats_lock:
        if gateway_id in gateway_stats_history:
            return list(gateway_stats_history[gateway_id])
        return []

def parse_prometheus_metrics(text: str) -> Dict:
    """Parse Prometheus/OpenMetrics format from rist22rist metrics endpoint"""
    metrics = {
        'quality': 0,
        'peers': 0,
        'bandwidth': 0,
        'retry_bandwidth': 0,
        'rtt': 0,
        'packets': {
            'sent': 0,
            'received': 0,
            'missing': 0,
            'reordered': 0,
            'recovered': 0,
            'recovered_one_retry': 0,
            'lost': 0
        },
        'timing': {
            'min_iat': 0,
            'cur_iat': 0,
            'max_iat': 0
        },
        'peer_details': [],
        'sender': {},
        'timestamp': datetime.now().isoformat()
    }

    # Aggregate values across all flows
    flow_data = {}  # flow_id -> metrics

    for line in text.split('\n'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue

        # Parse: metric_name{labels} value
        match = re.match(r'^([a-zA-Z_][a-zA-Z0-9_]*)\{([^}]*)\}\s+(.+)$', line)
        if not match:
            # Try without labels
            match = re.match(r'^([a-zA-Z_][a-zA-Z0-9_]*)\s+(.+)$', line)
            if match:
                metric_name = match.group(1)
                value = float(match.group(2))
                labels = {}
            else:
                continue
        else:
            metric_name = match.group(1)
            labels_str = match.group(2)
            value = float(match.group(3))

            # Parse labels
            labels = {}
            for label_match in re.finditer(r'(\w+)="([^"]*)"', labels_str):
                labels[label_match.group(1)] = label_match.group(2)

        flow_id = labels.get('flow_id', 'default')

        # Initialize flow if needed
        if flow_id not in flow_data:
            flow_data[flow_id] = {
                'quality': 0, 'bandwidth': 0, 'retry_bandwidth': 0, 'rtt': 0, 'peers': 0,
                'received': 0, 'sent': 0, 'missing': 0, 'reordered': 0,
                'recovered': 0, 'recovered_one_retry': 0, 'lost': 0,
                'min_iat': 0, 'cur_iat': 0, 'max_iat': 0
            }

        fd = flow_data[flow_id]

        # Map metrics
        if metric_name == 'rist_client_flow_quality':
            fd['quality'] = value
        elif metric_name == 'rist_client_flow_peers':
            fd['peers'] = int(value)
        elif metric_name == 'rist_client_flow_bandwidth_bps':
            fd['bandwidth'] = value / 1_000_000  # bps to Mbps
        elif metric_name == 'rist_client_flow_retry_bandwidth_bps':
            fd['retry_bandwidth'] = value / 1_000_000
        elif metric_name == 'rist_client_flow_rtt_seconds':
            fd['rtt'] = value * 1000  # seconds to ms
        elif metric_name == 'rist_client_flow_received_packets_total':
            fd['received'] = int(value)
        elif metric_name == 'rist_client_flow_sent_packets_total':
            fd['sent'] = int(value)
        elif metric_name == 'rist_client_flow_missing_packets_total':
            fd['missing'] = int(value)
        elif metric_name == 'rist_client_flow_reordered_packets_total':
            fd['reordered'] = int(value)
        elif metric_name == 'rist_client_flow_recovered_packets_total':
            fd['recovered'] = int(value)
        elif metric_name == 'rist_client_flow_recovered_one_retry_packets_total':
            fd['recovered_one_retry'] = int(value)
        elif metric_name == 'rist_client_flow_lost_packets_total':
            fd['lost'] = int(value)
        elif metric_name == 'rist_client_flow_min_iat_seconds':
            fd['min_iat'] = value * 1000  # seconds to ms
        elif metric_name == 'rist_client_flow_cur_iat_seconds':
            fd['cur_iat'] = value * 1000
        elif metric_name == 'rist_client_flow_max_iat_seconds':
            fd['max_iat'] = value * 1000

    # Aggregate all flows
    if flow_data:
        total_bandwidth = 0
        total_quality = 0
        total_peers = 0
        quality_count = 0

        for fid, fd in flow_data.items():
            total_bandwidth += fd['bandwidth']
            total_peers += fd['peers']
            if fd['quality'] > 0:
                total_quality += fd['quality']
                quality_count += 1

            # Sum packet counters
            metrics['packets']['received'] += fd['received']
            metrics['packets']['sent'] += fd['sent']
            metrics['packets']['missing'] += fd['missing']
            metrics['packets']['reordered'] += fd['reordered']
            metrics['packets']['recovered'] += fd['recovered']
            metrics['packets']['recovered_one_retry'] += fd['recovered_one_retry']
            metrics['packets']['lost'] += fd['lost']

            # Use max timing values
            metrics['timing']['min_iat'] = max(metrics['timing']['min_iat'], fd['min_iat'])
            metrics['timing']['cur_iat'] = max(metrics['timing']['cur_iat'], fd['cur_iat'])
            metrics['timing']['max_iat'] = max(metrics['timing']['max_iat'], fd['max_iat'])

            # RTT - use first non-zero
            if fd['rtt'] > 0 and metrics['rtt'] == 0:
                metrics['rtt'] = fd['rtt']

            # Add as peer detail
            metrics['peer_details'].append({
                'id': fid,
                'dead': 0,
                'rtt': fd['rtt'],
                'bitrate': fd['bandwidth'],
                'quality': fd['quality']
            })

        metrics['bandwidth'] = total_bandwidth
        metrics['retry_bandwidth'] = sum(fd['retry_bandwidth'] for fd in flow_data.values())
        metrics['peers'] = total_peers if total_peers > 0 else len(flow_data)
        metrics['quality'] = total_quality / quality_count if quality_count > 0 else 0

    return metrics


def collect_stats_from_metrics_http(gateway_id: str, metrics_port: int, running: bool = True):
    """Collect stats from rist22rist metrics HTTP endpoint"""
    import urllib.request
    import urllib.error

    try:
        url = f"http://127.0.0.1:{metrics_port}/metrics"
        req = urllib.request.Request(url)

        with urllib.request.urlopen(req, timeout=2) as response:
            data = response.read().decode('utf-8')

            # Parse Prometheus format
            metrics = parse_prometheus_metrics(data)

            if metrics['peers'] > 0 or metrics['packets']['received'] > 0 or metrics['quality'] > 0:
                update_gateway_stats(gateway_id, metrics, running)
                return True

    except urllib.error.URLError as e:
        logger.debug(f"Could not connect to metrics endpoint for {gateway_id}: {e}")
    except Exception as e:
        logger.error(f"Error collecting stats from HTTP for {gateway_id}: {e}")

    return False


def parse_metrics_json(data: dict) -> Dict:
    """Parse metrics from HTTP JSON response"""
    metrics = {
        'quality': 0,
        'peers': 0,
        'bandwidth': 0,
        'retry_bandwidth': 0,
        'rtt': 0,
        'packets': {
            'sent': 0,
            'received': 0,
            'missing': 0,
            'reordered': 0,
            'recovered': 0,
            'recovered_one_retry': 0,
            'lost': 0
        },
        'timing': {
            'min_iat': 0,
            'cur_iat': 0,
            'max_iat': 0
        },
        'peer_details': [],
        'sender': {},
        'timestamp': datetime.now().isoformat()
    }

    try:
        # Handle receiver-stats
        if 'receiver-stats' in data:
            rx = data['receiver-stats']
            if 'flowinstant' in rx:
                flow = rx['flowinstant']
                stats = flow.get('stats', {})

                metrics['quality'] = stats.get('quality', 0)
                metrics['packets']['received'] = stats.get('received', 0)
                metrics['packets']['missing'] = stats.get('missing', 0)
                metrics['packets']['recovered'] = stats.get('recovered_total', 0)
                metrics['packets']['reordered'] = stats.get('reordered', 0)
                metrics['packets']['lost'] = stats.get('lost', 0)
                metrics['packets']['recovered_one_retry'] = stats.get('recovered_one_nack', 0)
                metrics['bandwidth'] = stats.get('bitrate', 0) / 1_000_000
                metrics['timing']['min_iat'] = stats.get('min_inter_packet_spacing', 0) / 1000
                metrics['timing']['cur_iat'] = stats.get('cur_inter_packet_spacing', 0) / 1000
                metrics['timing']['max_iat'] = stats.get('max_inter_packet_spacing', 0) / 1000

                peers = flow.get('peers', [])
                metrics['peers'] = len(peers)

                if peers:
                    total_rtt = sum(p.get('stats', {}).get('rtt', 0) for p in peers)
                    metrics['rtt'] = total_rtt / len(peers)
                    metrics['peer_details'] = [
                        {
                            'id': p.get('id'),
                            'dead': p.get('dead', 0),
                            'rtt': p.get('stats', {}).get('rtt', 0),
                            'bitrate': p.get('stats', {}).get('bitrate', 0) / 1_000_000
                        }
                        for p in peers
                    ]

        # Handle sender-stats
        if 'sender-stats' in data:
            tx = data['sender-stats']
            if 'peer' in tx:
                peer = tx['peer']
                stats = peer.get('stats', {})
                metrics['sender'] = {
                    'cname': peer.get('cname', ''),
                    'quality': stats.get('quality', 0),
                    'sent': stats.get('sent', 0),
                    'retransmitted': stats.get('retransmitted', 0),
                    'bandwidth': stats.get('bandwidth', 0) / 1_000_000,
                    'retry_bandwidth': stats.get('retry_bandwidth', 0) / 1_000_000,
                    'rtt': stats.get('rtt', 0)
                }
                metrics['retry_bandwidth'] = stats.get('retry_bandwidth', 0) / 1_000_000
                metrics['packets']['sent'] = stats.get('sent', 0)

    except Exception as e:
        logger.error(f"Error parsing metrics JSON: {e}")

    return metrics


def collect_gateway_stats(gateway_id: str, running: bool = True):
    """Collect stats for a gateway from journal (has both receiver and sender stats)"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        return

    # Use journal parsing - it has both receiver-stats and sender-stats
    # HTTP metrics endpoint only has sender stats (incomplete)
    collect_stats_from_journal(gateway_id, running)


def collect_stats_from_journal(gateway_id: str, running: bool = True):
    """Collect latest stats from journald for a gateway (fallback)"""
    service_name = f"ristgateway-{gateway_id}"
    try:
        result = subprocess.run(
            ['journalctl', '-u', service_name, '-n', '10', '--no-pager', '-o', 'cat'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout:
            metrics = parse_rist_json_stats(result.stdout)
            if metrics['peers'] > 0 or metrics['packets']['received'] > 0:
                update_gateway_stats(gateway_id, metrics, running)
    except Exception as e:
        logger.error(f"Error collecting stats from journal for {gateway_id}: {e}")


def stats_poller_loop():
    """Background thread that polls stats for all gateways"""
    global stats_poller_running
    logger.info("Stats poller started")

    while stats_poller_running:
        try:
            config = load_config()
            gateways = config.get('gateways', {})

            for gateway_id, gw in gateways.items():
                if not stats_poller_running:
                    break

                service_name = f"ristgateway-{gateway_id}"

                # Check if running (simple, direct call)
                try:
                    result = subprocess.run(
                        ['systemctl', 'is-active', f'{service_name}.service'],
                        capture_output=True, text=True, timeout=3
                    )
                    is_running = result.stdout.strip() == 'active'
                except:
                    is_running = False

                # Update status cache
                update_cached_status(gateway_id, is_running, None, None)

                # Collect stats if running
                if is_running:
                    try:
                        result = subprocess.run(
                            ['journalctl', '-u', service_name, '-n', '5', '--no-pager', '-o', 'cat'],
                            capture_output=True, text=True, timeout=3
                        )
                        if result.returncode == 0 and result.stdout:
                            metrics = parse_rist_json_stats(result.stdout)
                            if metrics['peers'] > 0 or metrics['packets']['received'] > 0:
                                update_gateway_stats(gateway_id, metrics, True)
                    except Exception as e:
                        logger.debug(f"Error collecting stats for {gateway_id}: {e}")

            # Sleep for 2 seconds between poll cycles
            for _ in range(20):  # Check every 100ms if we should stop
                if not stats_poller_running:
                    break
                time.sleep(0.1)

        except Exception as e:
            logger.error(f"Error in stats poller: {e}")
            time.sleep(2)

    logger.info("Stats poller stopped")


def initialize_gateway_status_cache():
    """Initialize status cache for all gateways on startup"""
    logger.info("Initializing gateway status cache...")
    try:
        config = load_config()
        gateways = config.get('gateways', {})
        for gateway_id in gateways:
            status = get_gateway_status(gateway_id)
            update_cached_status(gateway_id, status['running'], status['pid'], status['uptime'])
            logger.info(f"  {gateway_id}: {'running' if status['running'] else 'stopped'}")
    except Exception as e:
        logger.error(f"Error initializing status cache: {e}")

def start_stats_poller():
    """Start the background stats polling thread"""
    global stats_poller_thread, stats_poller_running

    if stats_poller_thread and stats_poller_thread.is_alive():
        return  # Already running

    stats_poller_running = True
    stats_poller_thread = threading.Thread(target=stats_poller_loop, daemon=True)
    stats_poller_thread.start()
    logger.info("Stats poller thread started")


def stop_stats_poller():
    """Stop the background stats polling thread"""
    global stats_poller_running
    stats_poller_running = False
    logger.info("Stats poller thread stopping...")


# Session storage
sessions: Dict[str, Dict] = {}
SESSION_TIMEOUT = timedelta(hours=24)

# Config cache
config_cache = {'data': None, 'mtime': 0}

# =============================================================================
# Pydantic Models
# =============================================================================

class PeerConfig(BaseModel):
    url: str
    name: str = ""
    enabled: bool = True

class GatewaySettings(BaseModel):
    profile: int = 1
    buffer: int = 1000
    encryption_type: Optional[int] = None
    secret: Optional[str] = None
    npd: bool = True
    ssrc_passthrough: bool = True
    stats_interval: int = 1000

class GatewayConfig(BaseModel):
    name: str
    enabled: bool = True
    inputs: List[PeerConfig]
    outputs: List[PeerConfig]
    settings: GatewaySettings = GatewaySettings()
    metrics_port: Optional[int] = None

class GatewayUpdate(BaseModel):
    name: Optional[str] = None
    enabled: Optional[bool] = None
    inputs: Optional[List[PeerConfig]] = None
    outputs: Optional[List[PeerConfig]] = None
    settings: Optional[GatewaySettings] = None

class LoginRequest(BaseModel):
    password: str

class PasswordChange(BaseModel):
    current_password: str
    new_password: str

class BulkOperation(BaseModel):
    operation: str
    gateway_ids: List[str]

# =============================================================================
# Configuration Management
# =============================================================================

def load_config() -> dict:
    """Load configuration from YAML file with caching"""
    global config_cache

    try:
        mtime = os.path.getmtime(CONFIG_FILE) if os.path.exists(CONFIG_FILE) else 0
        if config_cache['data'] is None or mtime > config_cache['mtime']:
            if os.path.exists(CONFIG_FILE):
                with open(CONFIG_FILE, 'r') as f:
                    config_cache['data'] = yaml.safe_load(f) or {}
            else:
                config_cache['data'] = {'gateways': {}, 'system': {}}
            config_cache['mtime'] = mtime
        return config_cache['data']
    except Exception as e:
        logger.error(f"Error loading config: {e}")
        return {'gateways': {}, 'system': {}}

def save_config(config: dict):
    """Save configuration to YAML file"""
    global config_cache
    try:
        with open(CONFIG_FILE, 'w') as f:
            yaml.dump(config, f, default_flow_style=False, sort_keys=False)
        config_cache['data'] = config
        config_cache['mtime'] = time.time()
        logger.info("Configuration saved")
    except Exception as e:
        logger.error(f"Error saving config: {e}")
        raise HTTPException(status_code=500, detail=f"Failed to save configuration: {e}")

def get_next_gateway_id(config: dict) -> str:
    """Generate next available gateway ID"""
    existing = config.get('gateways', {}).keys()
    n = 1
    while f"gateway{n}" in existing:
        n += 1
    return f"gateway{n}"

def get_next_metrics_port(config: dict) -> int:
    """Get next available metrics port"""
    used_ports = set()
    for gw in config.get('gateways', {}).values():
        if gw.get('metrics_port'):
            used_ports.add(gw['metrics_port'])
    port = 9301
    while port in used_ports:
        port += 1
    return port

# =============================================================================
# Systemd Service Management
# =============================================================================

def generate_systemd_service(gateway_id: str, gateway: dict) -> str:
    """Generate systemd service file content for a gateway"""
    inputs = gateway.get('inputs', [])
    outputs = gateway.get('outputs', [])
    settings = gateway.get('settings', {})

    # Build command line
    cmd_parts = ['/usr/local/bin/rist22rist']

    # Add input peers
    for peer in inputs:
        if peer.get('enabled', True):
            cmd_parts.append(f"-i '{peer['url']}'")

    # Add output peers
    for peer in outputs:
        if peer.get('enabled', True):
            cmd_parts.append(f"-o '{peer['url']}'")

    # Add settings
    if settings.get('profile') is not None:
        cmd_parts.append(f"-p {settings['profile']}")

    if settings.get('npd'):
        cmd_parts.append('-n')

    if settings.get('ssrc_passthrough'):
        cmd_parts.append('-P')

    if settings.get('encryption_type') and settings.get('secret'):
        cmd_parts.append(f"-e {settings['encryption_type']}")
        cmd_parts.append(f"-s '{settings['secret']}'")

    if settings.get('stats_interval'):
        cmd_parts.append(f"-S {settings['stats_interval']}")

    # Add metrics HTTP endpoint (requires librist compiled with HAVE_PROMETHEUS_SUPPORT)
    metrics_port = gateway.get('metrics_port')
    if metrics_port and gateway.get('enable_metrics_http', True):
        cmd_parts.append('-M')
        cmd_parts.append('--metrics-http')
        cmd_parts.append(f'--metrics-port={metrics_port}')

    cmd = ' '.join(cmd_parts)

    service_content = f"""[Unit]
Description=RIST Gateway - {gateway.get('name', gateway_id)}
After=network.target

[Service]
Type=simple
ExecStart=/bin/bash -c "{cmd}"
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=ristgateway-{gateway_id}

[Install]
WantedBy=multi-user.target
"""
    return service_content

def write_systemd_service(gateway_id: str, gateway: dict):
    """Write systemd service file for a gateway"""
    service_name = f"ristgateway-{gateway_id}.service"
    service_path = os.path.join(SYSTEMD_DIR, service_name)

    content = generate_systemd_service(gateway_id, gateway)

    with open(service_path, 'w') as f:
        f.write(content)

    # Reload systemd
    subprocess.run(['systemctl', 'daemon-reload'], check=True, timeout=10)
    logger.info(f"Created systemd service: {service_name}")

def remove_systemd_service(gateway_id: str):
    """Remove systemd service file for a gateway"""
    service_name = f"ristgateway-{gateway_id}.service"
    service_path = os.path.join(SYSTEMD_DIR, service_name)

    # Stop service if running
    subprocess.run(['systemctl', 'stop', service_name], capture_output=True, timeout=30)
    subprocess.run(['systemctl', 'disable', service_name], capture_output=True, timeout=10)

    if os.path.exists(service_path):
        os.remove(service_path)
        subprocess.run(['systemctl', 'daemon-reload'], check=True, timeout=10)
        logger.info(f"Removed systemd service: {service_name}")

def run_cmd(args, timeout=5) -> str:
    """Sync run_cmd for use outside request handlers (start/stop/restart)"""
    proc = None
    try:
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, _ = proc.communicate(timeout=timeout)
        return stdout.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        if proc:
            proc.kill()
            proc.communicate()
        return ''
    except Exception:
        return ''


def _check_cgroup(gateway_id: str) -> dict:
    """Check service status via cgroup filesystem - no subprocess, instant"""
    service_name = f"ristgateway-{gateway_id}.service"
    cgroup_path = f"/sys/fs/cgroup/system.slice/{service_name}"

    result = {
        'running': False,
        'status': 'stopped',
        'pid': None,
        'uptime': None,
        'cpu_percent': None,
        'memory_mb': None
    }

    # Check if cgroup directory exists (service is running)
    if not os.path.isdir(cgroup_path):
        return result

    result['running'] = True
    result['status'] = 'running'

    # Read PIDs from cgroup
    try:
        procs_file = os.path.join(cgroup_path, 'cgroup.procs')
        if os.path.exists(procs_file):
            with open(procs_file, 'r') as f:
                pids = [int(line.strip()) for line in f if line.strip().isdigit()]
                if pids:
                    result['pid'] = pids[0]
    except:
        pass

    # Read memory usage from cgroup
    try:
        memory_current = os.path.join(cgroup_path, 'memory.current')
        if os.path.exists(memory_current):
            with open(memory_current, 'r') as f:
                memory_bytes = int(f.read().strip())
                result['memory_mb'] = round(memory_bytes / (1024 * 1024), 2)
    except:
        pass

    return result


def _read_stats_file(service_name: str) -> str:
    """Read the last line from the stats file"""
    stats_file = f"/tmp/ristgateway-stats/{service_name}.txt"
    try:
        if os.path.exists(stats_file):
            with open(stats_file, 'r') as f:
                lines = f.readlines()
                if lines:
                    return lines[-1].strip()
    except Exception as e:
        logger.error(f"Error reading stats file: {e}")
    return ''


def get_gateway_status(gateway_id: str) -> dict:
    """Get gateway service status via cgroup (no subprocess, cached with TTL)"""
    cached = _status_cache.get(gateway_id)
    if cached and (time.time() - cached['ts']) < STATUS_CACHE_TTL:
        return cached['status']

    # cgroup check is synchronous and instant (filesystem read)
    status = _check_cgroup(gateway_id)
    _status_cache[gateway_id] = {'status': status, 'ts': time.time()}
    return status

def start_gateway(gateway_id: str, gateway: dict) -> bool:
    """Start a gateway service"""
    # Ensure service file exists and is up to date
    write_systemd_service(gateway_id, gateway)

    service_name = f"ristgateway-{gateway_id}.service"

    # Enable and start
    try:
        subprocess.run(['systemctl', 'enable', service_name], capture_output=True, timeout=10)
        result = subprocess.run(['systemctl', 'start', service_name], capture_output=True, text=True, timeout=30)

        if result.returncode != 0:
            logger.error(f"Failed to start {service_name}: {result.stderr}")
            return False

        logger.info(f"Started gateway: {gateway_id}")
        return True
    except subprocess.TimeoutExpired:
        logger.error(f"Timeout starting {service_name}")
        return False

def stop_gateway(gateway_id: str) -> bool:
    """Stop a gateway service"""
    service_name = f"ristgateway-{gateway_id}.service"

    try:
        result = subprocess.run(['systemctl', 'stop', service_name], capture_output=True, text=True, timeout=30)

        if result.returncode != 0:
            logger.error(f"Failed to stop {service_name}: {result.stderr}")
            return False

        logger.info(f"Stopped gateway: {gateway_id}")
        return True
    except subprocess.TimeoutExpired:
        logger.error(f"Timeout stopping {service_name}")
        return False

def restart_gateway(gateway_id: str, gateway: dict) -> bool:
    """Restart a gateway service"""
    # Update service file first
    write_systemd_service(gateway_id, gateway)

    service_name = f"ristgateway-{gateway_id}.service"
    try:
        result = subprocess.run(['systemctl', 'restart', service_name], capture_output=True, text=True, timeout=30)

        if result.returncode != 0:
            logger.error(f"Failed to restart {service_name}: {result.stderr}")
            return False

        logger.info(f"Restarted gateway: {gateway_id}")
        return True
    except subprocess.TimeoutExpired:
        logger.error(f"Timeout restarting {service_name}")
        return False

# =============================================================================
# Authentication
# =============================================================================

def hash_password(password: str) -> str:
    """Hash password with SHA-256"""
    salt = "ristgateway_salt_v1"
    return hashlib.sha256(f"{salt}{password}".encode()).hexdigest()

def get_stored_password_hash() -> str:
    """Get stored password hash from config"""
    config = load_config()
    return config.get('system', {}).get('password_hash', hash_password('admin'))

def verify_session(session_token: str) -> bool:
    """Verify if session token is valid"""
    if session_token not in sessions:
        return False
    session = sessions[session_token]
    if datetime.now() > session['expires']:
        del sessions[session_token]
        return False
    return True

def create_session() -> str:
    """Create new session token"""
    token = secrets.token_urlsafe(32)
    sessions[token] = {
        'created': datetime.now(),
        'expires': datetime.now() + SESSION_TIMEOUT
    }
    return token

def auth_required(request: Request):
    """Dependency for authentication - synchronous for compatibility"""
    # Check session cookie
    session_token = request.cookies.get('session_token')
    if session_token and verify_session(session_token):
        return True

    # Check Authorization header
    auth_header = request.headers.get('Authorization')
    if auth_header and auth_header.startswith('Bearer '):
        token = auth_header[7:]
        if verify_session(token):
            return True

    raise HTTPException(status_code=401, detail="Authentication required")

# =============================================================================
# FastAPI App
# =============================================================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    """Application lifespan handler"""
    logger.info("RIST Gateway API starting...")
    # Initialize config if it doesn't exist
    if not os.path.exists(CONFIG_FILE):
        save_config({'gateways': {}, 'system': {'password_hash': hash_password('admin')}})

    # No background poller - stats collected on-demand via /stats endpoint
    # History builds up as client polls the stats endpoint

    yield

    logger.info("RIST Gateway API shutting down...")

app = FastAPI(
    title="RIST Gateway API",
    description="API for managing rist22rist gateways with multi-peer bonding",
    version="1.0.0",
    lifespan=lifespan
)

# CORS middleware
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# =============================================================================
# Auth Endpoints
# =============================================================================

@app.post("/api/login")
async def login(request: LoginRequest, response: Response):
    """Login and get session token"""
    password_hash = hash_password(request.password)
    stored_hash = get_stored_password_hash()

    if password_hash != stored_hash:
        raise HTTPException(status_code=401, detail="Invalid password")

    token = create_session()
    response.set_cookie(
        key="session_token",
        value=token,
        httponly=True,
        max_age=86400,
        samesite="lax"
    )
    return {"success": True, "token": token}

@app.post("/api/logout")
async def logout(request: Request, response: Response):
    """Logout and invalidate session"""
    token = request.cookies.get('session_token')
    if token and token in sessions:
        del sessions[token]
    response.delete_cookie("session_token")
    return {"success": True}

@app.get("/api/auth/status")
async def auth_status(request: Request):
    """Check authentication status"""
    token = request.cookies.get('session_token')
    authenticated = token and verify_session(token)
    return {"authenticated": authenticated}

@app.post("/api/password", dependencies=[Depends(auth_required)])
async def change_password(request: PasswordChange):
    """Change admin password"""
    current_hash = hash_password(request.current_password)
    stored_hash = get_stored_password_hash()

    if current_hash != stored_hash:
        raise HTTPException(status_code=401, detail="Current password is incorrect")

    config = load_config()
    if 'system' not in config:
        config['system'] = {}
    config['system']['password_hash'] = hash_password(request.new_password)
    save_config(config)

    return {"success": True, "message": "Password changed successfully"}

# =============================================================================
# Gateway Endpoints
# =============================================================================

@app.get("/api/gateways", dependencies=[Depends(auth_required)])
def list_gateways():
    """List all gateways with their status"""
    config = load_config()
    gateways = config.get('gateways', {})

    result = {}
    for gw_id, gw in gateways.items():
        status = _check_cgroup(gw_id)
        result[gw_id] = {
            **gw,
            'id': gw_id,
            'status': status['status'],
            'running': status['running'],
            'pid': status.get('pid'),
            'uptime': status.get('uptime'),
            'input_count': len(gw.get('inputs', [])),
            'output_count': len(gw.get('outputs', []))
        }

    return {"gateways": result}

@app.get("/api/gateways/{gateway_id}", dependencies=[Depends(auth_required)])
def get_gateway(gateway_id: str):
    """Get single gateway details"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]
    status = _check_cgroup(gateway_id)

    return {
        **gw,
        'id': gateway_id,
        'status': status['status'],
        'running': status['running'],
        'pid': status.get('pid'),
        'uptime': status.get('uptime')
    }

@app.post("/api/gateways", dependencies=[Depends(auth_required)])
def create_gateway(gateway: GatewayConfig):
    """Create a new gateway"""
    config = load_config()
    if 'gateways' not in config:
        config['gateways'] = {}

    gateway_id = get_next_gateway_id(config)

    gw_data = {
        'name': gateway.name,
        'enabled': gateway.enabled,
        'status': 'stopped',
        'process_id': None,
        'last_error': None,
        'metrics_port': gateway.metrics_port or get_next_metrics_port(config),
        'inputs': [p.model_dump() for p in gateway.inputs],
        'outputs': [p.model_dump() for p in gateway.outputs],
        'settings': gateway.settings.model_dump()
    }

    config['gateways'][gateway_id] = gw_data
    save_config(config)

    # Create systemd service
    write_systemd_service(gateway_id, gw_data)

    logger.info(f"Created gateway: {gateway_id}")
    return {"id": gateway_id, "gateway": gw_data}

@app.put("/api/gateways/{gateway_id}", dependencies=[Depends(auth_required)])
def update_gateway(gateway_id: str, update: GatewayUpdate):
    """Update an existing gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]

    if update.name is not None:
        gw['name'] = update.name
    if update.enabled is not None:
        gw['enabled'] = update.enabled
    if update.inputs is not None:
        gw['inputs'] = [p.model_dump() for p in update.inputs]
    if update.outputs is not None:
        gw['outputs'] = [p.model_dump() for p in update.outputs]
    if update.settings is not None:
        gw['settings'] = update.settings.model_dump()

    config['gateways'][gateway_id] = gw
    save_config(config)

    # Update systemd service
    write_systemd_service(gateway_id, gw)

    # Restart if running
    status = get_gateway_status(gateway_id)
    if status['running']:
        restart_gateway(gateway_id, gw)

    logger.info(f"Updated gateway: {gateway_id}")
    return {"id": gateway_id, "gateway": gw}

@app.delete("/api/gateways/{gateway_id}", dependencies=[Depends(auth_required)])
def delete_gateway(gateway_id: str):
    """Delete a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    # Stop and remove service
    remove_systemd_service(gateway_id)

    # Remove from config
    del config['gateways'][gateway_id]
    save_config(config)

    logger.info(f"Deleted gateway: {gateway_id}")
    return {"success": True, "message": f"Gateway {gateway_id} deleted"}

# =============================================================================
# Gateway Control Endpoints
# =============================================================================

@app.put("/api/gateways/{gateway_id}/start", dependencies=[Depends(auth_required)])
def api_start_gateway(gateway_id: str):
    """Start a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]

    if not gw.get('inputs') or not gw.get('outputs'):
        raise HTTPException(status_code=400, detail="Gateway needs at least one input and one output")

    if start_gateway(gateway_id, gw):
        # Update status cache immediately
        status = get_gateway_status(gateway_id)
        update_cached_status(gateway_id, status['running'], status['pid'], status['uptime'])
        return {"success": True, "message": f"Gateway {gateway_id} started"}
    else:
        raise HTTPException(status_code=500, detail="Failed to start gateway")

@app.put("/api/gateways/{gateway_id}/stop", dependencies=[Depends(auth_required)])
def api_stop_gateway(gateway_id: str):
    """Stop a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    if stop_gateway(gateway_id):
        # Update status cache immediately
        update_cached_status(gateway_id, False, None, None)
        return {"success": True, "message": f"Gateway {gateway_id} stopped"}
    else:
        raise HTTPException(status_code=500, detail="Failed to stop gateway")

@app.put("/api/gateways/{gateway_id}/restart", dependencies=[Depends(auth_required)])
def api_restart_gateway(gateway_id: str):
    """Restart a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]

    if restart_gateway(gateway_id, gw):
        # Update status cache immediately
        status = get_gateway_status(gateway_id)
        update_cached_status(gateway_id, status['running'], status['pid'], status['uptime'])
        return {"success": True, "message": f"Gateway {gateway_id} restarted"}
    else:
        raise HTTPException(status_code=500, detail="Failed to restart gateway")

@app.post("/api/gateways/bulk", dependencies=[Depends(auth_required)])
def bulk_operation(request: BulkOperation):
    """Perform bulk operation on multiple gateways"""
    operation = request.operation
    gateway_ids = request.gateway_ids

    if operation not in ['start', 'stop', 'restart']:
        raise HTTPException(status_code=400, detail="Invalid operation")

    config = load_config()
    gateways = config.get('gateways', {})

    results = {}
    for gw_id in gateway_ids:
        if gw_id not in gateways:
            results[gw_id] = {'success': False, 'error': 'Not found'}
            continue

        gw = gateways[gw_id]
        try:
            if operation == 'start':
                success = start_gateway(gw_id, gw)
            elif operation == 'stop':
                success = stop_gateway(gw_id)
            else:
                success = restart_gateway(gw_id, gw)
            results[gw_id] = {'success': success}
        except Exception as e:
            results[gw_id] = {'success': False, 'error': str(e)}

    return {"results": results}

# =============================================================================
# Peer Management Endpoints
# =============================================================================

@app.post("/api/gateways/{gateway_id}/inputs", dependencies=[Depends(auth_required)])
def add_input_peer(gateway_id: str, peer: PeerConfig):
    """Add an input peer to a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]
    if 'inputs' not in gw:
        gw['inputs'] = []

    gw['inputs'].append(peer.model_dump())
    save_config(config)

    # Update service and restart if running
    write_systemd_service(gateway_id, gw)
    status = get_gateway_status(gateway_id)
    if status['running']:
        restart_gateway(gateway_id, gw)

    return {"success": True, "inputs": gw['inputs']}

@app.delete("/api/gateways/{gateway_id}/inputs/{index}", dependencies=[Depends(auth_required)])
def remove_input_peer(gateway_id: str, index: int):
    """Remove an input peer from a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]
    inputs = gw.get('inputs', [])

    if index < 0 or index >= len(inputs):
        raise HTTPException(status_code=404, detail="Input peer not found")

    inputs.pop(index)
    gw['inputs'] = inputs
    save_config(config)

    # Update service and restart if running
    write_systemd_service(gateway_id, gw)
    status = get_gateway_status(gateway_id)
    if status['running']:
        restart_gateway(gateway_id, gw)

    return {"success": True, "inputs": gw['inputs']}

@app.post("/api/gateways/{gateway_id}/outputs", dependencies=[Depends(auth_required)])
def add_output_peer(gateway_id: str, peer: PeerConfig):
    """Add an output peer to a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]
    if 'outputs' not in gw:
        gw['outputs'] = []

    gw['outputs'].append(peer.model_dump())
    save_config(config)

    # Update service and restart if running
    write_systemd_service(gateway_id, gw)
    status = get_gateway_status(gateway_id)
    if status['running']:
        restart_gateway(gateway_id, gw)

    return {"success": True, "outputs": gw['outputs']}

@app.delete("/api/gateways/{gateway_id}/outputs/{index}", dependencies=[Depends(auth_required)])
def remove_output_peer(gateway_id: str, index: int):
    """Remove an output peer from a gateway"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    gw = gateways[gateway_id]
    outputs = gw.get('outputs', [])

    if index < 0 or index >= len(outputs):
        raise HTTPException(status_code=404, detail="Output peer not found")

    outputs.pop(index)
    gw['outputs'] = outputs
    save_config(config)

    # Update service and restart if running
    write_systemd_service(gateway_id, gw)
    status = get_gateway_status(gateway_id)
    if status['running']:
        restart_gateway(gateway_id, gw)

    return {"success": True, "outputs": gw['outputs']}

# =============================================================================
# System Endpoints
# =============================================================================

@app.get("/api/system/metrics", dependencies=[Depends(auth_required)])
def system_metrics():
    """Get system metrics"""
    cpu_percent = psutil.cpu_percent(interval=0.1)
    memory = psutil.virtual_memory()
    disk = psutil.disk_usage('/')

    # Network stats
    net_io = psutil.net_io_counters()

    return {
        "cpu_percent": cpu_percent,
        "memory": {
            "total": memory.total,
            "available": memory.available,
            "percent": memory.percent
        },
        "disk": {
            "total": disk.total,
            "used": disk.used,
            "free": disk.free,
            "percent": disk.percent
        },
        "network": {
            "bytes_sent": net_io.bytes_sent,
            "bytes_recv": net_io.bytes_recv,
            "packets_sent": net_io.packets_sent,
            "packets_recv": net_io.packets_recv
        }
    }

# =============================================================================
# Gateway Stats Endpoints
# =============================================================================

@app.get("/api/gateways/{gateway_id}/stats", dependencies=[Depends(auth_required)])
def get_gateway_metrics(gateway_id: str):
    """Get metrics for a gateway from its stats file"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail=f"Gateway '{gateway_id}' not found")

    service_name = f"ristgateway-{gateway_id}"
    stats = {}

    # Get cgroup status
    status = _check_cgroup(gateway_id)
    is_running = status['running']

    # Read stats from file
    if is_running:
        stats_data = _read_stats_file(service_name)
        if stats_data:
            stats = parse_rist_json_stats(stats_data)

    return {
        "gateway_id": gateway_id,
        "running": is_running,
        "stats": stats,
        "cpu_percent": status.get('cpu_percent'),
        "memory_mb": status.get('memory_mb'),
        "timestamp": datetime.now().isoformat()
    }

@app.get("/api/gateways/{gateway_id}/stats/history", dependencies=[Depends(auth_required)])
def get_gateway_metrics_history(gateway_id: str):
    """Get historical stats for graphing"""
    config = load_config()
    gateways = config.get('gateways', {})

    if gateway_id not in gateways:
        raise HTTPException(status_code=404, detail="Gateway not found")

    history = get_gateway_stats_history(gateway_id)

    return {
        "gateway_id": gateway_id,
        "history": history
    }

# =============================================================================
# Static Files (Web UI)
# =============================================================================

@app.get("/")
async def root():
    """Serve main page"""
    index_path = os.path.join(WEB_DIR, 'index.html')
    if os.path.exists(index_path):
        return FileResponse(index_path)
    return HTMLResponse("<h1>RIST Gateway API</h1><p>Web UI not found. API available at /api/</p>")

@app.get("/login")
async def login_page():
    """Serve login page"""
    login_path = os.path.join(WEB_DIR, 'login.html')
    if os.path.exists(login_path):
        return FileResponse(login_path)
    return HTMLResponse("<h1>Login</h1><p>Login page not found.</p>")

@app.get("/edit")
@app.get("/edit/{gateway_id}")
async def edit_page(gateway_id: str = None):
    """Serve edit page"""
    edit_path = os.path.join(WEB_DIR, 'edit.html')
    if os.path.exists(edit_path):
        return FileResponse(edit_path)
    return HTMLResponse("<h1>Edit Gateway</h1><p>Edit page not found.</p>")

@app.get("/stats/{gateway_id}")
async def stats_page(gateway_id: str):
    """Serve stats page"""
    stats_path = os.path.join(WEB_DIR, 'stats.html')
    if os.path.exists(stats_path):
        return FileResponse(stats_path)
    return HTMLResponse("<h1>Gateway Stats</h1><p>Stats page not found.</p>")

# Mount static files
if os.path.exists(WEB_DIR):
    app.mount("/static", StaticFiles(directory=WEB_DIR), name="static")

# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=80)
