#!/bin/bash
# RIST Sender Installation Script
# Installs the RIST Sender web management interface
# NOTE: Does NOT install librist - assumes it's already installed on the system
#
# Usage:
#   GITHUB_TOKEN=xxx ./install.sh          # Set token via env
#   ./install.sh --token ghp_xxxxx         # Set token via arg
#   ./install.sh                           # Uses SSH or prompts

set -e

INSTALL_DIR="/opt/ristsender"
BRANCH="${RIST_BRANCH:-claude/pid-selection-oob-lkWRy}"  # Override with RIST_BRANCH env var

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --token)
            GITHUB_TOKEN="$2"
            shift 2
            ;;
        --branch)
            BRANCH="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

# Repository URL - supports SSH, HTTPS with token, or plain HTTPS
if [ -n "$GITHUB_TOKEN" ]; then
    REPO_URL="https://${GITHUB_TOKEN}@github.com/caritechsolutions/ristSTB.git"
elif [ -n "$USE_SSH" ] || [ -f ~/.ssh/id_ed25519 ] || [ -f ~/.ssh/id_rsa ]; then
    REPO_URL="git@github.com:caritechsolutions/ristSTB.git"
else
    REPO_URL="https://github.com/caritechsolutions/ristSTB.git"
fi

echo "========================================"
echo "RIST Sender Installation"
echo "========================================"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo ./install.sh)"
    exit 1
fi

echo "Repository: $REPO_URL"
echo "Branch: $BRANCH"

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    echo "Cannot detect OS"
    exit 1
fi

echo "Detected OS: $OS"

# Check if ristsender binary exists
if ! command -v ristsender &> /dev/null && [ ! -f /usr/local/bin/ristsender ]; then
    echo ""
    echo "[WARNING] ristsender binary not found!"
    echo "Please ensure librist is installed with ristsender tool."
    echo "You may need to run the ristgateway install.sh first to build librist."
    exit 1
fi

echo "[OK] ristsender binary found"

# Install dependencies (minimal - just Python and web dependencies)
echo ""
echo "Installing dependencies..."
case $OS in
    ubuntu|debian)
        apt-get update
        apt-get install -y \
            git \
            python3 \
            python3-pip \
            python3-venv \
            ffmpeg
        ;;
    centos|rhel|rocky|almalinux)
        dnf install -y epel-release || yum install -y epel-release
        dnf install -y \
            git \
            python3 \
            python3-pip \
            ffmpeg
        ;;
    fedora)
        dnf install -y \
            git \
            python3 \
            python3-pip \
            ffmpeg
        ;;
    *)
        echo "Unsupported OS: $OS"
        echo "Please install manually: git, python3"
        exit 1
        ;;
esac

# Create install directory
echo ""
echo "Creating installation directory: $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
cd "$INSTALL_DIR"

# Clone or update repository
if [ -d "$INSTALL_DIR/ristSTB" ]; then
    echo "Repository exists, pulling latest changes..."
    cd "$INSTALL_DIR/ristSTB"
    git fetch origin
    git checkout $BRANCH
    git pull origin $BRANCH
else
    echo "Cloning repository..."
    git clone "$REPO_URL" "$INSTALL_DIR/ristSTB"
    cd "$INSTALL_DIR/ristSTB"
    git checkout $BRANCH
fi

# Install Web API
echo ""
echo "Installing Sender Web API..."

# Create directories
mkdir -p /etc/ristsender
mkdir -p /var/log/ristsender
mkdir -p /var/run/ristsender

# Create Python virtual environment
VENV_DIR="$INSTALL_DIR/venv"
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi

# Install Python dependencies
source "$VENV_DIR/bin/activate"
pip install --upgrade pip
pip install -r "$INSTALL_DIR/ristSTB/ristsender/requirements.txt"
deactivate

# Copy web files
cp -r "$INSTALL_DIR/ristSTB/ristsender/web" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristsender/sender_api.py" "$INSTALL_DIR/"
cp "$INSTALL_DIR/ristSTB/ristsender/stats_collector.sh" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/stats_collector.sh"
cp "$INSTALL_DIR/ristSTB/ristsender/sender_config.yaml" /etc/ristsender/ 2>/dev/null || true

# Create shared memory directories (RAM-backed, no disk I/O)
mkdir -p /dev/shm/ristsender-stats

# Create systemd service for stats collector
cat > /etc/systemd/system/ristsender-stats.service << 'EOF'
[Unit]
Description=RIST Sender Stats Collector
After=network.target

[Service]
Type=simple
ExecStart=/opt/ristsender/stats_collector.sh
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

# Create systemd service for API (port 8080 to avoid conflicts)
cat > /etc/systemd/system/ristsender-api.service << 'EOF'
[Unit]
Description=RIST Sender Web API
After=network.target ristsender-stats.service

[Service]
Type=simple
User=root
WorkingDirectory=/opt/ristsender
Environment="SENDER_CONFIG=/etc/ristsender/sender_config.yaml"
ExecStart=/opt/ristsender/venv/bin/python /opt/ristsender/sender_api.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=ristsender-api

[Install]
WantedBy=multi-user.target
EOF

# Reload systemd and enable services
systemctl daemon-reload
systemctl enable ristsender-stats.service
systemctl enable ristsender-api.service
systemctl start ristsender-stats.service
systemctl start ristsender-api.service

echo ""
echo "========================================"
echo "Installation complete!"
echo "========================================"
echo ""
echo "Installed components:"
echo "  - Web API     (http://localhost:8080)"
echo "  - Stats Collector (writes to /dev/shm)"
echo ""
echo "Web Interface:"
echo "  URL: http://$(hostname -I | awk '{print $1}'):8080"
echo "  Default password: admin"
echo ""
echo "Note: This installation uses the existing ristsender binary from librist."
echo "      If ristsender is not working, run the ristgateway install.sh to build librist."
echo ""
