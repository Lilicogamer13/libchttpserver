import threading
import time
import http_server as hs

app = hs.FileRouter(8080)


app.add_file_route("/", "./www/index.html")


def background_monitor(stop_event):
    """This thread runs in the background and can perform periodic checks

    or other tasks while the server is running.
    """
    i: int = 0
    print("[Monitor Thread] Started.")
    while not stop_event.is_set():
        # You can do other background checks or tasks here
        i += 1
        print(i, end=", ", flush=True)
        time.sleep(0.1)

    print("[Monitor Thread] Shutting down...")


# 1. Start your server in the background
app.enable_https("cert.pem", "key.pem")
app.start(background=True)
print("Server is running...")

# 2. Setup a threading Event to coordinate a clean shutdown
shutdown_event = threading.Event()

# 3. Start your custom background monitor thread
monitor_thread = threading.Thread(target=background_monitor, args=(shutdown_event,))
# Making it a daemon ensures it dies if the main thread crashes unexpectedly
monitor_thread.daemon = True
monitor_thread.start()

# 4. Use the main thread strictly to listen for Ctrl+C
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\nCtrl+C detected! Triggering background shutdown...")
    # Signal the background thread to stop its loop
    shutdown_event.set()
    # Wait for the background thread to finish cleanly
    monitor_thread.join()

print("All threads stopped. Server closed gracefully.")
