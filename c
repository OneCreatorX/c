#!/bin/bash
# XTunnel — modo prueba
# Uso: sudo bash xtunnel-test.sh

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${GREEN}[✓]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[✗]${NC} $*"; }

[ "$(id -u)" -ne 0 ] && { error "Ejecutar como root: sudo bash xtunnel-test.sh"; exit 1; }

INSTALL_DIR="/opt/xtunnel"
CONFIG_FILE="$INSTALL_DIR/config.conf"
CLIENT_PY="$INSTALL_DIR/btclient.py"
XRAY_BIN="$INSTALL_DIR/xray"
SOCKS_PORT=10808
TUNNEL_PORT=10809
VLESS_UUID="a3482e88-686a-4a58-8126-99c9df64b7bf"

# ── Generar ID corto si no existe ─────────────────────────────────────────────
generate_id() {
    python3 -c "
import random, string
chars = string.ascii_lowercase + string.digits
print(''.join(random.choices(chars, k=8)))
"
}

load_config() {
    [ -f "$CONFIG_FILE" ] && source "$CONFIG_FILE"
}

save_config() {
    mkdir -p "$INSTALL_DIR"
    cat > "$CONFIG_FILE" << EOF
CLIENT_ID="$CLIENT_ID"
PANEL_URL="$PANEL_URL"
PANEL_TOKEN="$PANEL_TOKEN"
HOTSPOT_SSID="XTunnel"
HOTSPOT_PASS="xtunnel123"
EOF
}

# ── Instalación mínima ────────────────────────────────────────────────────────
install_deps() {
    info "Instalando dependencias..."
    apt-get update -qq
    apt-get install -y -qq python3 curl wget unzip iproute2 iptables tun2socks 2>/dev/null || true

    # tun2socks si no está en repos
    if ! command -v tun2socks &>/dev/null; then
        warn "tun2socks no en repos — descargando..."
        local ARCH; ARCH=$(uname -m)
        local URL
        case "$ARCH" in
            x86_64)  URL="https://github.com/xjasonlyu/tun2socks/releases/latest/download/tun2socks-linux-amd64.zip" ;;
            aarch64) URL="https://github.com/xjasonlyu/tun2socks/releases/latest/download/tun2socks-linux-arm64.zip" ;;
            *) error "Arquitectura no soportada: $ARCH"; exit 1 ;;
        esac
        cd /tmp
        wget -q "$URL" -O tun2socks.zip && unzip -qo tun2socks.zip
        find /tmp -maxdepth 1 -name "tun2socks*" -type f -exec mv {} /usr/local/bin/tun2socks \; 2>/dev/null || true
        chmod +x /usr/local/bin/tun2socks 2>/dev/null || true
        cd - > /dev/null
    fi
    info "tun2socks listo"
}

install_xray() {
    if [ -f "$XRAY_BIN" ]; then info "Xray ya instalado."; return; fi
    info "Instalando Xray..."
    bash -c "$(curl -fsSL https://github.com/XTLS/Xray-install/raw/main/install-release.sh)" @ install
    mkdir -p "$INSTALL_DIR"
    cp /usr/local/bin/xray "$XRAY_BIN" 2>/dev/null || cp /usr/bin/xray "$XRAY_BIN" 2>/dev/null || true
    chmod +x "$XRAY_BIN"
    info "Xray listo"
}

