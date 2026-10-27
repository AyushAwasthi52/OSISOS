# OSISOS Interactive Presentation Deck

A standalone, interactive HTML5 presentation deck showcasing the engineering, architecture, and working of **OSISOS** (a 32-bit Multitasking Operating System and Custom TCP/IP Stack).

---

## How to Open & Present

### Method 1: Direct File Open
Double-click `index.html` or run:
```bash
xdg-open index.html
```

### Method 2: Local HTTP Server (Recommended)
```bash
python3 -m http.server 3000
# Open your browser at: http://localhost:3000/
```

---

## Features
- **18 Beautiful Slides:** Covers Boot Journey, GDT/IDT/PIC, PMM, 2-Level Paging, Preemptive Multitasking, Intel E1000 NIC driver, OFS v2 Inode Filesystem, SHA-256 PBKDF2 Auth, Full RFC 793 TCP State Machine, Wi-Fi LAN Remote Messaging, and War Stories.
- **Interactive Terminal Demo (Slide 15):** Embedded terminal simulation where you can run real OSISOS commands (`sysinfo`, `ifconfig`, `httpd`, `df`, `dmesg`, `matrix`, `cowsay`) during your presentation.
- **Presenter Clock:** Live elapsed timer in the bottom HUD.
- **Speaker Notes Drawer (<kbd>N</kbd>):** Complete talking points and script for each slide.
- **Slide Overview Modal (<kbd>O</kbd> / <kbd>Esc</kbd>):** Visual thumbnail grid to jump to any slide instantly.
- **PDF Handout Generation (<kbd>Ctrl</kbd> + <kbd>P</kbd>):** Clean print layout rendering each slide as a standalone 16:9 landscape page.

---

## Full Defense Guide & Viva Preparation
See [`docs/PRESENTATION_GUIDE.md`](../docs/PRESENTATION_GUIDE.md) for the complete slide-by-slide script and answers to the top 12 college examination questions.
