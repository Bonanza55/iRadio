Open source SDR for macOS and Linux. 
Requires a NOOELEC Smart SDR.

Files included: iRadio.c, iRadio.py, Makefile, setup.sh

Prerequisites:
- macOS: Xcode Command Line Tools & Homebrew (handled automatically by setup.sh)
- Linux: apt-get package manager (Ubuntu/Debian)

Installation Instructions:

0) mkdir -p ~/RADIO
1) cd ~/RADIO
2) git clone https://github.com/Bonanza55/iRadio.git .
3) chmod +x setup.sh && ./setup.sh
4) make clean || true
5) make all