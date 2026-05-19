import tkinter as tk
from tkinter import ttk, messagebox
import subprocess
import threading
import shutil
import os

CHUNK_SIZE = 1024 * 1024  # 1MB


def get_removable_drives():
    result = subprocess.check_output(
        'wmic logicaldisk where "drivetype=2" get name,size',
        shell=True
    ).decode(errors="ignore")

    drives = []

    lines = result.strip().splitlines()[1:]

    for line in lines:

        parts = line.split()

        if len(parts) >= 1:

            drive = parts[0]

            size = "Unknown"

            if len(parts) >= 2:
                try:
                    size_gb = round(int(parts[1]) / (1024**3), 2)
                    size = f"{size_gb} GB"
                except:
                    pass

            drives.append({
                "letter": drive,
                "size": size
            })

    return drives


class USBWiper:

    def __init__(self, root):

        self.root = root
        self.root.title("USB Zero Wiper")
        self.root.geometry("500x250")

        self.drives = get_removable_drives()

        tk.Label(
            root,
            text="Select USB Drive",
            font=("Arial", 12)
        ).pack(pady=10)

        self.combo = ttk.Combobox(root, width=50)

        self.combo.pack(pady=5)

        values = []

        for d in self.drives:
            values.append(
                f"{d['letter']} ({d['size']})"
            )

        self.combo["values"] = values

        self.progress = ttk.Progressbar(
            root,
            length=400
        )

        self.progress.pack(pady=15)

        self.status = tk.Label(
            root,
            text="Idle"
        )

        self.status.pack()

        self.button = tk.Button(
            root,
            text="WRITE ZEROES",
            command=self.confirm
        )

        self.button.pack(pady=20)

    def confirm(self):

        selected = self.combo.current()

        if selected == -1:
            messagebox.showerror(
                "Error",
                "Select a USB drive first"
            )
            return

        yes = messagebox.askyesno(
            "WARNING",
            "THIS WILL FILL THE USB WITH ZEROES.\n\nContinue?"
        )

        if yes:
            threading.Thread(
                target=self.wipe,
                daemon=True
            ).start()

    def wipe(self):

        selected = self.combo.current()

        drive = self.drives[selected]["letter"]

        path = os.path.join(
            drive + "\\",
            "ZERO_FILL.bin"
        )

        self.status.config(
            text=f"Writing to {path}"
        )

        try:

            free_space = shutil.disk_usage(
                drive + "\\"
            ).free

            written = 0

            with open(path, "wb") as f:

                chunk = b"\x00" * CHUNK_SIZE

                while written < free_space - CHUNK_SIZE:

                    f.write(chunk)

                    written += CHUNK_SIZE

                    percent = (
                        written / free_space
                    ) * 100

                    self.progress["value"] = percent

                    self.status.config(
                        text=f"Writing zeroes... {round(percent, 1)}%"
                    )

                    self.root.update_idletasks()

            self.status.config(
                text="Completed"
            )

            messagebox.showinfo(
                "Done",
                "USB filled with zeroes successfully."
            )

        except Exception as e:

            messagebox.showerror(
                "Error",
                str(e)
            )


root = tk.Tk()

app = USBWiper(root)

root.mainloop()