create_btclient() {
    mkdir -p "$INSTALL_DIR"
    cat > "$CLIENT_PY" << 'PYEOF'
#!/usr/bin/env python3
import asyncio, struct, time, sys
from pathlib import Path

PROXY_HOST  = "emailmarketing.personal.com.ar"
PROXY_IPV6  = "2606:4700::6812:16b7"
PROXY_PORT  = 80
TUNNEL_HOST = "1.brawlpass.com.ar"
XRAY_HOST   = "127.0.0.1"
XRAY_PORT   = 10809
TYPE_OPEN   = 0x01
TYPE_DATA   = 0x02
TYPE_CLOSE  = 0x03

def load_client_id():
    for line in Path("/opt/xtunnel/config.conf").read_text().splitlines():
        if line.startswith("CLIENT_ID="):
            return line.split("=",1)[1].strip().strip('"')
    return None

async def open_tunnel(client_id):
    import socket as s
    addrs = []
    try: addrs += [(a[4], s.AF_INET6) for a in s.getaddrinfo(PROXY_IPV6, PROXY_PORT, s.AF_INET6, s.SOCK_STREAM)]
    except: pass
    try: addrs += [(a[4], a[0]) for a in s.getaddrinfo(PROXY_HOST, PROXY_PORT, s.AF_UNSPEC, s.SOCK_STREAM)]
    except: pass
    for addr, family in addrs:
        try:
            sock = s.socket(family, s.SOCK_STREAM)
            sock.settimeout(10); sock.connect(addr); sock.settimeout(None)
            sock.setsockopt(s.IPPROTO_TCP, s.TCP_NODELAY, 1)
            sock.sendall(f"GET / HTTP/1.1\r\nHost: {PROXY_HOST}\r\n\r\n".encode())
            await asyncio.sleep(0.01)
            t0 = time.time()
            sock.sendall((f"- / HTTP/1.1\r\nHost: {TUNNEL_HOST}\r\nUpgrade: websocket\r\n"
                f"Action: tunnel\r\nX-Client-Id: {client_id}\r\n\r\n").encode())
            sock.settimeout(8)
            raw = b""
            while b"\r\n\r\n" not in raw:
                c = sock.recv(4096)
                if not c: break
                raw += c
            sock.settimeout(None)
            if b"101" not in raw[:20]:
                r = raw.decode(errors="replace")
                if "EXPIRED" in r: print("[✗] Expirado"); return None, True
                if "UNKNOWN" in r or "INVALID" in r: print("[✗] ID no registrado en el servidor"); return None, True
                sock.close(); continue
            ms = int((time.time()-t0)*1000)
            print(f"[✓] Túnel establecido · {ms}ms", flush=True)
            rd, wr = await asyncio.open_connection(sock=sock)
            return (rd, wr), False
        except Exception as e:
            try: sock.close()
            except: pass
            print(f"[!] {e}", flush=True)
    return None, False

async def handle_mux(reader, writer):
    streams = {}; lock = asyncio.Lock(); buf = bytearray()
    async def send(t, sid, data=b""):
        async with lock:
            writer.write(struct.pack("!B I I", t, sid, len(data)) + data)
            await writer.drain()
    async def pipe(sid, xr):
        try:
            while True:
                d = await xr.read(65536)
                if not d: break
                await send(TYPE_DATA, sid, d)
        except: pass
        await send(TYPE_CLOSE, sid); streams.pop(sid, None)
    async def read_n(n):
        nonlocal buf
        while len(buf) < n:
            c = await reader.read(65536)
            if not c: raise ConnectionError()
            buf.extend(c)
        d, buf[:] = bytes(buf[:n]), buf[n:]; return d
    async def keepalive():
        while True:
            await asyncio.sleep(45)
            try: await send(TYPE_DATA, 0)
            except: break
    asyncio.get_event_loop().create_task(keepalive())
    try:
        while True:
            t, sid, l = struct.unpack("!B I I", await read_n(9))
            data = await read_n(l) if l else b""
            if t == TYPE_OPEN:
                try:
                    xr, xw = await asyncio.open_connection(XRAY_HOST, XRAY_PORT)
                    streams[sid] = (xr, xw)
                    asyncio.get_event_loop().create_task(pipe(sid, xr))
                except: await send(TYPE_CLOSE, sid)
            elif t == TYPE_DATA:
                p = streams.get(sid)
                if p:
                    try: p[1].write(data); await p[1].drain()
                    except: await send(TYPE_CLOSE, sid); streams.pop(sid, None)
            elif t == TYPE_CLOSE:
                p = streams.pop(sid, None)
                if p:
                    try: p[1].close(); await p[1].wait_closed()
                    except: pass
    except: pass
    finally:
        for _,(_, xw) in list(streams.items()):
            try: xw.close()
            except: pass
        try: writer.close()
        except: pass

async def main():
    cid = load_client_id()
    if not cid: print("[✗] Sin CLIENT_ID"); sys.exit(1)
    attempt = 0
    while True:
        if attempt > 0: print(f"[→] Reintentando ({attempt})...", flush=True)
        conn, fatal = await open_tunnel(cid)
        if fatal: sys.exit(1)
        if conn:
            attempt = 0
            print("[→] Tráfico activo — Ctrl+C para detener", flush=True)
            await handle_mux(*conn)
            print("[!] Conexión caída", flush=True)
        attempt += 1
        await asyncio.sleep(min(attempt*6, 30))

asyncio.run(main())
PYEOF
    chmod +x "$CLIENT_PY"
}

create_xray_config() {
    cat > "$INSTALL_DIR/xray-client.json" << EOF
{
  "log": { "loglevel": "none" },
  "dns": {
    "servers": ["fakedns",
      {"address":"8.8.8.8","queryStrategy":"UseIPv4"},
      {"address":"1.1.1.1","queryStrategy":"UseIPv4"}],
    "queryStrategy": "UseIPv4"
  },
  "fakedns": [{"ipPool":"198.18.0.0/15","poolSize":65535}],
  "inbounds": [{
    "protocol": "socks", "listen": "127.0.0.1", "port": $SOCKS_PORT,
    "settings": {"udp": true},
    "sniffing": {"enabled":true,"destOverride":["http","tls","quic","fakedns"],"metadataOnly":false}
  }],
  "outbounds": [{
    "protocol": "vless",
    "settings": {"vnext":[{"address":"127.0.0.1","port":$TUNNEL_PORT,
      "users":[{"id":"$VLESS_UUID","encryption":"none"}]}]},
    "streamSettings": {"network":"tcp","security":"none"},
    "mux": {"enabled":true,"concurrency":128,"xudpConcurrency":1024,"xudpProxyUDP443":"allow"}
  }]
}
EOF
}

