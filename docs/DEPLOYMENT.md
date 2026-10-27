# Deploying OSISOS Online (Vercel, GitHub Pages, Netlify)

Because OSISOS now includes a client-side **WebAssembly (Wasm) x86 PC runner**, you can deploy the entire operating system and presentation deck online for **free** on any static hosting platform.

Anyone with a modern web browser (on Windows, macOS, Linux, ChromeOS, iPad, or Android) can open your link and run your x86 operating system live without downloading or installing anything!

---

## Recommended Deployment Services

| Platform | Best For | Setup Time | URL Style |
|---|---|:---:|---|
| **Vercel** *(Recommended)* | Instant deployment, fastest CDN, custom domains | ~1 minute | `https://osisos.vercel.app` |
| **GitHub Pages** | Zero external accounts needed (built right into GitHub) | ~1 minute | `https://<username>.github.io/OSISOS` |
| **Netlify** | Simple drag-and-drop or Git integration | ~1 minute | `https://osisos.netlify.app` |

---

## Method 1: Deploy to Vercel (Recommended)

Vercel provides edge caching, automatic HTTPS, and instant rebuilds whenever you push changes.

### Option A: Via the Vercel Web Dashboard (Easiest)
1. **Push your latest commits to GitHub**:
   ```bash
   git push origin main
   ```
2. Go to [https://vercel.com/new](https://vercel.com/new) and log in with your GitHub account.
3. Find your **`OSISOS`** repository and click **Import**.
4. Configure the Project:
   - **Framework Preset:** `Other`
   - **Root Directory:** `./`
   - **Build Command:** *(Leave empty)*
   - **Output Directory:** `./`
5. Click **Deploy**.
6. Within 15–20 seconds, your site will be live at `https://osisos-<your-username>.vercel.app`!

### Option B: Via Terminal CLI
If you have Node.js installed, you can deploy straight from your shell:
```bash
npx vercel
```
- Set up and deploy? **Yes**
- Which scope? **Select your account**
- Link to existing project? **No**
- Project name: **osisos**
- In which directory is your code located? **`./`**
- Want to modify settings? **No**

When ready for production:
```bash
npx vercel --prod
```

### URLs on Vercel:
- **Presentation Deck:** `https://your-project.vercel.app/`
- **Live x86 Machine:** `https://your-project.vercel.app/web-runner.html` (or `https://your-project.vercel.app/run` or `/vm`)

---

## Method 2: Deploy to GitHub Pages (Zero Extra Accounts)

Since your code is already in a GitHub repository, you can enable GitHub Pages with two clicks:

1. **Push to GitHub**:
   ```bash
   git push origin main
   ```
2. Open your repository on GitHub in your browser.
3. Click on **Settings** (top menu bar) → **Pages** (left sidebar under "Code and automation").
4. Under **Build and deployment**:
   - **Source:** Select `Deploy from a branch`.
   - **Branch:** Select `main` and folder `/(root)`.
5. Click **Save**.
6. Wait ~30–60 seconds. Refresh the page, and GitHub will display:
   > *"Your site is live at `https://<your-username>.github.io/OSISOS/`"*

*(Note: We have already added `.nojekyll` to the repository to ensure all WebAssembly files and ROMs are served directly without interference).*

---

## Method 3: Deploy to Netlify

1. Go to [https://app.netlify.com/](https://app.netlify.com/).
2. Select **Add new site** → **Import an existing project** → **GitHub**.
3. Authorize and choose your **`OSISOS`** repository.
4. Leave the build command blank and publish directory as `.`.
5. Click **Deploy OSISOS**.

---

## What Gets Served

When deployed, your static website serves:
- `index.html`: The 18-slide interactive presentation deck with slide navigation, speaker notes drawer (<kbd>N</kbd>), overview grid (<kbd>O</kbd>), and presenter timer.
- `web-runner.html`: The full-screen WebAssembly x86 PC environment that boots `wasm/osisos.bin` (via Multiboot in ~50ms) or `wasm/osisos.iso` with 32 MB RAM and IDE hard disk (`wasm/drive.img`).
- `wasm/`: Prepackaged WebAssembly emulator (`v86.wasm`), runtime (`libv86.js`), SeaBIOS ROMs, and the OSISOS kernel binary.
