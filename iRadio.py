import json
import os
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox

# Configuration file matching your requested format
CONFIG_FILE = "sdr_config.json"

# Shown on the display while the receiver is suspended (same width as "104.500")
BLANK_DISPLAY = "---.---"

def load_last_frequency():
    """Load the last snapped frequency from disk or default to 104.5 MHz."""
    default_freq = 104500000.0
    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r") as f:
                data = json.load(f)
                return float(data.get("center_freq", default_freq))
        except Exception:
            pass
    return default_freq

def save_frequency(freq_hz):
    """Save the current frequency to disk."""
    try:
        with open(CONFIG_FILE, "w") as f:
            json.dump({"center_freq": float(freq_hz)}, f)
    except Exception as e:
        print(f"Error saving config: {e}", file=sys.stderr)

class SDRControllerApp:
    def __init__(self, root):
        self.root = root
        self.root.title("iRadio")
        self.root.geometry("400x350")
        self.root.configure(bg="#1e1e1e")  # Apple macOS Dark Mode background
        self.root.resizable(True, True)

        # Current frequency state in Hz
        self.current_freq_hz = load_last_frequency()
        self.last_freq_hz = self.current_freq_hz
        self.input_buffer = ""
        self.current_bandwidth = "w"  # Default wide band for normal tuning
        self.suspended = False        # True when CLEAR has stopped the receiver

        # SDR Process handler
        self.sdr_process = None

        self.create_widgets()
        self.start_sdr_backend()

    def create_widgets(self):
        # --- Top Title & Status Bar ---
        title_frame = tk.Frame(self.root, bg="#1e1e1e")
        title_frame.pack(fill="x", padx=16, pady=(16, 6))

        brand_label = tk.Label(
            title_frame, text="SDR TUNER", 
            fg="#8e8e93", bg="#1e1e1e", font=("-size", 10, "-weight", "bold")
        )
        brand_label.pack(side="left")

        self.status_label = tk.Label(
            title_frame, text="● LIVE", 
            fg="#30d158", bg="#1e1e1e", font=("-size", 9, "-weight", "bold")
        )
        self.status_label.pack(side="right")

        # --- Retro Display Panel ---
        display_frame = tk.Frame(
            self.root, bg="#0a0a0a", bd=4, relief="sunken"
        )
        display_frame.pack(fill="x", padx=16, pady=10, ipady=10)

        # Mode indicator (FM) and Units (MHz)
        info_subframe = tk.Frame(display_frame, bg="#0a0a0a")
        info_subframe.pack(fill="x", padx=10)

        tk.Label(
            info_subframe, text="FM STEREO", fg="#008888", bg="#0a0a0a", 
            font=("Courier", 9, "bold")
        ).pack(side="left")

        tk.Label(
            info_subframe, text="MHz", fg="#00ffff", bg="#0a0a0a", 
            font=("Courier", 10, "bold")
        ).pack(side="right")

        # Main 7-segment style digital frequency screen
        self.freq_display_var = tk.StringVar()
        self.update_display_string()

        self.freq_label = tk.Label(
            display_frame, textvariable=self.freq_display_var,
            fg="#00ffff", bg="#0a0a0a", font=("Courier", 32, "bold")
        )
        self.freq_label.pack(pady=5)

        # --- Keypad Panel (6x6 Grid Configuration using Frame + Label for guaranteed black styling) ---
        keypad_frame = tk.Frame(self.root, bg="#1e1e1e")
        keypad_frame.pack(padx=16, pady=4)

        buttons = [
            # Row 1 (6 columns)
            ('1', 0, 0), ('2', 0, 1), ('3', 0, 2), ('4', 0, 3), ('5', 0, 4), ('6', 0, 5),
            # Row 2 (6 columns)
            ('7', 1, 0), ('8', 1, 1), ('9', 1, 2), ('0', 1, 3), ('.', 1, 4), ('WX', 1, 5),
        ]

        for (text, r, c) in buttons:
            btn_frame = tk.Frame(keypad_frame, bg="#00ffff", bd=1, relief="solid")
            btn_frame.grid(row=r, column=c, padx=3, pady=3)

            btn = tk.Label(
                btn_frame, text=text, width=4, height=2,
                bg="#000000", fg="#ffffff", font=("-size", 11, "-weight", "bold"),
                cursor="hand2"
            )
            btn.pack(padx=1, pady=1)
            
            btn.bind("<Button-1>", lambda e, t=text: self.on_keypad_press(t))
            btn_frame.bind("<Button-1>", lambda e, t=text: self.on_keypad_press(t))

        # --- Row 3: "TUNE", "LAST" and "CLEAR" side-by-side ---
        bottom_actions_frame = tk.Frame(self.root, bg="#1e1e1e")
        bottom_actions_frame.pack(fill="x", padx=16, pady=(4, 12))

        actions = (
            ("TUNE",  self.apply_tuning,          (0, 3)),
            ("LAST",  self.recall_last_frequency, (3, 3)),
            ("CLEAR", self.clear_and_suspend,     (3, 0)),
        )

        for (text, handler, pad) in actions:
            container = tk.Frame(bottom_actions_frame, bg="#00ffff", bd=1, relief="solid")
            container.pack(side="left", expand=True, fill="x", padx=pad)

            btn = tk.Label(
                container, text=text, bg="#000000", fg="#ffffff",
                font=("-size", 10, "-weight", "bold"), cursor="hand2", pady=8
            )
            btn.pack(fill="x", padx=1, pady=1)

            btn.bind("<Button-1>", lambda e, h=handler: h())
            container.bind("<Button-1>", lambda e, h=handler: h())


    def update_display_string(self):
        """Format frequency in Hz to clean MHz display string (e.g., 104.50)"""
        mhz = self.current_freq_hz / 1000000.0
        self.freq_display_var.set(f"{mhz:06.3f}")

    def set_status(self, text, color):
        """Update the small status indicator in the title bar."""
        self.status_label.config(text=text, fg=color)

    def clear_and_suspend(self):
        """Blank the display and stop the receiver until a new frequency is tuned."""
        self.input_buffer = ""
        self.suspended = True
        self.freq_display_var.set(BLANK_DISPLAY)
        self.stop_sdr_backend()
        self.set_status("● STANDBY", "#ff9f0a")

    def on_keypad_press(self, char):
        if char == 'WX':
            # Save current as last before jumping to WX
            self.last_freq_hz = self.current_freq_hz
            self.current_freq_hz = 162550000.0
            self.current_bandwidth = "n"
            self.input_buffer = ""
            self.suspended = False
            self.update_display_string()
            save_frequency(self.current_freq_hz)
            self.restart_sdr_backend()
        elif char == '.':
            if '.' not in self.input_buffer:
                if not self.input_buffer:
                    self.input_buffer = "0."
                else:
                    self.input_buffer += "."
                self.freq_display_var.set(self.input_buffer)
        else:  # Digits 0-9
            if len(self.input_buffer) < 7:
                self.input_buffer += char
                self.freq_display_var.set(self.input_buffer)

    def apply_tuning(self):
        if self.input_buffer:
            try:
                entered_val = float(self.input_buffer)
                if entered_val < 1000:
                    target_freq = entered_val * 1000000.0
                else:
                    target_freq = entered_val
                
                # Save current as last before updating
                self.last_freq_hz = self.current_freq_hz
                self.current_freq_hz = target_freq
                self.current_bandwidth = "w"  # standard tuning uses wide band
                
                self.input_buffer = ""
                self.suspended = False
                self.update_display_string()
                save_frequency(self.current_freq_hz)
                self.restart_sdr_backend()
            except ValueError:
                messagebox.showerror("Error", "Invalid frequency input.")
        elif self.suspended:
            # Nothing entered and the receiver is stopped: stay in standby.
            return
        else:
            save_frequency(self.current_freq_hz)
            self.restart_sdr_backend()

    def recall_last_frequency(self):
        """Recall and tune to the last stored frequency."""
        temp = self.current_freq_hz
        self.current_freq_hz = self.last_freq_hz
        self.last_freq_hz = temp
        self.current_bandwidth = "w"
        
        self.input_buffer = ""
        self.suspended = False
        self.update_display_string()
        save_frequency(self.current_freq_hz)
        self.restart_sdr_backend()

    def start_sdr_backend(self):
        """Launch the C backend executable with stdout/stderr redirected to /dev/null."""
        if self.suspended:
            return

        freq_mhz_str = f"{self.current_freq_hz / 1e6:.4f}"
        binary_path = "./iRadio"
        
        if not os.path.exists(binary_path):
            print(f"Warning: {binary_path} not found locally. GUI running in mock mode.")
            self.set_status("● MOCK", "#ff9f0a")
            return

        try:
            cmd = [binary_path, "-f", freq_mhz_str, "-b", self.current_bandwidth, "-q", "-40"]
            self.sdr_process = subprocess.Popen(
                cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
            self.set_status("● LIVE", "#30d158")
        except Exception as e:
            print(f"Failed to start SDR backend: {e}", file=sys.stderr)
            self.set_status("● ERROR", "#ff453a")

    def stop_sdr_backend(self):
        """Safely terminate the current SDR process, if any."""
        if self.sdr_process:
            try:
                self.sdr_process.terminate()
                self.sdr_process.wait(timeout=1)
            except Exception:
                try:
                    self.sdr_process.kill()
                except Exception:
                    pass
            self.sdr_process = None

    def restart_sdr_backend(self):
        """Safely terminate the current SDR process and spin up the new frequency."""
        self.stop_sdr_backend()
        self.start_sdr_backend()

    def on_close(self):
        if self.sdr_process:
            try:
                self.sdr_process.terminate()
            except Exception:
                pass
        self.root.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = SDRControllerApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