setup_tun() {
    info "Configurando TUN..."
    sysctl -w net.ipv4.ip_forward=1 -q
    ip tuntap del dev tun0 mode tun 2>/dev/null || true
    ip tuntap add dev tun0 mode tun
    ip addr add 198.18.0.1/15 dev tun0
    ip link set tun0 up

    # Detectar interfaz de salida (USB del celular)
    local USB
    USB=$(ip route | awk '/default/{print $5}' | head -1)
    local GW
    GW=$(ip route | awk '/default/{print $3}' | head -1)

    # Rutas protegidas para el túnel (van por USB directo)
    ip route add 2606:4700::6812:16b7 via "$GW" dev "$USB" 2>/dev/null || true
    ip route add "$(dig +short emailmarketing.personal.com.ar | head -1)" via "$GW" dev "$USB" 2>/dev/null || true

    # Todo lo demás por tun0
    ip route add default dev tun0 metric 1 2>/dev/null || true

    info "TUN listo"
}

start_all() {
    load_config
    create_xray_config

    info "Iniciando xray..."
    pkill -f "xray run" 2>/dev/null || true
    "$XRAY_BIN" run -c "$INSTALL_DIR/xray-client.json" > /var/log/xtunnel-xray.log 2>&1 &
    sleep 1

    info "Iniciando tun2socks..."
    pkill -f tun2socks 2>/dev/null || true
    tun2socks -device tun0 -proxy "socks5://127.0.0.1:$SOCKS_PORT" > /var/log/xtunnel-tun2socks.log 2>&1 &
    sleep 1

    setup_tun

    echo ""
    echo -e "${CYAN}${BOLD}══════════════════════════════════${NC}"
    echo -e "${CYAN}${BOLD}  Conectando al servidor...${NC}"
    echo -e "${CYAN}${BOLD}══════════════════════════════════${NC}"
    echo ""

    # btclient en primer plano para ver el log
    python3 "$CLIENT_PY"
}

stop_all() {
    pkill -f btclient 2>/dev/null || true
    pkill -f "xray run" 2>/dev/null || true
    pkill -f tun2socks 2>/dev/null || true
    ip tuntap del dev tun0 mode tun 2>/dev/null || true
    ip route del default dev tun0 2>/dev/null || true
    info "Detenido."
}

# ── Main ──────────────────────────────────────────────────────────────────────
clear
echo -e "${CYAN}${BOLD}"
echo "  ╔══════════════════════════╗"
echo "  ║   XTunnel — Prueba PC   ║"
echo "  ╚══════════════════════════╝"
echo -e "${NC}"

# Primera vez — instalar
if [ ! -f "$CLIENT_PY" ] || [ ! -f "$XRAY_BIN" ]; then
    header "Primera ejecución — instalando..."
    install_deps
    install_xray
    create_btclient

    # Generar ID
    CLIENT_ID=$(generate_id)

    echo ""
    echo -e "${YELLOW}${BOLD}  ╔══════════════════════════════╗${NC}"
    echo -e "${YELLOW}${BOLD}  ║  ID de este cliente:         ║${NC}"
    echo -e "${YELLOW}${BOLD}  ║  ${CLIENT_ID}              ║${NC}"
    echo -e "${YELLOW}${BOLD}  ╚══════════════════════════════╝${NC}"
    echo ""
    echo -e "  Registralo en el panel antes de continuar:"
    echo -e "  ${CYAN}POST /client/create${NC}"
    echo -e "  ${CYAN}{ \"id\": \"$CLIENT_ID\", \"name\": \"pc-kiosco\", \"days\": 30 }${NC}"
    echo ""

    echo -e "${CYAN}URL del panel (ej: http://IP:8090):${NC}"
    read -r PANEL_URL
    echo -e "${CYAN}Token:${NC}"
    read -r PANEL_TOKEN

    # Registrar automáticamente
    R=$(curl -s -X POST "$PANEL_URL/client/create" \
        -H "X-Token: $PANEL_TOKEN" \
        -H "Content-Type: application/json" \
        -d "{\"id\":\"$CLIENT_ID\",\"name\":\"pc-kiosco\",\"days\":30}" 2>/dev/null || echo "")

    if echo "$R" | grep -q '"ok":true'; then
        info "Registrado correctamente en el servidor."
    else
        warn "No se pudo registrar automáticamente."
        warn "Registralo manualmente con el ID: $CLIENT_ID"
        echo -n "  Presioná ENTER cuando lo hayas registrado..."
        read -r
    fi

    save_config
    echo ""
fi

load_config

# Menú simple
echo -e "  ID: ${CYAN}${BOLD}$CLIENT_ID${NC}"
echo ""
echo -e "  ${BOLD}[1]${NC} Conectar"
echo -e "  ${BOLD}[2]${NC} Detener"
echo -e "  ${BOLD}[3]${NC} Salir"
echo ""
echo -n "  Opción: "; read -r OPT

case "$OPT" in
    1) start_all ;;
    2) stop_all ;;
    3) exit 0 ;;
    *) warn "Opción inválida" ;;
esac
