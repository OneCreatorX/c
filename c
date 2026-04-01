pkill -f redsocks
pkill -f xray
iptables -t nat -F OUTPUT
iptables -t nat -F REDSOCKS
iptables -t nat -X REDSOCKS
#!/usr/bin/env python3
# Test de conexion minimo — muestra cada paso
import socket, time, sys

CLIENT_ID  = "sz67pu5p"
PROXY_HOST = "emailmarketing.personal.com.ar"
PROXY_IPV6 = "2606:4700::6812:16b7"
PROXY_PORT = 80
TUNNEL_HOST = "1.brawlpass.com.ar"

print("=== XTunnel test de conexion ===")
print(f"ID: {CLIENT_ID}")
print()

# Paso 1 - resolver DNS
print("[1] Resolviendo DNS...")
addrs = []
try:
    r = socket.getaddrinfo(PROXY_IPV6, PROXY_PORT, socket.AF_INET6, socket.SOCK_STREAM)
    addrs.append((r[0][4], socket.AF_INET6))
    print(f"    IPv6 ok: {PROXY_IPV6}")
except Exception as e:
    print(f"    IPv6 fallo: {e}")

try:
    r = socket.getaddrinfo(PROXY_HOST, PROXY_PORT, socket.AF_UNSPEC, socket.SOCK_STREAM)
    for info in r:
        addrs.append((info[4], info[0]))
        print(f"    IPv4 ok: {info[4][0]}")
except Exception as e:
    print(f"    IPv4 fallo: {e}")

if not addrs:
    print("ERROR: Sin direcciones. Verificar internet.")
    sys.exit(1)

# Paso 2 - conectar socket
print()
print("[2] Conectando socket...")
sock = None
for addr, fam in addrs:
    try:
        s = socket.socket(fam, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(addr)
        s.settimeout(None)
        sock = s
        print(f"    Conectado a {addr[0]}")
        break
    except Exception as e:
        print(f"    Fallo {addr[0]}: {e}")

if not sock:
    print("ERROR: No se pudo conectar.")
    sys.exit(1)

# Paso 3 - P1
print()
print("[3] Enviando P1...")
p1 = f"GET / HTTP/1.1\r\nHost: {PROXY_HOST}\r\n\r\n"
sock.sendall(p1.encode())
print("    P1 enviado")
time.sleep(0.01)

# Paso 4 - P2
print()
print("[4] Enviando P2...")
t0 = time.time()
p2 = (f"- / HTTP/1.1\r\nHost: {TUNNEL_HOST}\r\nUpgrade: websocket\r\n"
      f"Action: tunnel\r\nX-Client-Id: {CLIENT_ID}\r\n\r\n")
sock.sendall(p2.encode())
print("    P2 enviado")

# Paso 5 - respuesta
print()
print("[5] Esperando respuesta del servidor...")
sock.settimeout(8)
raw = b""
try:
    while b"\r\n\r\n" not in raw:
        c = sock.recv(4096)
        if not c: break
        raw += c
except socket.timeout:
    print("    TIMEOUT - no respondio en 8 segundos")
    sock.close()
    sys.exit(1)

ms = int((time.time() - t0) * 1000)
print(f"    Respuesta en {ms}ms:")
print()
print("--- RESPUESTA CRUDA ---")
print(raw.decode(errors="replace"))
print("-----------------------")

if b"101" in raw[:30]:
    print()
    print("EXITO: Handshake 101 OK - tunel establecido")
else:
    print()
    print("ERROR: No se recibio 101")

sock.close()
