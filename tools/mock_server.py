#!/usr/bin/env python3
"""Mock ESP32 DJ for testing the web UI on a PC (no hardware).

    pip install aiohttp
    python3 tools/mock_server.py --music /path/to/mp3s --port 8080

Speaks the same WebSocket text protocol and HTTP API as the firmware and
simulates the transport (positions, loops, sync, auto-DJ is not simulated).
"""
import argparse, asyncio, json, math, os, pathlib, random, time
from aiohttp import web

ap = argparse.ArgumentParser()
ap.add_argument("--music", default=".")
ap.add_argument("--port", type=int, default=8080)
args = ap.parse_args()
ROOT = pathlib.Path(__file__).resolve().parent.parent
MUSIC = pathlib.Path(args.music).resolve()


def lib():
    out = []
    for p in sorted(MUSIC.rglob("*.mp3")):
        rel = "/music/" + str(p.relative_to(MUSIC)).replace(os.sep, "/")
        out.append([rel, round(random.uniform(118, 130), 1)])
    return out


LIB = lib()


def new_deck(i):
    return dict(st=0, ti="", ar="", p="", pos=0.0, dur=0.0, pl=False, cp=False, bpm=0.0, eb=0.0, pi=0.0, pf=0.0, rg=8,
                sy=False, lo=False, ls=0.0, le=0.0, lb=0.0, hc=[-1, -1, -1, -1], cue=0.0, fb=0.0, pfl=False, an=False,
                ex=True, ph=0.0, sp=0.0, end=False, vol=0.8, tr=1.0, eql=1.0, eqm=1.0, eqh=1.0, fl=0.0, vu=0.0, id=i)


S = dict(d=[new_deck(0), new_deck(1)], xf=0.5, mv=0.8, hm=0.3, hv=0.7, pv=0.7, vum=0.0, cpu=23,
         fx=dict(y=1, t=2, a=0.5, b=0.5, on=False), adj=False, qz=True, o1=True, o2=False, lv=1, nq=0, heap=180)
QUEUE = []
SETTINGS = dict(ssid="", pass_="", apPass="djmix1234", hostname="esp32dj", sdCs=42, sdSck=41, sdMosi=40, sdMiso=39,
                sdMHz=20, i2sBck=4, i2sWs=5, i2sDout=6, dac2=False, i2s2Bck=47, i2s2Ws=48, i2s2Dout=33,
                splitCue=False, xfCurve=0, fadeBeats=16, pitchRange=8, musicDir="/")
scr = [dict(on=False, v=0.0), dict(on=False, v=0.0)]
nudge = [0.0, 0.0]


def real(path):
    if path.startswith("/music/"):
        return MUSIC / path[len("/music/"):]
    return MUSIC / path.lstrip("/")


def mp3_duration(p):
    try:
        from mutagen.mp3 import MP3  # optional
        return MP3(p).info.length
    except Exception:
        return max(30.0, os.path.getsize(p) / 24000)


def tempo(k):
    return 1 + k["pi"] / 100


def beat_phase(k):
    if k["bpm"] <= 0:
        return 0
    b = 60 / k["bpm"]
    return ((k["pos"] - k["fb"]) / b) % 1


def command(line):
    t = line.split(" ")
    c = t[0]
    g = float(t[1]) if len(t) > 1 and c not in ("q+",) and t[1].replace(".", "", 1).replace("-", "", 1).isdigit() else 0
    if c == "xf": S["xf"] = g; return
    if c == "mvol": S["mv"] = g; return
    if c == "hpmix": S["hm"] = g; return
    if c == "hpvol": S["hv"] = g; return
    if c == "padvol": S["pv"] = g; return
    if c == "pad": return
    if c == "fx": S["fx"]["y"] = int(g); return
    if c == "fxt": S["fx"]["t"] = int(g); return
    if c == "fxa": S["fx"]["a"] = g; return
    if c == "fxb": S["fx"]["b"] = g; return
    if c == "fxon": S["fx"]["on"] = g != 0; return
    if c == "adj": S["adj"] = g != 0; return
    if c == "quant": S["qz"] = g != 0; return
    if c == "q+": QUEUE.append(line[3:]); S["nq"] = len(QUEUE); S["lv"] += 1; return
    if c == "q-":
        i = int(g)
        if 0 <= i < len(QUEUE): QUEUE.pop(i)
        S["nq"] = len(QUEUE); S["lv"] += 1; return
    if c == "qc": QUEUE.clear(); S["nq"] = 0; S["lv"] += 1; return
    if c == "rescan": return
    d = int(t[1]); k = S["d"][d]
    v = float(t[2]) if len(t) > 2 and c not in ("load",) else 0
    if c == "load":
        p = line.split(" ", 2)[2]
        k.update(new_deck(d)); k["st"] = 2; k["p"] = p; k["ti"] = pathlib.Path(p).stem; k["ar"] = "Mock Artist"
        k["dur"] = mp3_duration(real(p)); k["bpm"] = next((b for q, b in LIB if q == p), 124.0); k["eb"] = k["bpm"]
        k["fb"] = 0.12
    elif c == "eject": k.update(new_deck(d))
    elif c == "play":
        k["pl"] = (not k["pl"]) if len(t) < 3 else v != 0; k["cp"] = False
    elif c == "cue":
        if v:
            if k["pl"] and not k["cp"]: k["pl"] = False; k["pos"] = k["cue"]
            else:
                if abs(k["pos"] - k["cue"]) > 0.02: k["cue"] = k["pos"]
                k["cp"] = True; k["pl"] = True
        elif k["cp"]:
            k["cp"] = False; k["pl"] = False; k["pos"] = k["cue"]
    elif c == "sync":
        k["sy"] = not k["sy"]
    elif c == "vol": k["vol"] = v
    elif c == "trim": k["tr"] = v
    elif c == "lo": k["eql"] = v
    elif c == "mid": k["eqm"] = v
    elif c == "hi": k["eqh"] = v
    elif c == "flt": k["fl"] = v
    elif c == "pfl": k["pfl"] = v != 0
    elif c == "pitch": k["pf"] = v; k["sy"] = False
    elif c == "range": k["rg"] = int(v)
    elif c == "jog": nudge[d] = v
    elif c == "scr": scr[d]["on"] = v != 0; scr[d]["v"] = 0
    elif c == "sv": scr[d]["v"] = v
    elif c == "seek": k["pos"] = v * k["dur"]
    elif c == "hc":
        i = int(v)
        if k["hc"][i] < 0: k["hc"][i] = round(k["pos"], 3)
        else: k["pos"] = k["hc"][i]
    elif c == "hcdel": k["hc"][int(v)] = -1
    elif c == "loop":
        b = 60 / (k["bpm"] or 120)
        k["lo"] = True; k["lb"] = v; k["ls"] = k["pos"]; k["le"] = k["pos"] + v * b
    elif c == "loopx": k["lo"] = False
    elif c in ("loophalf", "loopdbl") and k["lo"]:
        k["lb"] = k["lb"] / 2 if c == "loophalf" else k["lb"] * 2
        k["le"] = k["ls"] + k["lb"] * 60 / (k["bpm"] or 120)
    elif c == "jump": k["pos"] = max(0, k["pos"] + v * 60 / (k["bpm"] or 120))
    elif c == "brake": k["pl"] = False
    elif c == "bpmx": k["bpm"] *= v
    elif c == "grid": k["fb"] = k["pos"] % (60 / (k["bpm"] or 120))


