#!/usr/bin/env python3
# Intermedio transparente — captura trafico WiFi y lo reenvía
import socket, threading, struct

LISTEN_IP   = "192.168.99.1"
LISTEN_PORT = 12345

def get_original_dest(sock):
    # SO_ORIGINAL_DST — obtener destino real antes de iptables
    SO_ORIGINAL_DST = 80
    dst = sock.getsockopt(socket.SOL_IP, SO_ORIGINAL_DST, 16)
    port = struct.unpack("!H", dst[2:4])[0]
    ip   = socket.inet_ntoa(dst[4:8])
    return ip, port

def pipe(src, dst, label):
    try:
        while True:
            d = src.recv(4096)
            if not d: break
            print(f"  [{label}] {len(d)} bytes", flush=True)
            dst.sendall(d)
    except: pass
    try: src.close()
    except: pass
    try: dst.close()
    except: pass

def handle(client, addr):
    try:
        dst_ip, dst_port = get_original_dest(client)
        print(f"[+] {addr[0]} -> {dst_ip}:{dst_port}", flush=True)
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.connect((dst_ip, dst_port))
        threading.Thread(target=pipe, args=(client, server, "->"), daemon=True).start()
        pipe(server, client, "<-")
    except Exception as e:
        print(f"[!] Error: {e}", flush=True)
        try: client.close()
        except: pass

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((LISTEN_IP, LISTEN_PORT))
srv.listen(200)
print(f"Intermedio activo en {LISTEN_IP}:{LISTEN_PORT}", flush=True)
print("Redirigí el trafico con:", flush=True)
print(f"  iptables -t nat -A PREROUTING -i wlp2s0 -p tcp -j REDIRECT --to-ports {LISTEN_PORT}", flush=True)
print("", flush=True)

while True:
    try:
        c, a = srv.accept()
        threading.Thread(target=handle, args=(c, a), daemon=True).start()
    except KeyboardInterrupt:
        break
