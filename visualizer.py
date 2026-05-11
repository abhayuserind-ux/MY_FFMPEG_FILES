import subprocess
import re
import sys
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.patches as patches
import matplotlib.lines as mlines

# ──────────────────────────────────────────────
#  BLUR WAVEFORM — live-updating wave graph
# ──────────────────────────────────────────────

def parse_and_plot_blur(executable_command):
    print(f"Executing: {executable_command}\nStreaming blur waveform...\n{'-'*50}")

    process = subprocess.Popen(
        ['bash', '-c', executable_command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    data_pattern   = re.compile(r"BLUR_DATA:\s+time=([\d.]+)\s+score=([\d.]+)")
    alert_pattern  = re.compile(r"BLUR_ALERT:\s+([\d.]+)")
    config_pattern = re.compile(r"BLUR_CONFIG:\s+threshold=([\d.]+)")

    times       = []
    scores      = []
    alert_times = []
    threshold   = 15.0          # fallback — overwritten by BLUR_CONFIG line

    # ── Live matplotlib setup ──
    plt.ion()
    fig, ax = plt.subplots(figsize=(12, 4))
    fig.patch.set_facecolor('#0e1117')
    ax.set_facecolor('#161b22')
    for spine in ax.spines.values():
        spine.set_edgecolor('#30363d')

    ax.set_title("Blur Score — Live Waveform", color='#e6edf3', fontsize=13, pad=10)
    ax.set_xlabel("Time (seconds)", color='#8b949e')
    ax.set_ylabel("Blur Score", color='#8b949e')
    ax.tick_params(colors='#8b949e')

    wave_line,   = ax.plot([], [], color='#58a6ff', linewidth=1.2, zorder=3)
    thresh_line  = ax.axhline(threshold, color='#f85149', linewidth=1,
                              linestyle='--', alpha=0.8, label=f'Threshold ({threshold})', zorder=4)
    fill_poly    = None       # rebuilt each redraw
    alert_vlines = []

    # Track when to refresh (every 30 frames to keep it snappy)
    REFRESH_EVERY = 30
    frame_count = 0

    def redraw():
        nonlocal fill_poly
        if not times:
            return
        wave_line.set_data(times, scores)
        ax.set_xlim(0, max(times[-1] + 1, 5))
        ax.set_ylim(0, max(max(scores) * 1.15, threshold * 1.5))

        # Rebuild shaded region above threshold
        if fill_poly:
            fill_poly.remove()
        fill_poly = ax.fill_between(
            times, scores, threshold,
            where=[s > threshold for s in scores],
            color='#f85149', alpha=0.25, zorder=2
        )
        # Threshold line always reflects latest parsed value
        thresh_line.set_ydata([threshold, threshold])
        thresh_line.set_label(f'Threshold ({threshold:.1f})')

        ax.legend(facecolor='#161b22', edgecolor='#30363d',
                  labelcolor='#e6edf3', fontsize=9, loc='upper left')
        fig.canvas.draw()
        plt.pause(0.001)

    # ── Stream loop ──
    for raw_line in process.stdout:
        print(raw_line, end='')

        cfg = config_pattern.search(raw_line)
        if cfg:
            threshold = float(cfg.group(1))
            thresh_line.set_ydata([threshold, threshold])
            thresh_line.set_label(f'Threshold ({threshold:.1f})')

        m = data_pattern.search(raw_line)
        if m:
            times.append(float(m.group(1)))
            scores.append(float(m.group(2)))
            frame_count += 1
            if frame_count % REFRESH_EVERY == 0:
                redraw()

        a = alert_pattern.search(raw_line)
        if a:
            t = float(a.group(1))
            alert_times.append(t)
            vl = ax.axvline(t, color='#ffa657', linewidth=0.8, alpha=0.6, zorder=5)
            alert_vlines.append(vl)

    process.wait()
    redraw()       # final full redraw
    plt.ioff()

    print(f"\n{'-'*50}")
    print(f"Done. {len(times)} frames plotted, {len(alert_times)} blur alerts.")

    # ── Final static annotation pass ──
    if alert_times:
        # Cluster nearby alerts into labelled spans so the graph isn't cluttered
        clusters = []
        start = alert_times[0]
        prev  = alert_times[0]
        for t in alert_times[1:]:
            if t - prev > 1.0:        # gap > 1 s = new cluster
                clusters.append((start, prev))
                start = t
            prev = t
        clusters.append((start, prev))

        for (cs, ce) in clusters:
            mid = (cs + ce) / 2
            ax.annotate(
                "blur",
                xy=(mid, threshold),
                xytext=(mid, ax.get_ylim()[1] * 0.88),
                color='#ffa657', fontsize=7.5,
                ha='center', va='top',
                arrowprops=dict(arrowstyle='->', color='#ffa657', lw=0.8)
            )

    plt.tight_layout()
    plt.show(block=True)


# ──────────────────────────────────────────────
#  ORIGINAL FREEZE / SILENCE DETECTOR (unchanged)
# ──────────────────────────────────────────────

def parse_and_plot(executable_command, is_audio):
    print(f"Executing: {executable_command}\nStreaming real-time output...\n{'-'*50}")

    process = subprocess.Popen(
        ['bash', '-c', executable_command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    events = []
    current_start = None

    if not is_audio:
        start_pattern = r"starts at\s+([\d.]+)\s+seconds"
        end_pattern   = r"over at\s+([\d.]+)\s+seconds"
        event_name    = "Video Freeze"
        color         = "#e74c3c"
    else:
        start_pattern = r"started at\s+([\d.]+)\s+seconds"
        end_pattern   = r"ended at\s+([\d.]+)\s+seconds"
        event_name    = "Audio Silence"
        color         = "#3498db"

    for line in process.stdout:
        print(line, end='')

        start_match = re.search(start_pattern, line, re.IGNORECASE)
        if start_match:
            current_start = float(start_match.group(1))

        end_match = re.search(end_pattern, line, re.IGNORECASE)
        if end_match and current_start is not None:
            current_end = float(end_match.group(1))
            events.append((current_start, current_end))
            current_start = None

    process.wait()
    print(f"\n{'-'*50}\nProcess finished. Found {len(events)} events.")

    if not events and current_start is None:
        print("⚠️  No events detected. Check printf output or pipe buffering.")

    if current_start is not None:
        events.append((current_start, current_start + 2.0))

    fig, ax = plt.subplots(figsize=(10, 3))
    ax.set_title(f"Dynamic {event_name} Detection Timeline")
    ax.set_xlabel("Timeline (seconds)")
    ax.set_ylabel("Detection Status")
    ax.set_yticks([0, 1])
    ax.set_yticklabels(["Normal", "Detected"])
    ax.set_ylim(-0.5, 1.5)
    ax.axhline(0, color='gray', linewidth=1, linestyle='--')

    for start, end in events:
        rect = patches.Rectangle(
            (start, 0), end - start, 1,
            linewidth=1, edgecolor=color, facecolor=color, alpha=0.7
        )
        ax.add_patch(rect)

    if events:
        ax.set_xlim(max(0, events[0][0] - 5), events[-1][1] + 5)

    plt.tight_layout()
    plt.show()


# ──────────────────────────────────────────────
#  ENTRY POINT
# ──────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 visualizer.py '<command>' <video|audio|blur>")
        sys.exit(1)

    cmd  = sys.argv[1]
    mode = sys.argv[2].lower()

    if mode == 'blur':
        parse_and_plot_blur(cmd)
    elif mode == 'audio':
        parse_and_plot(cmd, is_audio=True)
    else:
        parse_and_plot(cmd, is_audio=False)