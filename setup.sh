#!/usr/bin/env bash
set -e

# Detect Operating System
OS="$(uname -s)"
echo "===> Detecting operating system: $OS"

if [ "$OS" = "Darwin" ]; then
    echo "===> Configuring for macOS..."

    # Check for Xcode Command Line Tools
    if ! xcode-select -p &> /dev/null; then
        echo "===> Xcode Command Line Tools not found. Requesting installation..."
        echo "===> A popup window may appear. Please click 'Install' and re-run this script when finished."
        xcode-select --install
        echo "Error: Please wait for the Xcode Command Line Tools installation to complete, then re-run ./setup.sh"
        exit 1
    fi

    if ! command -v brew &> /dev/null; then
        echo "Error: Homebrew is required on macOS. Install it from https://brew.sh/"
        exit 1
    fi

    echo "===> Updating Homebrew and installing C libraries & tools..."
    brew update
    brew install librtlsdr ffmpeg pkg-config

elif [ "$OS" = "Linux" ]; then
    echo "===> Configuring for Linux..."

    if ! command -v apt-get &> /dev/null; then
        echo "Error: apt-get package manager not found."
        exit 1
    fi

    echo "===> Updating apt repository and installing C libraries & Python GUI dependencies..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        librtlsdr-dev \
        libusb-1.0-0-dev \
        ffmpeg \
        pkg-config \
        python3-pip \
        python3-tk

    echo "===> Installing Python packages globally (no venv)..."
    python3 -m pip install --upgrade pip --break-system-packages || python3 -m pip install --upgrade pip

else
    echo "Error: Unsupported operating system: $OS"
    exit 1
fi

echo "===> Environment setup complete!"