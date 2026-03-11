#!/bin/bash
# RIST Gateway Installation Script
# Installs the modified librist with rist22rist gateway tool

set -e

INSTALL_DIR="/opt/ristgateway"
BRANCH="${RIST_BRANCH:-claude/pid-selection-oob-lkWRy}"  # Override with RIST_BRANCH env var

# Repository URL - supports SSH, HTTPS with token, or plain HTTPS
# Set GITHUB_TOKEN env var for private repo access, or use SSH
if [ -n "$GITHUB_TOKEN" ]; then
    REPO_URL="https://${GITHUB_TOKEN}@github.com/caritechsolutions/ristSTB.git"
elif [ -n "$USE_SSH" ] || [ -f ~/.ssh/id_ed25519 ] || [ -f ~/.ssh/id_rsa ]; then
    REPO_URL="git@github.com:caritechsolutions/ristSTB.git"
else
    REPO_URL="https://github.com/caritechsolutions/ristSTB.git"
fi

echo "========================================"
echo "RIST Gateway Installation"
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

# Install dependencies
echo ""
echo "Installing build dependencies..."
case $OS in
    ubuntu|debian)
        apt-get update
        apt-get install -y \
            git \
            build-essential \
            meson \
            ninja-build \
            libmbedtls-dev \
            libcjson-dev \
            libmicrohttpd-dev \
            pkg-config
        ;;
    centos|rhel|rocky|almalinux)
        dnf install -y epel-release || yum install -y epel-release
        dnf install -y \
            git \
            gcc \
            gcc-c++ \
            meson \
            ninja-build \
            mbedtls-devel \
            cjson-devel \
            libmicrohttpd-devel \
            pkgconfig
        ;;
    fedora)
        dnf install -y \
            git \
            gcc \
            gcc-c++ \
            meson \
            ninja-build \
            mbedtls-devel \
            cjson-devel \
            libmicrohttpd-devel \
            pkgconfig
        ;;
    *)
        echo "Unsupported OS: $OS"
        echo "Please install manually: git, meson, ninja, mbedtls-dev, cjson-dev"
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

# Build librist
echo ""
echo "Building librist..."
cd "$INSTALL_DIR/ristSTB/librist"

# Clean previous build if exists
if [ -d "build" ]; then
    rm -rf build
fi

# Configure with meson
meson setup build \
    --prefix=/usr/local \
    --buildtype=release \
    -Dbuiltin_cjson=false \
    -Dtest=false

# Build
ninja -C build

# Install
echo ""
echo "Installing..."
ninja -C build install

# Update library cache
ldconfig

# Verify installation
echo ""
echo "Verifying installation..."
if command -v rist22rist &> /dev/null; then
    echo "rist22rist installed successfully!"
    rist22rist --help | head -20
else
    echo "Warning: rist22rist not found in PATH"
    echo "It may be installed at /usr/local/bin/rist22rist"
    if [ -f /usr/local/bin/rist22rist ]; then
        echo "Found at /usr/local/bin/rist22rist"
        /usr/local/bin/rist22rist --help | head -20
    fi
fi

echo ""
echo "========================================"
echo "Installation complete!"
echo "========================================"
echo ""
echo "Installed tools:"
echo "  - rist22rist  (RIST gateway with multi-peer, NPD, SSRC passthrough)"
echo "  - rist2rist   (original simple RIST relay)"
echo "  - ristsender  (RIST sender)"
echo "  - ristreceiver (RIST receiver)"
echo ""
echo "Example usage:"
echo "  rist22rist -i 'rist://@:5000' -i 'rist://@:5001' \\"
echo "             -o 'rist://server1:6000' -o 'rist://server2:6001' \\"
echo "             -n -P"
echo ""
echo "Run 'rist22rist --help' for full options"
echo ""
