#!/usr/bin/env python3
"""Keep the board's USB serial open (opening it resets the board) and expose it on a local TCP port.

Each TCP client writes `<id> <command>` lines and reads every line the board prints. Board log lines go
to stdout. Usage: serial_bridge.py [port] [tcp_port]
"""
import glob, socket, sys, threading, time
import serial

dev = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].startswith('/dev/') else glob.glob('/dev/cu.usbmodem*')[0]
tcp = int(sys.argv[-1]) if sys.argv[-1].isdigit() else 3333
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout, ser.dtr, ser.rts = dev, 115200, 0.1, False, False
ser.open()
clients, lock = [], threading.Lock()


def from_board():
    buf = b''
    while True:
        buf += ser.read(1024)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            print(time.strftime('%H:%M:%S'), line.decode(errors='replace').rstrip(), flush=True)
            with lock:
                for c in list(clients):
                    try: c.sendall(line + b'\n')
                    except OSError: clients.remove(c)


def serve(conn):
    with lock: clients.append(conn)
    f = conn.makefile('rb')
    for line in f:
        ser.write(line.rstrip(b'\r\n') + b'\n')
    with lock:
        if conn in clients: clients.remove(conn)


threading.Thread(target=from_board, daemon=True).start()
srv = socket.create_server(('127.0.0.1', tcp))
print(f'bridge {dev} <-> 127.0.0.1:{tcp}', flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