async def ticker(app):
    last = time.time()
    while True:
        await asyncio.sleep(0.1)
        now = time.time(); dt = now - last; last = now
        for d, k in enumerate(S["d"]):
            o = S["d"][1 - d]
            if k["sy"] and o["bpm"] > 0 and k["bpm"] > 0:
                k["pi"] = (o["eb"] / k["bpm"] - 1) * 100
            else:
                k["pi"] = k["pf"] * k["rg"]
            k["eb"] = k["bpm"] * tempo(k)
            sp = scr[d]["v"] if scr[d]["on"] else (tempo(k) + nudge[d] if k["pl"] else 0)
            k["sp"] = sp
            k["pos"] = max(0, k["pos"] + sp * dt)
            if k["lo"] and k["pos"] >= k["le"]: k["pos"] -= (k["le"] - k["ls"])
            if k["dur"] and k["pos"] >= k["dur"]: k["pos"] = k["dur"]; k["pl"] = False; k["end"] = True
            k["ph"] = beat_phase(k)
            k["vu"] = (0.5 + 0.4 * math.sin(now * 9 + d)) * k["vol"] if k["pl"] else 0
        S["vum"] = max(S["d"][0]["vu"], S["d"][1]["vu"]) * S["mv"]
        msg = json.dumps(S)
        for ws in list(app["clients"]):
            try:
                await ws.send_str(msg)
            except Exception:
                app["clients"].discard(ws)


async def h_ws(req):
    ws = web.WebSocketResponse()
    await ws.prepare(req)
    req.app["clients"].add(ws)
    async for m in ws:
        if m.type == web.WSMsgType.TEXT:
            for line in m.data.split("\n"):
                if line.strip():
                    try: command(line.strip())
                    except Exception as e: print("bad cmd", line, e)
    req.app["clients"].discard(ws)
    return ws


async def h_index(req):
    return web.FileResponse(ROOT / "web" / "index.html")


async def h_lib(req):
    return web.json_response(dict(v=S["lv"], q=QUEUE, t=LIB))


async def h_file(req):
    return web.FileResponse(real(req.query["path"]))


async def h_ls(req):
    d = req.query.get("dir", "/")
    base = real(d) if d.startswith("/music") else MUSIC
    e = []
    for p in sorted(base.iterdir()):
        e.append(dict(n=p.name, d=1) if p.is_dir() else dict(n=p.name, s=p.stat().st_size))
    return web.json_response(dict(dir=d, e=e))


async def h_info(req):
    return web.json_response(dict(version="1.0.0-mock", board="Mock board", psram=2048, heap=180, minHeap=120, sdTotal=30000,
                                  sdUsed=1234, sdOk=True, mode="hotspot", ip="192.168.4.1", ssid="ESP32-DJ-MOCK",
                                  tracks=len(LIB), pads=[]))


async def h_settings(req):
    if req.method == "POST":
        data = await req.post()
        print("settings:", dict(data))
        return web.json_response(dict(ok=True))
    s = dict(SETTINGS); s["pass"] = s.pop("pass_")
    return web.json_response(s)


async def h_ok(req):
    await req.read()
    return web.json_response(dict(ok=True))


app = web.Application(client_max_size=200 * 1024 * 1024)
app["clients"] = set()
app.router.add_get("/", h_index)
app.router.add_get("/ws", h_ws)
app.router.add_get("/api/lib", h_lib)
app.router.add_get("/api/file", h_file)
app.router.add_get("/api/ls", h_ls)
app.router.add_get("/api/info", h_info)
app.router.add_get("/api/settings", h_settings)
app.router.add_post("/api/settings", h_settings)
for p in ("/api/upload", "/api/rm", "/api/mkdir", "/api/reboot", "/api/ota"):
    app.router.add_post(p, h_ok)


async def start(app):
    app["t"] = asyncio.create_task(ticker(app))
app.on_startup.append(start)
web.run_app(app, port=args.port)
